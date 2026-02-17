#include <stdint.h>
#include <stdlib.h>

typedef struct {
    int32_t key;
    int32_t value;
    uint8_t used;
} int_int_entry;

typedef struct {
    int_int_entry* entries;
    int32_t cap; // power-of-2
} int_int_map;

static uint32_t hash_u32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

void* tua_iimap_new(int32_t cap_pow2) {
    if (cap_pow2 <= 0) return NULL;
    // Require power-of-2.
    if ((cap_pow2 & (cap_pow2 - 1)) != 0) return NULL;
    int_int_map* m = (int_int_map*)calloc(1, sizeof(int_int_map));
    if (!m) return NULL;
    m->cap = cap_pow2;
    m->entries = (int_int_entry*)calloc((size_t)cap_pow2, sizeof(int_int_entry));
    if (!m->entries) {
        free(m);
        return NULL;
    }
    return (void*)m;
}

void tua_iimap_free(void* h) {
    if (!h) return;
    int_int_map* m = (int_int_map*)h;
    free(m->entries);
    m->entries = NULL;
    m->cap = 0;
    free(m);
}

void tua_iimap_set(void* h, int32_t key, int32_t value) {
    int_int_map* m = (int_int_map*)h;
    if (!m || !m->entries || m->cap <= 0) return;
    uint32_t hsh = hash_u32((uint32_t)key);
    uint32_t mask = (uint32_t)m->cap - 1U;
    uint32_t i = hsh & mask;
    for (;;) {
        int_int_entry* e = &m->entries[i];
        if (!e->used || e->key == key) {
            e->used = 1;
            e->key = key;
            e->value = value;
            return;
        }
        i = (i + 1U) & mask;
    }
}

int32_t tua_iimap_get(void* h, int32_t key, int32_t* out) {
    int_int_map* m = (int_int_map*)h;
    if (!m || !m->entries || m->cap <= 0 || !out) return 0;
    uint32_t hsh = hash_u32((uint32_t)key);
    uint32_t mask = (uint32_t)m->cap - 1U;
    uint32_t i = hsh & mask;
    for (;;) {
        int_int_entry* e = &m->entries[i];
        if (!e->used) return 0;
        if (e->key == key) {
            *out = e->value;
            return 1;
        }
        i = (i + 1U) & mask;
    }
}

int32_t tua_iimap_get_present(void* h, int32_t key) {
    int_int_map* m = (int_int_map*)h;
    if (!m || !m->entries || m->cap <= 0) return 0;
    uint32_t hsh = hash_u32((uint32_t)key);
    uint32_t mask = (uint32_t)m->cap - 1U;
    uint32_t i = hsh & mask;
    for (;;) {
        int_int_entry* e = &m->entries[i];
        if (!e->used) return 0;
        if (e->key == key) return e->value;
        i = (i + 1U) & mask;
    }
}

