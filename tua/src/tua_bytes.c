#include "tua_bytes.h"

#include "rt/rt_alloc.h"
#include "rt/rt_fs.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

// Provided by `src/tua_map.c`.
void tua_panic(const char* msg);

typedef void (*tua_bytes_drop_fn)(void* ctx, uint8_t* data, int64_t len);

struct tua_bytes {
    int64_t len;
    int64_t cap;
    uint8_t* data;
    int32_t readonly;
    void* drop_ctx;
    tua_bytes_drop_fn drop_fn;
};

static void tua_bytes_drop_free(void* ctx, uint8_t* data, int64_t len) {
    (void)ctx;
    (void)len;
    tua_free(data);
}

static void tua_bytes_drop_mmap(void* ctx, uint8_t* data, int64_t len) {
    (void)data;
    (void)len;
    tua_fs_mmap_close((tua_mmap_t*)ctx);
}

tua_bytes* tua_bytes_new(int64_t len) {
    if (len < 0) return NULL;
    tua_bytes* b = (tua_bytes*)tua_malloc(sizeof(tua_bytes));
    if (!b) return NULL;
    memset(b, 0, sizeof(*b));
    b->len = len;
    b->cap = len;
    b->readonly = 0;
    b->drop_ctx = NULL;
    b->drop_fn = tua_bytes_drop_free;
    if (len == 0) {
        b->data = NULL;
        return b;
    }
    if ((uint64_t)len > (uint64_t)SIZE_MAX) {
        tua_free(b);
        return NULL;
    }
    b->data = (uint8_t*)tua_malloc((size_t)len);
    if (!b->data) {
        tua_free(b);
        return NULL;
    }
    memset(b->data, 0, (size_t)len);
    return b;
}

tua_bytes* tua_bytes_new_uninit(int64_t len) {
    if (len < 0) return NULL;
    tua_bytes* b = (tua_bytes*)tua_malloc(sizeof(tua_bytes));
    if (!b) return NULL;
    memset(b, 0, sizeof(*b));
    b->len = len;
    b->cap = len;
    b->readonly = 0;
    b->drop_ctx = NULL;
    b->drop_fn = tua_bytes_drop_free;
    if (len == 0) {
        b->data = NULL;
        return b;
    }
    if ((uint64_t)len > (uint64_t)SIZE_MAX) {
        tua_free(b);
        return NULL;
    }
    b->data = (uint8_t*)tua_malloc((size_t)len);
    if (!b->data) {
        tua_free(b);
        return NULL;
    }
    return b;
}

tua_bytes* tua_bytes_from_copy(const void* data, int64_t len) {
    if (len < 0) return NULL;
    if (len == 0) return tua_bytes_new(0);
    if (!data) return NULL;
    tua_bytes* b = tua_bytes_new(len);
    if (!b || !b->data) return b;
    memcpy(b->data, data, (size_t)len);
    return b;
}

tua_bytes* tua_bytes_from_string_copy(const char* s) {
    if (!s) return tua_bytes_new(0);
    size_t n = strlen(s);
    if (n > (size_t)LLONG_MAX) return NULL;
    return tua_bytes_from_copy(s, (int64_t)n);
}

tua_err_t tua_bytes_mmap_file(const char* path_utf8, tua_bytes** out_bytes) {
    if (!out_bytes) return TUA_E_INVALID;
    *out_bytes = NULL;

    tua_mmap_t* map = NULL;
    tua_err_t err = tua_fs_mmap_ro(path_utf8, &map);
    if (err != TUA_OK) return err;
    if (!map) return TUA_E_INVALID;

    int64_t len = tua_fs_mmap_len(map);
    const uint8_t* data = tua_fs_mmap_data(map);
    if (len < 0 || (len > 0 && data == NULL)) {
        tua_fs_mmap_close(map);
        return TUA_E_INVALID;
    }

    tua_bytes* b = (tua_bytes*)tua_malloc(sizeof(tua_bytes));
    if (!b) {
        tua_fs_mmap_close(map);
        return TUA_E_NOMEM;
    }
    memset(b, 0, sizeof(*b));
    b->len = len;
    b->cap = len;
    b->data = (uint8_t*)data; // read-only mapping; mutation via bytes.set is UB (guarded at language level later)
    b->readonly = 1;
    b->drop_ctx = map;
    b->drop_fn = tua_bytes_drop_mmap;
    *out_bytes = b;
    return TUA_OK;
}

int64_t tua_bytes_len(tua_bytes* b) {
    if (!b) return 0;
    return b->len;
}

uint8_t* tua_bytes_data(tua_bytes* b) {
    if (!b) return NULL;
    return b->data;
}

uint8_t* tua_bytes_data_at(tua_bytes* b, int64_t off) {
    if (!b) return NULL;
    if (!b->data) return NULL;
    if (off < 0 || off > b->len) return NULL;
    return b->data + off;
}

int32_t tua_bytes_is_readonly(tua_bytes* b) {
    if (!b) return 0;
    return b->readonly ? 1 : 0;
}

tua_err_t tua_bytes_get_u8(tua_bytes* b, int64_t idx, int32_t* out) {
    if (!out) return TUA_E_INVALID;
    *out = 0;
    if (!b) return TUA_E_INVALID;
    if (idx < 0 || idx >= b->len) return TUA_E_INVALID;
    if (!b->data) return TUA_E_INVALID;
    *out = (int32_t)b->data[idx];
    return TUA_OK;
}

tua_err_t tua_bytes_set_u8(tua_bytes* b, int64_t idx, int32_t v) {
    if (!b) return TUA_E_INVALID;
    if (b->readonly) return TUA_E_ACCESS;
    if (idx < 0 || idx >= b->len) return TUA_E_INVALID;
    if (!b->data) return TUA_E_INVALID;
    if (v < 0 || v > 255) return TUA_E_INVALID;
    b->data[idx] = (uint8_t)v;
    return TUA_OK;
}

tua_err_t tua_bytes_copy(tua_bytes* dst, int64_t dst_off, tua_bytes* src, int64_t src_off, int64_t n) {
    if (!dst || !src) return TUA_E_INVALID;
    if (dst->readonly) return TUA_E_ACCESS;
    if (n < 0) return TUA_E_INVALID;
    if (n == 0) return TUA_OK;
    if (!dst->data || !src->data) return TUA_E_INVALID;
    if (dst_off < 0 || src_off < 0) return TUA_E_INVALID;
    if (dst_off > dst->len || src_off > src->len) return TUA_E_INVALID;
    if (dst_off + n > dst->len) return TUA_E_INVALID;
    if (src_off + n > src->len) return TUA_E_INVALID;
    memmove(dst->data + dst_off, src->data + src_off, (size_t)n);
    return TUA_OK;
}

char* tua_str_from_bytes_copy(tua_bytes* b, int64_t off, int64_t len) {
    if (len < 0) return NULL;
    if (len == 0) {
        char* out = (char*)malloc(1);
        if (!out) tua_panic("out of memory");
        out[0] = '\0';
        return out;
    }
    if (!b || !b->data) return NULL;
    if (off < 0 || off > b->len) return NULL;
    if (off + len > b->len) return NULL;
    if ((uint64_t)len > (uint64_t)SIZE_MAX - 1) return NULL;
    char* out = (char*)malloc((size_t)len + 1);
    if (!out) tua_panic("out of memory");
    memcpy(out, b->data + off, (size_t)len);
    out[(size_t)len] = '\0';
    return out;
}

void* tua_str_ptr(const char* s) {
    return (void*)s;
}

double tua_f32_from_u32_bits(uint32_t bits) {
    float f = 0.0f;
    memcpy(&f, &bits, sizeof(float));
    return (double)f;
}

double tua_f64_from_u64_bits(uint64_t bits) {
    double d = 0.0;
    memcpy(&d, &bits, sizeof(double));
    return d;
}

tua_err_t tua_bytes_bf16_to_f32(tua_bytes* src, int64_t src_off, tua_bytes* dst, int64_t dst_off, int64_t n) {
    if (!src || !dst) return TUA_E_INVALID;
    if (dst->readonly) return TUA_E_ACCESS;
    if (n < 0) return TUA_E_INVALID;
    if (n == 0) return TUA_OK;
    if (!src->data || !dst->data) return TUA_E_INVALID;
    if (src_off < 0 || dst_off < 0) return TUA_E_INVALID;
    if (src_off > src->len || dst_off > dst->len) return TUA_E_INVALID;

    const int64_t src_bytes = n * 2;
    const int64_t dst_bytes = n * 4;
    if (src_bytes < 0 || dst_bytes < 0) return TUA_E_INVALID;
    if (src_off + src_bytes > src->len) return TUA_E_INVALID;
    if (dst_off + dst_bytes > dst->len) return TUA_E_INVALID;

    const uint16_t* s = (const uint16_t*)(src->data + src_off);
    float* d = (float*)(dst->data + dst_off);
    for (int64_t i = 0; i < n; i++) {
        union {
            uint32_t u;
            float f;
        } v;
        v.u = ((uint32_t)s[i]) << 16;
        d[i] = v.f;
    }
    return TUA_OK;
}

void tua_bytes_free(tua_bytes* b) {
    if (!b) return;
    if (b->drop_fn) {
        b->drop_fn(b->drop_ctx, b->data, b->len);
    }
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    b->readonly = 0;
    b->drop_ctx = NULL;
    b->drop_fn = NULL;
    tua_free(b);
}
