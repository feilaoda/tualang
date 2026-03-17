#include "tua_array.h"

#include "tua_map.h"
#include "tua_bytes.h"

#include <stdlib.h>
#include <string.h>

#include "rt/rt_alloc.h"

void tua_array_elem_retain_string(void* elem) {
    if (!elem) return;
    char* s = *(char**)elem;
    tua_str_retain(s);
}

void tua_array_elem_release_string(void* elem) {
    if (!elem) return;
    char** p = (char**)elem;
    tua_str_release(*p);
    *p = NULL;
}

void tua_array_elem_retain_map(void* elem) {
    if (!elem) return;
    tua_map* m = *(tua_map**)elem;
    tua_map_retain(m);
}

void tua_array_elem_release_map(void* elem) {
    if (!elem) return;
    tua_map** p = (tua_map**)elem;
    tua_map_free(*p);
    *p = NULL;
}

void tua_array_elem_retain_array(void* elem) {
    if (!elem) return;
    tua_array* a = *(tua_array**)elem;
    tua_array_retain(a);
}

void tua_array_elem_release_array(void* elem) {
    if (!elem) return;
    tua_array** p = (tua_array**)elem;
    tua_array_free(*p);
    *p = NULL;
}

void tua_array_elem_retain_bytes(void* elem) {
    if (!elem) return;
    tua_bytes* b = *(tua_bytes**)elem;
    tua_bytes_retain(b);
}

void tua_array_elem_release_bytes(void* elem) {
    if (!elem) return;
    tua_bytes** p = (tua_bytes**)elem;
    tua_bytes_free(*p);
    *p = NULL;
}

static void tua_array_drop_elements(tua_array* a) {
    if (!a || !a->data || a->len <= 0 || a->elem_size <= 0 || !a->elem_release) return;
    for (int64_t i = 0; i < a->len; i++) {
        void* elem = (void*)((char*)a->data + (size_t)i * (size_t)a->elem_size);
        a->elem_release(elem);
    }
}

tua_array* tua_array_new(int64_t len, int64_t cap, int64_t elem_size, int64_t fixed_len) {
    if (elem_size <= 0) tua_panic("invalid array element size");
    if (fixed_len >= 0) {
        len = fixed_len;
        cap = fixed_len;
    }
    if (len < 0) tua_panic("invalid array length");
    if (cap < len) cap = len;

    tua_array* a = (tua_array*)tua_malloc(sizeof(tua_array));
    if (!a) tua_panic("out of memory");

    a->len = len;
    a->cap = cap;
    a->elem_size = elem_size;
    a->fixed_len = fixed_len;
    a->refcnt = 1;
    a->elem_retain = NULL;
    a->elem_release = NULL;

    if (cap == 0) {
        a->data = NULL;
        return a;
    }

    size_t bytes = (size_t)cap * (size_t)elem_size;
    void* buf = tua_calloc(1, bytes);
    if (!buf) tua_panic("out of memory");
    a->data = buf;
    return a;
}

tua_array* tua_array_clone(tua_array* a) {
    if (!a) tua_panic("null array");
    tua_array* b = (tua_array*)tua_malloc(sizeof(tua_array));
    if (!b) tua_panic("out of memory");
    b->len = a->len;
    b->cap = a->cap;
    b->elem_size = a->elem_size;
    b->fixed_len = a->fixed_len;
    b->refcnt = 1;
    b->elem_retain = a->elem_retain;
    b->elem_release = a->elem_release;

    if (a->cap <= 0 || a->elem_size <= 0) {
        b->data = NULL;
        return b;
    }

    size_t bytes = (size_t)a->cap * (size_t)a->elem_size;
    void* buf = tua_malloc(bytes);
    if (!buf) tua_panic("out of memory");
    if (a->data) memcpy(buf, a->data, bytes);
    else memset(buf, 0, bytes);
    b->data = buf;
    if (b->elem_retain && b->len > 0) {
        for (int64_t i = 0; i < b->len; i++) {
            void* elem = (void*)((char*)b->data + (size_t)i * (size_t)b->elem_size);
            b->elem_retain(elem);
        }
    }
    return b;
}

int64_t tua_array_push(tua_array* a, const void* elem) {
    if (!a) tua_panic("null array");
    if (a->fixed_len >= 0) tua_panic("cannot push to fixed-length array");
    if (a->elem_size <= 0) tua_panic("invalid array element size");
    // Defensive guard: element sizes this large are almost certainly memory corruption.
    if (a->elem_size > (int64_t)(1024 * 1024)) tua_panic("invalid array element size (too large)");
    if (!elem && a->elem_size > 0) tua_panic("null element pointer");

    if (a->len < 0 || a->cap < 0) tua_panic("invalid array header");
    if (a->len >= a->cap) {
        int64_t newCap = a->cap > 0 ? a->cap * 2 : 1;
        if (newCap < a->len + 1) newCap = a->len + 1;

        size_t oldBytes = (size_t)a->cap * (size_t)a->elem_size;
        size_t newBytes = (size_t)newCap * (size_t)a->elem_size;
        void* newData = tua_realloc(a->data, newBytes);
        if (!newData) tua_panic("out of memory");
        if (newBytes > oldBytes) {
            memset((char*)newData + oldBytes, 0, newBytes - oldBytes);
        }
        a->data = newData;
        a->cap = newCap;
    }

    if (a->elem_size > 0) {
        memcpy((char*)a->data + (size_t)a->len * (size_t)a->elem_size, elem, (size_t)a->elem_size);
        if (a->elem_retain) {
            void* dst = (void*)((char*)a->data + (size_t)a->len * (size_t)a->elem_size);
            a->elem_retain(dst);
        }
    }
    a->len += 1;
    return a->len;
}

void tua_array_set_elem_hooks(tua_array* a, tua_array_elem_hook retain_fn, tua_array_elem_hook release_fn) {
    if (!a) tua_panic("null array");
    a->elem_retain = retain_fn;
    a->elem_release = release_fn;
}

void tua_array_set_at(tua_array* a, int64_t index, const void* elem) {
    if (!a) tua_panic("null array");
    if (!elem && a->elem_size > 0) tua_panic("null element pointer");
    if (index < 0 || index >= a->len) tua_panic("array index out of bounds");
    if (!a->data && a->elem_size > 0) tua_panic("null array data");

    void* dst = (void*)((char*)a->data + (size_t)index * (size_t)a->elem_size);
    if (a->elem_size > 0 && memcmp(dst, elem, (size_t)a->elem_size) == 0) return;
    if (a->elem_release) a->elem_release(dst);
    if (a->elem_size > 0) {
        memcpy(dst, elem, (size_t)a->elem_size);
        if (a->elem_retain) a->elem_retain(dst);
    }
}

void tua_array_retain(tua_array* a) {
    if (!a) return;
    if (a->refcnt <= 0) tua_panic("invalid array refcount");
    a->refcnt += 1;
}

void tua_array_release(tua_array* a) {
    if (!a) return;
    if (a->refcnt <= 0) tua_panic("invalid array refcount");
    a->refcnt -= 1;
    if (a->refcnt > 0) return;
    tua_array_drop_elements(a);
    tua_free(a->data);
    a->data = NULL;
    tua_free(a);
}

void tua_array_free(tua_array* a) {
    tua_array_release(a);
}
