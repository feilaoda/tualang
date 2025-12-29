#include "tua_array.h"

#include "tua_map.h"

#include <stdlib.h>
#include <string.h>

tua_array* tua_array_new(int64_t len, int64_t cap, int64_t elem_size, int64_t fixed_len) {
    if (elem_size <= 0) tua_panic("invalid array element size");
    if (fixed_len >= 0) {
        len = fixed_len;
        cap = fixed_len;
    }
    if (len < 0) tua_panic("invalid array length");
    if (cap < len) cap = len;

    tua_array* a = (tua_array*)malloc(sizeof(tua_array));
    if (!a) tua_panic("out of memory");

    a->len = len;
    a->cap = cap;
    a->elem_size = elem_size;
    a->fixed_len = fixed_len;

    if (cap == 0) {
        a->data = NULL;
        return a;
    }

    size_t bytes = (size_t)cap * (size_t)elem_size;
    void* buf = calloc(1, bytes);
    if (!buf) tua_panic("out of memory");
    a->data = buf;
    return a;
}

tua_array* tua_array_clone(tua_array* a) {
    if (!a) tua_panic("null array");
    tua_array* b = (tua_array*)malloc(sizeof(tua_array));
    if (!b) tua_panic("out of memory");
    b->len = a->len;
    b->cap = a->cap;
    b->elem_size = a->elem_size;
    b->fixed_len = a->fixed_len;

    if (a->cap <= 0 || a->elem_size <= 0) {
        b->data = NULL;
        return b;
    }

    size_t bytes = (size_t)a->cap * (size_t)a->elem_size;
    void* buf = malloc(bytes);
    if (!buf) tua_panic("out of memory");
    if (a->data) memcpy(buf, a->data, bytes);
    else memset(buf, 0, bytes);
    b->data = buf;
    return b;
}

int64_t tua_array_push(tua_array* a, const void* elem) {
    if (!a) tua_panic("null array");
    if (a->fixed_len >= 0) tua_panic("cannot push to fixed-length array");
    if (a->elem_size <= 0) tua_panic("invalid array element size");
    if (!elem && a->elem_size > 0) tua_panic("null element pointer");

    if (a->len < 0 || a->cap < 0) tua_panic("invalid array header");
    if (a->len >= a->cap) {
        int64_t newCap = a->cap > 0 ? a->cap * 2 : 1;
        if (newCap < a->len + 1) newCap = a->len + 1;

        size_t oldBytes = (size_t)a->cap * (size_t)a->elem_size;
        size_t newBytes = (size_t)newCap * (size_t)a->elem_size;
        void* newData = realloc(a->data, newBytes);
        if (!newData) tua_panic("out of memory");
        if (newBytes > oldBytes) {
            memset((char*)newData + oldBytes, 0, newBytes - oldBytes);
        }
        a->data = newData;
        a->cap = newCap;
    }

    if (a->elem_size > 0) {
        memcpy((char*)a->data + (size_t)a->len * (size_t)a->elem_size, elem, (size_t)a->elem_size);
    }
    a->len += 1;
    return a->len;
}

void tua_array_free(tua_array* a) {
    if (!a) return;
    free(a->data);
    a->data = NULL;
    free(a);
}
