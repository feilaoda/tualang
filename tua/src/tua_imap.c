#include "tua_map.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
    IMAP_CTRL_EMPTY = 0x80,
    IMAP_CTRL_DELETED = 0xFE
};

enum {
    IMAP_KEY_I64 = 0,
    IMAP_KEY_I32 = 1
};

typedef struct tua_imap {
    uint32_t kind;      // TUA_MAP_KIND_IMAP
    int32_t value_tag;  // TUA_VAL_*
    uint32_t key_kind;  // IMAP_KEY_*

    size_t capacity; // power of two
    size_t count;
    size_t tombstones;

    uint8_t* ctrl; // capacity bytes
    union {
        void* any;
        int64_t* keys64;
        int32_t* keys32;
    } keys;
    uint64_t* vals; // capacity entries (raw tua_value.payload bits)
} tua_imap;

static uint32_t hash_u64(uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return (uint32_t)(x ^ (x >> 32));
}

static uint32_t hash_u32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x ? x : 1u;
}

static uint32_t hash_i64(int64_t v) {
    if (v >= (int64_t)INT32_MIN && v <= (int64_t)INT32_MAX) {
        return hash_u32((uint32_t)(int32_t)v);
    }
    return hash_u64((uint64_t)v);
}

static size_t next_pow2_size(size_t x) {
    if (x <= 16) return 16;
    size_t p = 1;
    while (p < x) p <<= 1;
    return p;
}

static size_t desired_capacity(size_t wantCount) {
    // max load factor ~= 7/8
    size_t cap = 16;
    while ((wantCount * 8) >= (cap * 7)) cap <<= 1;
    return cap;
}

static inline int imap_i32_key_ok(int64_t k, int32_t* out) {
    if (k < (int64_t)INT32_MIN || k > (int64_t)INT32_MAX) return 0;
    *out = (int32_t)k;
    return 1;
}

static void imap_alloc_arrays(tua_imap* m, size_t cap) {
    if (!m) return;
    m->capacity = cap;
    m->count = 0;
    m->tombstones = 0;
    m->ctrl = (uint8_t*)malloc(cap);
    m->vals = (uint64_t*)malloc(sizeof(uint64_t) * cap);
    if (m->key_kind == IMAP_KEY_I32) {
        m->keys.keys32 = (int32_t*)malloc(sizeof(int32_t) * cap);
    } else {
        m->keys.keys64 = (int64_t*)malloc(sizeof(int64_t) * cap);
    }
    if (!m->ctrl || !m->keys.any || !m->vals) tua_panic("out of memory");
    memset(m->ctrl, IMAP_CTRL_EMPTY, cap);
}

static void imap_rehash(tua_imap* m, size_t newCap) {
    if (!m) return;
    newCap = next_pow2_size(newCap);
    if (newCap < 16) newCap = 16;

    uint8_t* oldCtrl = m->ctrl;
    void* oldKeys = m->keys.any;
    uint64_t* oldVals = m->vals;
    size_t oldCap = m->capacity;
    uint32_t oldKeyKind = m->key_kind;

    m->ctrl = NULL;
    m->keys.any = NULL;
    m->vals = NULL;
    m->capacity = 0;
    m->count = 0;
    m->tombstones = 0;
    m->key_kind = oldKeyKind;
    imap_alloc_arrays(m, newCap);

    if (!oldCtrl || !oldKeys || !oldVals || oldCap == 0) return;

    size_t mask = m->capacity - 1;
    int isI32 = (oldKeyKind == IMAP_KEY_I32);
    for (size_t i = 0; i < oldCap; i++) {
        uint8_t c = oldCtrl[i];
        if (c >= IMAP_CTRL_EMPTY) continue;
        int64_t key = 0;
        if (oldKeyKind == IMAP_KEY_I32) key = (int64_t)((int32_t*)oldKeys)[i];
        else key = ((int64_t*)oldKeys)[i];
        uint64_t val = oldVals[i];

        uint32_t h = isI32 ? hash_u32((uint32_t)(int32_t)key) : hash_i64(key);
        uint8_t h2 = (uint8_t)(h & 0x7f);
        size_t idx = (size_t)h & mask;
        while (m->ctrl[idx] < IMAP_CTRL_EMPTY) idx = (idx + 1) & mask;
        m->ctrl[idx] = h2;
        if (isI32) m->keys.keys32[idx] = (int32_t)key;
        else m->keys.keys64[idx] = key;
        m->vals[idx] = val;
        m->count++;
    }

    free(oldCtrl);
    free(oldKeys);
    free(oldVals);
}

static void imap_maybe_grow(tua_imap* m, size_t addCount) {
    if (!m) return;
    size_t want = m->count + addCount;
    if (m->capacity == 0) {
        imap_rehash(m, desired_capacity(want));
        return;
    }
    // Rehash to clean tombstones even if we don't need more capacity.
    if (m->tombstones > (m->capacity / 4)) {
        imap_rehash(m, m->capacity);
        return;
    }
    if (((m->count + m->tombstones + addCount) * 8) >= (m->capacity * 7)) {
        imap_rehash(m, m->capacity * 2);
        return;
    }
}

static int imap_find_slot_i64(const tua_imap* m, int64_t key, uint32_t h, size_t* outSlot) {
    if (!m || m->capacity == 0 || !outSlot) return 0;
    size_t mask = m->capacity - 1;
    size_t idx = (size_t)h & mask;
    uint8_t h2 = (uint8_t)(h & 0x7f);
    for (;;) {
        uint8_t c = m->ctrl[idx];
        if (c == IMAP_CTRL_EMPTY) return 0;
        if (c == h2 && m->keys.keys64[idx] == key) {
            *outSlot = idx;
            return 1;
        }
        idx = (idx + 1) & mask;
    }
}

static int imap_find_slot_i32(const tua_imap* m, int32_t key, uint32_t h, size_t* outSlot) {
    if (!m || m->capacity == 0 || !outSlot) return 0;
    size_t mask = m->capacity - 1;
    size_t idx = (size_t)h & mask;
    uint8_t h2 = (uint8_t)(h & 0x7f);
    for (;;) {
        uint8_t c = m->ctrl[idx];
        if (c == IMAP_CTRL_EMPTY) return 0;
        if (c == h2 && m->keys.keys32[idx] == key) {
            *outSlot = idx;
            return 1;
        }
        idx = (idx + 1) & mask;
    }
}

static size_t imap_find_slot_for_insert_i64(const tua_imap* m, int64_t key, uint32_t h, int* outFound) {
    if (!m || m->capacity == 0) tua_panic("invalid map");
    size_t mask = m->capacity - 1;
    size_t idx = (size_t)h & mask;
    uint8_t h2 = (uint8_t)(h & 0x7f);
    size_t firstTomb = (size_t)(-1);
    for (;;) {
        uint8_t c = m->ctrl[idx];
        if (c == IMAP_CTRL_EMPTY) {
            if (outFound) *outFound = 0;
            return firstTomb != (size_t)(-1) ? firstTomb : idx;
        }
        if (c == IMAP_CTRL_DELETED) {
            if (firstTomb == (size_t)(-1)) firstTomb = idx;
        } else if (c == h2 && m->keys.keys64[idx] == key) {
            if (outFound) *outFound = 1;
            return idx;
        }
        idx = (idx + 1) & mask;
    }
}

static size_t imap_find_slot_for_insert_i32(const tua_imap* m, int32_t key, uint32_t h, int* outFound) {
    if (!m || m->capacity == 0) tua_panic("invalid map");
    size_t mask = m->capacity - 1;
    size_t idx = (size_t)h & mask;
    uint8_t h2 = (uint8_t)(h & 0x7f);
    size_t firstTomb = (size_t)(-1);
    for (;;) {
        uint8_t c = m->ctrl[idx];
        if (c == IMAP_CTRL_EMPTY) {
            if (outFound) *outFound = 0;
            return firstTomb != (size_t)(-1) ? firstTomb : idx;
        }
        if (c == IMAP_CTRL_DELETED) {
            if (firstTomb == (size_t)(-1)) firstTomb = idx;
        } else if (c == h2 && m->keys.keys32[idx] == key) {
            if (outFound) *outFound = 1;
            return idx;
        }
        idx = (idx + 1) & mask;
    }
}

static tua_map* imap_new_internal(uint32_t key_kind, int32_t value_tag, int32_t hint) {
    if (value_tag == TUA_VAL_NIL) tua_panic("invalid typed map value tag");
    tua_imap* m = (tua_imap*)calloc(1, sizeof(tua_imap));
    if (!m) tua_panic("out of memory");
    m->kind = (uint32_t)TUA_MAP_KIND_IMAP;
    m->value_tag = value_tag;
    m->key_kind = key_kind;
    size_t want = 0;
    if (hint > 0) want = (size_t)hint;
    imap_alloc_arrays(m, desired_capacity(want));
    return (tua_map*)m;
}

tua_map* tua_imap_new(int32_t value_tag, int32_t hint) {
    return imap_new_internal(IMAP_KEY_I64, value_tag, hint);
}

tua_map* tua_imap_new_i32(int32_t value_tag, int32_t hint) {
    return imap_new_internal(IMAP_KEY_I32, value_tag, hint);
}

uint64_t tua_imap_get_payload_with_ok(tua_map* map, int64_t key, int32_t* outOk) {
    if (!map) tua_panic("index null map");
    if (!outOk) tua_panic("invalid outOk");
    tua_imap* m = (tua_imap*)map;
    if (m->kind != (uint32_t)TUA_MAP_KIND_IMAP) tua_panic("invalid typed map");
    if (m->capacity == 0) {
        *outOk = 0;
        return 0;
    }
    size_t slot = 0;
    if (m->key_kind == IMAP_KEY_I32) {
        int32_t kk = 0;
        if (!imap_i32_key_ok(key, &kk)) {
            *outOk = 0;
            return 0;
        }
        uint32_t h = hash_u32((uint32_t)kk);
        if (!imap_find_slot_i32(m, kk, h, &slot)) {
            *outOk = 0;
            return 0;
        }
    } else {
        uint32_t h = hash_i64(key);
        if (!imap_find_slot_i64(m, key, h, &slot)) {
            *outOk = 0;
            return 0;
        }
    }
    if (m->ctrl[slot] >= IMAP_CTRL_EMPTY) {
        *outOk = 0;
        return 0;
    }
    *outOk = 1;
    return m->vals[slot];
}

void tua_imap_set_payload(tua_map* map, int64_t key, uint64_t payload) {
    if (!map) tua_panic("assign into null map");
    tua_imap* m = (tua_imap*)map;
    if (m->kind != (uint32_t)TUA_MAP_KIND_IMAP) tua_panic("invalid typed map");
    imap_maybe_grow(m, 1);
    int found = 0;
    size_t slot = 0;
    if (m->key_kind == IMAP_KEY_I32) {
        int32_t kk = 0;
        if (!imap_i32_key_ok(key, &kk)) tua_panic("map key must be int");
        uint32_t h = hash_u32((uint32_t)kk);
        slot = imap_find_slot_for_insert_i32(m, kk, h, &found);
    } else {
        uint32_t h = hash_i64(key);
        slot = imap_find_slot_for_insert_i64(m, key, h, &found);
    }
    uint8_t prev = m->ctrl[slot];
    if (!found) {
        if (prev == IMAP_CTRL_DELETED) m->tombstones--;
        if (m->key_kind == IMAP_KEY_I32) {
            int32_t kk = (int32_t)key;
            uint32_t h = hash_u32((uint32_t)kk);
            m->ctrl[slot] = (uint8_t)(h & 0x7f);
            m->keys.keys32[slot] = kk;
        } else {
            uint32_t h = hash_i64(key);
            m->ctrl[slot] = (uint8_t)(h & 0x7f);
            m->keys.keys64[slot] = key;
        }
        m->count++;
    }
    m->vals[slot] = payload;
}

int32_t tua_imap_delete(tua_map* map, int64_t key) {
    if (!map) tua_panic("delete on null map");
    tua_imap* m = (tua_imap*)map;
    if (m->kind != (uint32_t)TUA_MAP_KIND_IMAP) tua_panic("invalid typed map");
    if (m->capacity == 0) return 0;
    size_t slot = 0;
    if (m->key_kind == IMAP_KEY_I32) {
        int32_t kk = 0;
        if (!imap_i32_key_ok(key, &kk)) return 0;
        uint32_t h = hash_u32((uint32_t)kk);
        if (!imap_find_slot_i32(m, kk, h, &slot)) return 0;
    } else {
        uint32_t h = hash_i64(key);
        if (!imap_find_slot_i64(m, key, h, &slot)) return 0;
    }
    m->ctrl[slot] = IMAP_CTRL_DELETED;
    m->tombstones++;
    m->count--;
    return 1;
}

void tua_imap_clear(tua_map* map) {
    if (!map) tua_panic("clear on null map");
    tua_imap* m = (tua_imap*)map;
    if (m->kind != (uint32_t)TUA_MAP_KIND_IMAP) tua_panic("invalid typed map");
    if (!m->ctrl || m->capacity == 0) return;
    memset(m->ctrl, IMAP_CTRL_EMPTY, m->capacity);
    m->count = 0;
    m->tombstones = 0;
}

int32_t tua_imap_has(tua_map* map, int64_t key) {
    if (!map) tua_panic("hasKey on null map");
    tua_imap* m = (tua_imap*)map;
    if (m->kind != (uint32_t)TUA_MAP_KIND_IMAP) tua_panic("invalid typed map");
    if (m->capacity == 0) return 0;
    size_t slot = 0;
    if (m->key_kind == IMAP_KEY_I32) {
        int32_t kk = 0;
        if (!imap_i32_key_ok(key, &kk)) return 0;
        uint32_t h = hash_u32((uint32_t)kk);
        return imap_find_slot_i32(m, kk, h, &slot) ? 1 : 0;
    }
    uint32_t h = hash_i64(key);
    return imap_find_slot_i64(m, key, h, &slot) ? 1 : 0;
}

int32_t tua_imap_len(tua_map* map) {
    if (!map) tua_panic("len on null map");
    tua_imap* m = (tua_imap*)map;
    if (m->kind != (uint32_t)TUA_MAP_KIND_IMAP) tua_panic("invalid typed map");
    if (m->count > (size_t)INT32_MAX) return (int32_t)INT32_MAX;
    return (int32_t)m->count;
}

int32_t tua_imap_iter_next(tua_map* map, int32_t* index, tua_value* outKey, tua_value* outValue) {
    if (!map) tua_panic("iter on null map");
    if (!index) tua_panic("invalid iter index");
    if (!outKey || !outValue) tua_panic("invalid iter out");
    tua_imap* m = (tua_imap*)map;
    if (m->kind != (uint32_t)TUA_MAP_KIND_IMAP) tua_panic("invalid typed map");
    if (m->capacity == 0) return 0;
    int32_t i = *index;
    if (i < 0) i = 0;
    for (size_t p = (size_t)i; p < m->capacity; p++) {
        uint8_t c = m->ctrl[p];
        if (c >= IMAP_CTRL_EMPTY) continue;
        outKey->tag = TUA_VAL_LONG;
        outKey->payload = (uint64_t)((m->key_kind == IMAP_KEY_I32) ? (int64_t)m->keys.keys32[p] : m->keys.keys64[p]);
        outValue->tag = m->value_tag;
        outValue->payload = m->vals[p];
        *index = (int32_t)(p + 1);
        return 1;
    }
    return 0;
}

void tua_imap_free(tua_map* map) {
    if (!map) return;
    tua_imap* m = (tua_imap*)map;
    if (m->kind != (uint32_t)TUA_MAP_KIND_IMAP) tua_panic("invalid typed map");
    free(m->ctrl);
    free(m->keys.any);
    free(m->vals);
    free(m);
}
