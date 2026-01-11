#include "rt/rt_alloc.h"

#include <string.h>
#include <stdlib.h>

static void* tua_sys_alloc(void* ud, void* ptr, size_t old_sz, size_t new_sz) {
    (void)ud;
    (void)old_sz;
    if (new_sz == 0) {
        free(ptr);
        return NULL;
    }
    if (ptr == NULL) {
        return malloc(new_sz);
    }
    return realloc(ptr, new_sz);
}

static tua_allocator tua_global_allocator = { tua_sys_alloc, NULL };

tua_allocator tua_allocator_default(void) {
    tua_allocator a = { tua_sys_alloc, NULL };
    return a;
}

void tua_allocator_set_global(tua_allocator a) {
    if (a.alloc == NULL) {
        tua_global_allocator = tua_allocator_default();
        return;
    }
    tua_global_allocator = a;
}

tua_allocator tua_allocator_get_global(void) {
    return tua_global_allocator;
}

void* tua_alloc(tua_allocator* a, void* ptr, size_t old_sz, size_t new_sz) {
    tua_allocator* use = a ? a : &tua_global_allocator;
    if (use->alloc == NULL) {
        // Defensive: treat as default instead of crashing.
        tua_allocator def = tua_allocator_default();
        return def.alloc(def.ud, ptr, old_sz, new_sz);
    }
    return use->alloc(use->ud, ptr, old_sz, new_sz);
}

void* tua_malloc(size_t size) {
    return tua_alloc(NULL, NULL, 0, size);
}

void* tua_calloc(size_t n, size_t size) {
    if (n == 0 || size == 0) return tua_alloc(NULL, NULL, 0, 0);
    size_t total = n * size;
    void* p = tua_alloc(NULL, NULL, 0, total);
    if (p) memset(p, 0, total);
    return p;
}

void* tua_realloc(void* ptr, size_t size) {
    return tua_alloc(NULL, ptr, 0, size);
}

void tua_free(void* ptr) {
    (void)tua_alloc(NULL, ptr, 0, 0);
}
