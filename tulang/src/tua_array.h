#ifndef TUA_ARRAY_H
#define TUA_ARRAY_H

#include <stdint.h>

typedef struct tua_array {
    int64_t len;
    int64_t cap;
    void* data;
    int64_t elem_size;
    int64_t fixed_len; // -1 => dynamic, >=0 => fixed (len == fixed_len)
} tua_array;

tua_array* tua_array_new(int64_t len, int64_t cap, int64_t elem_size, int64_t fixed_len);
tua_array* tua_array_clone(tua_array* a);
int64_t tua_array_push(tua_array* a, const void* elem);
void tua_array_free(tua_array* a);

#endif
