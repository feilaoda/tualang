#include "tua_map.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    TUA_VAL_NIL = 0,
    TUA_VAL_INT = 1,
    TUA_VAL_LONG = 2,
    TUA_VAL_DOUBLE = 3,
    TUA_VAL_BOOL = 4,
    TUA_VAL_STRING = 5,
    TUA_VAL_PTR = 6
};

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
        char* s;
    } k;
    tua_value v;
} MapEntry;

struct tua_map {
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
    if (file && line > 0 && col > 0) {
        fprintf(stderr, "%s:%d:%d: error: %s\n", file, (int)line, (int)col, msg ? msg : "(null)");
    } else if (file && line > 0) {
        fprintf(stderr, "%s:%d: error: %s\n", file, (int)line, msg ? msg : "(null)");
    } else if (line > 0 && col > 0) {
        fprintf(stderr, "error:%d:%d: %s\n", (int)line, (int)col, msg ? msg : "(null)");
    } else if (line > 0) {
        fprintf(stderr, "error:%d: %s\n", (int)line, msg ? msg : "(null)");
    } else {
        fprintf(stderr, "error: %s\n", msg ? msg : "(null)");
    }
    abort();
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

static uint32_t hash_cstr(const char* s) {
    // FNV-1a 32-bit
    uint32_t h = 2166136261u;
    for (const unsigned char* p = (const unsigned char*)s; p && *p; p++) {
        h ^= *p;
        h *= 16777619u;
    }
    return h ? h : 1u;
}

static int key_equals(const MapEntry* e, KeyKind kind, int64_t ikey, const char* skey) {
    if (!e || e->kind != kind) return 0;
    if (kind == KEY_INT) return e->k.i == ikey;
    if (kind == KEY_STRING) return strcmp(e->k.s, skey) == 0;
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
        if (e->kind != KEY_TOMBSTONE && e->hash == hash && key_equals(e, kind, ikey, skey)) {
            if (found) *found = 1;
            return i;
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
        } else if (e->hash == hash && key_equals(e, kind, ikey, skey)) {
            if (found) *found = 1;
            return i;
        }
        i = (i + 1) & mask;
    }
}

static void map_rehash(tua_map* m, size_t newCap) {
    MapEntry* old = m->entries;
    size_t oldCap = m->capacity;

    MapEntry* entries = (MapEntry*)calloc(newCap, sizeof(MapEntry));
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

    free(old);
}

static void ensure_capacity(tua_map* m) {
    if (!m) tua_panic("invalid map");
    if (m->capacity == 0) {
        m->capacity = 16;
        m->entries = (MapEntry*)calloc(m->capacity, sizeof(MapEntry));
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
        *hash = hash_u64((uint64_t)v);
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
        *hash = hash_cstr(s);
        if (skey) *skey = s;
        return;
    }

    tua_panic("map key must be int/long/string");
}

tua_map* tua_map_new(void) {
    tua_map* m = (tua_map*)calloc(1, sizeof(tua_map));
    if (!m) tua_panic("out of memory");
    m->capacity = 0;
    m->count = 0;
    m->tombstones = 0;
    m->entries = NULL;
    ensure_capacity(m);
    return m;
}

tua_value tua_map_get(tua_map* map, tua_value key) {
    if (!map) tua_panic("index null map");
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

void tua_map_set(tua_map* map, tua_value key, tua_value value) {
    if (!map) tua_panic("assign into null map");
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
            e->k.s = strdup(skey ? skey : "");
            if (!e->k.s) tua_panic("out of memory");
        }
        map->count++;
    }

    // Overwrite semantics: later entries win.
    e->v = value;
}

int32_t tua_map_delete(tua_map* map, tua_value key) {
    if (!map) tua_panic("index null map");
    KeyKind kind;
    uint32_t hash;
    int64_t ikey = 0;
    const char* skey = NULL;
    decode_key(key, &kind, &hash, &ikey, &skey);

    int found = 0;
    size_t slot = find_slot(map, kind, hash, ikey, skey, &found);
    if (!found) return 0;

    MapEntry* e = &map->entries[slot];
    if (e->kind == KEY_STRING && e->k.s) {
        free(e->k.s);
        e->k.s = NULL;
    }

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
    if (!map->entries || map->capacity == 0) return;
    for (size_t i = 0; i < map->capacity; i++) {
        MapEntry* e = &map->entries[i];
        if (e->kind == KEY_STRING && e->k.s) {
            free(e->k.s);
            e->k.s = NULL;
        }
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
    if (map->count > (size_t)INT32_MAX) return INT32_MAX;
    return (int32_t)map->count;
}

int32_t tua_map_iter_next(tua_map* map, int32_t* index, tua_value* outKey, tua_value* outValue) {
    if (!map) tua_panic("index null map");
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
            if (!s) fputs("null", stdout);
            else fputs(s, stdout);
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

char* tua_str_concat(const char* a, const char* b) {
    const char* na = a ? a : "null";
    const char* nb = b ? b : "null";
    size_t la = strlen(na);
    size_t lb = strlen(nb);
    char* out = (char*)malloc(la + lb + 1);
    if (!out) tua_panic("out of memory");
    memcpy(out, na, la);
    memcpy(out + la, nb, lb);
    out[la + lb] = '\0';
    return out;
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
    abort();
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
    if (v.tag == TUA_VAL_STRING) return (char*)(uintptr_t)v.payload;
    if (v.tag == TUA_VAL_NIL) return NULL;
    tua_panic("cannot convert value to string");
    return NULL;
}
