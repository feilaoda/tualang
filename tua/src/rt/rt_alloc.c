#include "rt/rt_alloc.h"

#include "rt/rt_config.h"

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

tua_allocator tua_allocator_default(void) {
    tua_allocator a = { tua_sys_alloc, NULL };
    return a;
}

void tua_allocator_set_global(tua_allocator a) {
    tua_config g = tua_rt_get_global_config();
    g.allocator = a.alloc ? a : tua_allocator_default();
    tua_rt_configure(&g);
}

tua_allocator tua_allocator_get_global(void) {
    tua_config g = tua_rt_get_global_config();
    return g.allocator.alloc ? g.allocator : tua_allocator_default();
}

void* tua_alloc(tua_allocator* a, void* ptr, size_t old_sz, size_t new_sz) {
    tua_allocator use;
    if (a && a->alloc) {
        use = *a;
    } else {
        tua_config c = tua_rt_get_config();
        use = c.allocator.alloc ? c.allocator : tua_allocator_default();
    }
    if (!use.alloc) use = tua_allocator_default();
    return use.alloc(use.ud, ptr, old_sz, new_sz);
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
