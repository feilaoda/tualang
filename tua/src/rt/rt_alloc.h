#ifndef _TUA_RT_ALLOC_H_
#define _TUA_RT_ALLOC_H_

#include <stddef.h>

typedef void* (*tua_alloc_fn)(void* ud, void* ptr, size_t old_sz, size_t new_sz);

typedef struct tua_allocator {
    tua_alloc_fn alloc;
    void* ud;
} tua_allocator;

// Returns the built-in allocator (malloc/realloc/free semantics).
tua_allocator tua_allocator_default(void);

// Sets the process-global allocator used by `tua_malloc/tua_realloc/tua_free`.
// Not thread-safe; call once during startup (before any Tua runtime allocations).
// Passing `.alloc = NULL` resets to the default allocator.
void tua_allocator_set_global(tua_allocator a);
tua_allocator tua_allocator_get_global(void);

// Unified allocation entry. If `a` is NULL, uses the global allocator.
// Note: `old_sz` may be 0 (unknown) because the runtime doesn't track sizes yet.
void* tua_alloc(tua_allocator* a, void* ptr, size_t old_sz, size_t new_sz);

void* tua_malloc(size_t size);
void* tua_calloc(size_t n, size_t size);
void* tua_realloc(void* ptr, size_t size);
void tua_free(void* ptr);

#endif
