#ifndef _TUAC_ALLOC_H_
#define _TUAC_ALLOC_H_

// Compiler/tooling allocation shim.
//
// Today: route compiler allocations through the runtime allocator entrypoints so
// a custom allocator (for embedding) can observe/limit memory consistently.
// Future: this will become per-instance (tuac_ctx/tua_state) instead of global.

#include "rt/rt_alloc.h"

#include <stddef.h>
#include <string.h>

static inline void* tuac_malloc_impl(size_t n) { return tua_malloc(n); }
static inline void* tuac_calloc_impl(size_t n, size_t size) { return tua_calloc(n, size); }
static inline void* tuac_realloc_impl(void* p, size_t n) { return tua_realloc(p, n); }
static inline void tuac_free_impl(void* p) { tua_free(p); }

static inline char* tuac_strdup_impl(const char* s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char* out = (char*)tua_malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

// Redirect common libc allocators in compiler code to the shim.
// Important: include this header AFTER all system headers in a .c file.
#define malloc  tuac_malloc_impl
#define calloc  tuac_calloc_impl
#define realloc tuac_realloc_impl
#define free    tuac_free_impl
#define strdup  tuac_strdup_impl

#endif

