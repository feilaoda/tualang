#ifndef TUA_ARRAY_H
#define TUA_ARRAY_H

#include <stdint.h>

typedef struct tua_array {
    int64_t len;
    int64_t cap;
    void* data;
    int64_t elem_size;
    int64_t fixed_len; // -1 => dynamic, >=0 => fixed (len == fixed_len)
    int32_t refcnt;
    void (*elem_retain)(void* elem);
    void (*elem_release)(void* elem);
} tua_array;

typedef void (*tua_array_elem_hook)(void* elem);

tua_array* tua_array_new(int64_t len, int64_t cap, int64_t elem_size, int64_t fixed_len);
tua_array* tua_array_clone(tua_array* a);
int64_t tua_array_push(tua_array* a, const void* elem);
void tua_array_set_elem_hooks(tua_array* a, tua_array_elem_hook retain_fn, tua_array_elem_hook release_fn);
void tua_array_set_at(tua_array* a, int64_t index, const void* elem);
void tua_array_retain(tua_array* a);
void tua_array_release(tua_array* a);
void tua_array_free(tua_array* a);

// Built-in element hooks for handle-like array element types.
void tua_array_elem_retain_string(void* elem);
void tua_array_elem_release_string(void* elem);
void tua_array_elem_retain_map(void* elem);
void tua_array_elem_release_map(void* elem);
void tua_array_elem_retain_array(void* elem);
void tua_array_elem_release_array(void* elem);
void tua_array_elem_retain_bytes(void* elem);
void tua_array_elem_release_bytes(void* elem);

#endif
