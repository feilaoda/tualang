#include "tua_map.h"

#include <ctype.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt/rt_alloc.h"
#include "rt/rt_config.h"
#include "tua_str.h"

		// Value tags are defined in `tua_map.h` (ABI-stable for external FFI helpers).

static uint32_t map_kind_any(const void* p) {
    if (!p) return 0;
    uint32_t k = 0;
    memcpy(&k, p, sizeof(k));
    return k;
}

static int32_t imap_value_tag_any(const void* p) {
    if (!p) return 0;
    int32_t t = 0;
    const uint8_t* b = (const uint8_t*)p;
    memcpy(&t, b + sizeof(uint32_t), sizeof(t));
    return t;
}

typedef enum {
    KEY_EMPTY = 0,
    KEY_INT = 1,
    KEY_STRING = 2,
    KEY_TOMBSTONE = 3
} KeyKind;

typedef struct {
    KeyKind kind;
    uint32_t hash;
    union {
        int64_t i;
        char* s; // points to a Tua string (`char*` with optional managed header)
    } k;
    tua_value v;
} MapEntry;

struct tua_map {
    uint32_t kind; // TUA_MAP_KIND_*
    uint32_t _pad;
    size_t capacity;
    size_t count;
    size_t tombstones;
    MapEntry* entries;
};

static const char* tua_current_file = NULL;
static int32_t tua_current_line = 0;
static int32_t tua_current_col = 0;

void tua_set_line(int32_t line) {
    tua_current_line = line;
}

void tua_set_loc(const char* file, int32_t line, int32_t col) {
    if (file) tua_current_file = file;
    tua_current_line = line;
    tua_current_col = col;
}

void tua_panic(const char* msg) {
    const char* file = tua_current_file;
    int32_t line = tua_current_line;
    int32_t col = tua_current_col;

    char buf[1024];
    const char* m = msg ? msg : "(null)";
    if (file && line > 0 && col > 0) {
        snprintf(buf, sizeof(buf), "%s:%d:%d: error: %s\n", file, (int)line, (int)col, m);
    } else if (file && line > 0) {
        snprintf(buf, sizeof(buf), "%s:%d: error: %s\n", file, (int)line, m);
    } else if (line > 0 && col > 0) {
        snprintf(buf, sizeof(buf), "error:%d:%d: %s\n", (int)line, (int)col, m);
    } else if (line > 0) {
        snprintf(buf, sizeof(buf), "error:%d: %s\n", (int)line, m);
    } else {
        snprintf(buf, sizeof(buf), "error: %s\n", m);
    }

    tua_config cfg = tua_rt_get_config();
    if (cfg.panic) {
        cfg.panic(cfg.panic_ud, buf);
    } else {
        fputs(buf, stderr);
        fflush(stderr);
    }
    exit(1);
}

static uint32_t hash_u64(uint64_t x) {
    // 64-bit mix then fold to 32-bit.
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return (uint32_t)(x ^ (x >> 32));
}

static uint32_t hash_u32(uint32_t x) {
    // 32-bit mix (same as bench/c/bench.c).
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x ? x : 1u;
}

static uint32_t hash_i64(int64_t v) {
    // Fast path for small / i32-range keys (common for dense key spaces).
    if (v >= (int64_t)INT32_MIN && v <= (int64_t)INT32_MAX) {
        return hash_u32((uint32_t)(int32_t)v);
    }
    return hash_u64((uint64_t)v);
}

static int key_equals(const MapEntry* e, KeyKind kind, int64_t ikey, const char* skey) {
    if (!e || e->kind != kind) return 0;
    if (kind == KEY_INT) return e->k.i == ikey;
    if (kind == KEY_STRING) return tua_str_eq(e->k.s, skey) == 1;
    return 0;
}

static size_t find_slot(tua_map* m, KeyKind kind, uint32_t hash, int64_t ikey, const char* skey, int* found) {
    if (!m || m->capacity == 0) tua_panic("invalid map");
    size_t mask = m->capacity - 1;
    size_t i = (size_t)hash & mask;
    for (;;) {
        MapEntry* e = &m->entries[i];
        if (e->kind == KEY_EMPTY) {
            if (found) *found = 0;
            return i;
        }
        if (e->kind != KEY_TOMBSTONE) {
            if (kind == KEY_INT) {
                // For int keys, key compare is cheap; avoid loading/comparing stored hash.
                if (e->kind == KEY_INT && e->k.i == ikey) {
                    if (found) *found = 1;
                    return i;
                }
            } else {
                // For string keys, hash compare avoids expensive strcmp most of the time.
                if (e->hash == hash && key_equals(e, kind, ikey, skey)) {
                    if (found) *found = 1;
                    return i;
                }
            }
        }
        i = (i + 1) & mask;
    }
}

static size_t find_slot_for_insert(tua_map* m, KeyKind kind, uint32_t hash, int64_t ikey, const char* skey, int* found) {
    if (!m || m->capacity == 0) tua_panic("invalid map");
    size_t mask = m->capacity - 1;
    size_t i = (size_t)hash & mask;
    size_t firstTombstone = (size_t)(-1);
    for (;;) {
        MapEntry* e = &m->entries[i];
        if (e->kind == KEY_EMPTY) {
            if (found) *found = 0;
            return firstTombstone != (size_t)(-1) ? firstTombstone : i;
        }
        if (e->kind == KEY_TOMBSTONE) {
            if (firstTombstone == (size_t)(-1)) firstTombstone = i;
        } else {
            if (kind == KEY_INT) {
                if (e->kind == KEY_INT && e->k.i == ikey) {
                    if (found) *found = 1;
                    return i;
                }
            } else {
                if (e->hash == hash && key_equals(e, kind, ikey, skey)) {
                    if (found) *found = 1;
                    return i;
                }
            }
        }
        i = (i + 1) & mask;
    }
}

static void map_rehash(tua_map* m, size_t newCap) {
    MapEntry* old = m->entries;
    size_t oldCap = m->capacity;

    MapEntry* entries = (MapEntry*)tua_calloc(newCap, sizeof(MapEntry));
    if (!entries) tua_panic("out of memory");

    m->entries = entries;
    m->capacity = newCap;
    m->count = 0;
    m->tombstones = 0;

    for (size_t i = 0; i < oldCap; i++) {
        MapEntry* e = &old[i];
        if (e->kind == KEY_EMPTY || e->kind == KEY_TOMBSTONE) continue;
        int found = 0;
        if (e->kind == KEY_INT) {
            size_t slot = find_slot_for_insert(m, KEY_INT, e->hash, e->k.i, NULL, &found);
            m->entries[slot] = *e;
        } else if (e->kind == KEY_STRING) {
            size_t slot = find_slot_for_insert(m, KEY_STRING, e->hash, 0, e->k.s, &found);
            m->entries[slot] = *e;
        }
        (void)found;
        m->count++;
    }

        tua_free(old);
}

static void ensure_capacity(tua_map* m) {
    if (!m) tua_panic("invalid map");
    if (m->capacity == 0) {
        m->capacity = 16;
        m->entries = (MapEntry*)tua_calloc(m->capacity, sizeof(MapEntry));
        if (!m->entries) tua_panic("out of memory");
        return;
    }
    // Keep enough empty slots so lookups always terminate (need KEY_EMPTY sentinel).
    size_t used = m->count + m->tombstones;

    // load factor ~0.75 based on used slots (count + tombstones)
    if ((used + 1) * 4 < m->capacity * 3) return;

    // If table is cluttered with tombstones, rehash in-place to clean it up.
    if (m->tombstones > m->count) {
        map_rehash(m, m->capacity);
        return;
    }

    map_rehash(m, m->capacity * 2);
}

static void decode_key(tua_value key, KeyKind* kind, uint32_t* hash, int64_t* ikey, const char** skey) {
    if (!kind || !hash) tua_panic("invalid key decode");
    *kind = KEY_EMPTY;
    *hash = 0;
    if (ikey) *ikey = 0;
    if (skey) *skey = NULL;

    if (key.tag == TUA_VAL_LONG || key.tag == TUA_VAL_INT) {
        // Normalize int/long keys to int64 domain.
        int64_t v = (int64_t)key.payload;
        *kind = KEY_INT;
        *hash = hash_i64(v);
        if (ikey) *ikey = v;
        return;
    }
    if (key.tag == TUA_VAL_STRING) {
        const char* s = (const char*)(uintptr_t)key.payload;
        if (!s) {
            // Treat null string key as empty string key.
            s = "";
        }
        *kind = KEY_STRING;
        *hash = tua_str_hash32(s);
        if (skey) *skey = s;
        return;
    }

    tua_panic("map key must be int/long/string");
}

tua_map* tua_map_new(void) {
    tua_map* m = (tua_map*)tua_calloc(1, sizeof(tua_map));
    if (!m) tua_panic("out of memory");
    m->kind = (uint32_t)TUA_MAP_KIND_GENERIC;
    m->capacity = 0;
    m->count = 0;
    m->tombstones = 0;
    m->entries = NULL;
    ensure_capacity(m);
    return m;
}

tua_value tua_map_get(tua_map* map, tua_value key) {
    if (!map) tua_panic("index null map");
    if (map_kind_any(map) == (uint32_t)TUA_MAP_KIND_IMAP) {
        int64_t k = 0;
        if (key.tag == TUA_VAL_LONG) k = (int64_t)key.payload;
        else if (key.tag == TUA_VAL_INT) k = (int64_t)(int32_t)key.payload;
        else tua_panic("map key must be int/long");
        int32_t ok = 0;
        uint64_t payload = tua_imap_get_payload_with_ok(map, k, &ok);
        if (!ok) return (tua_value){ .tag = TUA_VAL_NIL, .payload = 0 };
        return (tua_value){ .tag = imap_value_tag_any(map), .payload = payload };
    }
    KeyKind kind;
    uint32_t hash;
    int64_t ikey = 0;
    const char* skey = NULL;
    decode_key(key, &kind, &hash, &ikey, &skey);

    int found = 0;
    size_t slot = find_slot(map, kind, hash, ikey, skey, &found);
    if (!found) return (tua_value){ .tag = TUA_VAL_NIL, .payload = 0 };
    return map->entries[slot].v;
}

tua_value tua_map_get_with_ok(tua_map* map, tua_value key, int32_t* outOk) {
    if (!map) tua_panic("index null map");
    if (!outOk) tua_panic("invalid outOk");
    if (map_kind_any(map) == (uint32_t)TUA_MAP_KIND_IMAP) {
        int64_t k = 0;
        if (key.tag == TUA_VAL_LONG) k = (int64_t)key.payload;
        else if (key.tag == TUA_VAL_INT) k = (int64_t)(int32_t)key.payload;
        else tua_panic("map key must be int/long");
        uint64_t payload = tua_imap_get_payload_with_ok(map, k, outOk);
        if (!*outOk) return (tua_value){ .tag = TUA_VAL_NIL, .payload = 0 };
        return (tua_value){ .tag = imap_value_tag_any(map), .payload = payload };
    }
    KeyKind kind;
    uint32_t hash;
    int64_t ikey = 0;
    const char* skey = NULL;
    decode_key(key, &kind, &hash, &ikey, &skey);

    int found = 0;
    size_t slot = find_slot(map, kind, hash, ikey, skey, &found);
    *outOk = found ? 1 : 0;
    if (!found) return (tua_value){ .tag = TUA_VAL_NIL, .payload = 0 };
    return map->entries[slot].v;
}

tua_value* tua_map_get_ref_with_ok(tua_map* map, tua_value key, int32_t* outOk) {
    if (!map) tua_panic("index null map");
    if (!outOk) tua_panic("invalid outOk");
    if (map_kind_any(map) == (uint32_t)TUA_MAP_KIND_IMAP) {
        tua_panic("map.get/getMut is not supported for scalar typed maps");
        return NULL;
    }
    KeyKind kind;
    uint32_t hash;
    int64_t ikey = 0;
    const char* skey = NULL;
    decode_key(key, &kind, &hash, &ikey, &skey);

    int found = 0;
    size_t slot = find_slot(map, kind, hash, ikey, skey, &found);
    *outOk = found ? 1 : 0;
    if (!found) return NULL;
    return &map->entries[slot].v;
}

void tua_map_set(tua_map* map, tua_value key, tua_value value) {
    if (!map) tua_panic("assign into null map");
    if (map_kind_any(map) == (uint32_t)TUA_MAP_KIND_IMAP) {
        int64_t k = 0;
        if (key.tag == TUA_VAL_LONG) k = (int64_t)key.payload;
        else if (key.tag == TUA_VAL_INT) k = (int64_t)(int32_t)key.payload;
        else tua_panic("map key must be int/long");
        int32_t wantTag = imap_value_tag_any(map);
        if (value.tag != wantTag) tua_panic("typed map value tag mismatch");
        tua_imap_set_payload(map, k, value.payload);
        return;
    }
    ensure_capacity(map);

    KeyKind kind;
    uint32_t hash;
    int64_t ikey = 0;
    const char* skey = NULL;
    decode_key(key, &kind, &hash, &ikey, &skey);

    int found = 0;
    size_t slot = find_slot_for_insert(map, kind, hash, ikey, skey, &found);
    MapEntry* e = &map->entries[slot];

    if (!found) {
        if (e->kind == KEY_TOMBSTONE) {
            map->tombstones--;
        }
        e->kind = kind;
        e->hash = hash;
        if (kind == KEY_INT) {
            e->k.i = ikey;
        } else if (kind == KEY_STRING) {
            e->k.s = (char*)(skey ? skey : "");
            tua_str_retain(e->k.s);
        }
        map->count++;
        e->v.tag = TUA_VAL_NIL;
        e->v.payload = 0;
    }

    // Overwrite semantics: later entries win.
    if (e->v.tag == TUA_VAL_STRING) tua_str_release((const char*)(uintptr_t)e->v.payload);
    if (value.tag == TUA_VAL_STRING) tua_str_retain((const char*)(uintptr_t)value.payload);
    e->v = value;
}

int32_t tua_map_delete(tua_map* map, tua_value key) {
    if (!map) tua_panic("index null map");
    if (map_kind_any(map) == (uint32_t)TUA_MAP_KIND_IMAP) {
        int64_t k = 0;
        if (key.tag == TUA_VAL_LONG) k = (int64_t)key.payload;
        else if (key.tag == TUA_VAL_INT) k = (int64_t)(int32_t)key.payload;
        else tua_panic("map key must be int/long");
        return tua_imap_delete(map, k);
    }
    KeyKind kind;
    uint32_t hash;
    int64_t ikey = 0;
    const char* skey = NULL;
    decode_key(key, &kind, &hash, &ikey, &skey);

    int found = 0;
    size_t slot = find_slot(map, kind, hash, ikey, skey, &found);
    if (!found) return 0;

    MapEntry* e = &map->entries[slot];
    if (e->kind == KEY_STRING) tua_str_release(e->k.s);
    if (e->v.tag == TUA_VAL_STRING) tua_str_release((const char*)(uintptr_t)e->v.payload);

    e->kind = KEY_TOMBSTONE;
    e->hash = 0;
    e->k.i = 0;
    e->v.tag = TUA_VAL_NIL;
    e->v.payload = 0;

    map->count--;
    map->tombstones++;
    return 1;
}

void tua_map_clear(tua_map* map) {
    if (!map) tua_panic("index null map");
    if (map_kind_any(map) == (uint32_t)TUA_MAP_KIND_IMAP) {
        tua_imap_clear(map);
        return;
    }
    if (!map->entries || map->capacity == 0) return;
    for (size_t i = 0; i < map->capacity; i++) {
        MapEntry* e = &map->entries[i];
        if (e->kind == KEY_STRING) tua_str_release(e->k.s);
        if (e->v.tag == TUA_VAL_STRING) tua_str_release((const char*)(uintptr_t)e->v.payload);
        e->kind = KEY_EMPTY;
        e->hash = 0;
        e->k.i = 0;
        e->v.tag = TUA_VAL_NIL;
        e->v.payload = 0;
    }
    map->count = 0;
    map->tombstones = 0;
}

int32_t tua_map_has(tua_map* map, tua_value key) {
    if (!map) tua_panic("index null map");
    if (map_kind_any(map) == (uint32_t)TUA_MAP_KIND_IMAP) {
        int64_t k = 0;
        if (key.tag == TUA_VAL_LONG) k = (int64_t)key.payload;
        else if (key.tag == TUA_VAL_INT) k = (int64_t)(int32_t)key.payload;
        else tua_panic("map key must be int/long");
        return tua_imap_has(map, k);
    }
    KeyKind kind;
    uint32_t hash;
    int64_t ikey = 0;
    const char* skey = NULL;
    decode_key(key, &kind, &hash, &ikey, &skey);

    int found = 0;
    (void)find_slot(map, kind, hash, ikey, skey, &found);
    return found ? 1 : 0;
}

int32_t tua_map_len(tua_map* map) {
    if (!map) tua_panic("index null map");
    if (map_kind_any(map) == (uint32_t)TUA_MAP_KIND_IMAP) return tua_imap_len(map);
    if (map->count > (size_t)INT32_MAX) return INT32_MAX;
    return (int32_t)map->count;
}

int32_t tua_map_iter_next(tua_map* map, int32_t* index, tua_value* outKey, tua_value* outValue) {
    if (!map) tua_panic("index null map");
    if (map_kind_any(map) == (uint32_t)TUA_MAP_KIND_IMAP) return tua_imap_iter_next(map, index, outKey, outValue);
    if (!index || !outKey || !outValue) tua_panic("invalid map iterator args");

    size_t i = 0;
    if (*index > 0) i = (size_t)(*index);

    for (; i < map->capacity; i++) {
        MapEntry* e = &map->entries[i];
        if (e->kind == KEY_EMPTY || e->kind == KEY_TOMBSTONE) continue;

        if (e->kind == KEY_INT) {
            outKey->tag = TUA_VAL_LONG;
            outKey->payload = (uint64_t)e->k.i;
        } else if (e->kind == KEY_STRING) {
            outKey->tag = TUA_VAL_STRING;
            outKey->payload = (uint64_t)(uintptr_t)e->k.s;
        } else {
            outKey->tag = TUA_VAL_NIL;
            outKey->payload = 0;
        }

        *outValue = e->v;
        *index = (int32_t)(i + 1);
        return 1;
    }

    outKey->tag = TUA_VAL_NIL;
    outKey->payload = 0;
    outValue->tag = TUA_VAL_NIL;
    outValue->payload = 0;
    *index = (int32_t)map->capacity;
    return 0;
}

void tua_map_free(tua_map* map) {
    if (!map) return;
    if (map_kind_any(map) == (uint32_t)TUA_MAP_KIND_IMAP) {
        tua_imap_free(map);
        return;
    }
    if (map->entries && map->capacity > 0) {
        for (size_t i = 0; i < map->capacity; i++) {
            MapEntry* e = &map->entries[i];
            if (e->kind == KEY_STRING) tua_str_release(e->k.s);
            if (e->v.tag == TUA_VAL_STRING) tua_str_release((const char*)(uintptr_t)e->v.payload);
        }
        tua_free(map->entries);
        map->entries = NULL;
    }
    map->capacity = 0;
    map->count = 0;
    map->tombstones = 0;
    tua_free(map);
}

void tua_print_value(tua_value value, int32_t newline) {
    switch (value.tag) {
        case TUA_VAL_NIL:
            fputs("null", stdout);
            break;
        case TUA_VAL_BOOL:
            fputs(value.payload ? "true" : "false", stdout);
            break;
        case TUA_VAL_INT:
            printf("%" PRId32, (int32_t)value.payload);
            break;
        case TUA_VAL_LONG:
            printf("%" PRId64, (int64_t)value.payload);
            break;
        case TUA_VAL_DOUBLE: {
            double d = 0.0;
            uint64_t bits = value.payload;
            memcpy(&d, &bits, sizeof(double));
            printf("%g", d);
            break;
        }
        case TUA_VAL_STRING: {
            const char* s = (const char*)(uintptr_t)value.payload;
            if (!s) {
                fputs("null", stdout);
            } else {
                int64_t n = tua_str_byte_len(s);
                if (n > 0) fwrite(s, 1, (size_t)n, stdout);
            }
            break;
        }
        case TUA_VAL_PTR:
        default: {
            void* p = (void*)(uintptr_t)value.payload;
            printf("%p", p);
            break;
        }
    }
    if (newline) fputc('\n', stdout);
}

void tua_assert_fail(const char* msg, int32_t line) {
    const char* file = tua_current_file;
    int32_t useLine = line > 0 ? line : tua_current_line;
    int32_t col = tua_current_col;
    if (file && useLine > 0 && col > 0) {
        fprintf(stderr, "%s:%d:%d: error: assert failed: %s\n", file, (int)useLine, (int)col, (msg && msg[0] != '\0') ? msg : "");
    } else if (file && useLine > 0) {
        fprintf(stderr, "%s:%d: error: assert failed: %s\n", file, (int)useLine, (msg && msg[0] != '\0') ? msg : "");
    } else if (useLine > 0 && col > 0) {
        fprintf(stderr, "error:%d:%d: assert failed: %s\n", (int)useLine, (int)col, (msg && msg[0] != '\0') ? msg : "");
    } else if (useLine > 0) {
        fprintf(stderr, "error:%d: assert failed: %s\n", (int)useLine, (msg && msg[0] != '\0') ? msg : "");
    } else if (msg && msg[0] != '\0') {
        fprintf(stderr, "error: assert failed: %s\n", msg);
    } else {
        fprintf(stderr, "error: assert failed\n");
    }
    fflush(stderr);
    exit(1);
}

int32_t tua_value_to_int(tua_value v) {
    if (v.tag == TUA_VAL_INT) return (int32_t)v.payload;
    if (v.tag == TUA_VAL_LONG) return (int32_t)(int64_t)v.payload;
    tua_panic("cannot convert value to int");
    return 0;
}

int64_t tua_value_to_long(tua_value v) {
    if (v.tag == TUA_VAL_LONG) return (int64_t)v.payload;
    if (v.tag == TUA_VAL_INT) return (int64_t)(int32_t)v.payload;
    tua_panic("cannot convert value to long");
    return 0;
}

double tua_value_to_double(tua_value v) {
    if (v.tag == TUA_VAL_DOUBLE) {
        double d = 0.0;
        uint64_t bits = v.payload;
        memcpy(&d, &bits, sizeof(double));
        return d;
    }
    if (v.tag == TUA_VAL_INT) return (double)(int32_t)v.payload;
    if (v.tag == TUA_VAL_LONG) return (double)(int64_t)v.payload;
    tua_panic("cannot convert value to double");
    return 0.0;
}

int32_t tua_value_to_bool(tua_value v) {
    if (v.tag == TUA_VAL_BOOL) return v.payload ? 1 : 0;
    if (v.tag == TUA_VAL_NIL) return 0;
    // truthy for non-null values
    return 1;
}

char* tua_value_to_string(tua_value v) {
    if (v.tag == TUA_VAL_STRING) {
        const char* s = (const char*)(uintptr_t)v.payload;
        tua_str_retain(s);
        return (char*)s;
    }
    if (v.tag == TUA_VAL_NIL) return NULL;
    tua_panic("cannot convert value to string");
    return NULL;
}

int32_t tua_parse_int(const char* s, int32_t* out) {
    if (!s || !out) return 0;

    const unsigned char* p = (const unsigned char*)s;
    while (*p && isspace(*p)) p++;
    if (!*p) return 0;

    int sign = 1;
    if (*p == (unsigned char)'+') {
        p++;
    } else if (*p == (unsigned char)'-') {
        sign = -1;
        p++;
    }

    if (!*p || !isdigit(*p)) return 0;

    int64_t acc = 0;
    while (*p && isdigit(*p)) {
        acc = acc * 10 + (int64_t)(*p - (unsigned char)'0');
        if (sign > 0 && acc > (int64_t)INT32_MAX) return 0;
        if (sign < 0 && -acc < (int64_t)INT32_MIN) return 0;
        p++;
    }

    while (*p && isspace(*p)) p++;
    if (*p) return 0;

    int64_t v = sign > 0 ? acc : -acc;
    if (v < (int64_t)INT32_MIN || v > (int64_t)INT32_MAX) return 0;
    *out = (int32_t)v;
    return 1;
}
