#ifndef _TUA_RT_ALLOC_H_
#define _TUA_RT_ALLOC_H_

#include <stddef.h>

void* tua_malloc(size_t size);
void* tua_realloc(void* ptr, size_t size);
void tua_free(void* ptr);

#endif

