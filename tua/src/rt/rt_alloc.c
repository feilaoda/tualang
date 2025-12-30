#include "rt/rt_alloc.h"

#include <stdlib.h>

void* tua_malloc(size_t size) {
    return malloc(size);
}

void* tua_realloc(void* ptr, size_t size) {
    return realloc(ptr, size);
}

void tua_free(void* ptr) {
    free(ptr);
}

