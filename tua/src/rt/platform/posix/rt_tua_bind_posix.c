#include "rt/rt_platform.h"

#if defined(TUA_OS_POSIX)

#include "rt/rt_alloc.h"
#include "rt/rt_cancel.h"
#include "rt/rt_err.h"
#include "rt/rt_fs_async.h"
#include "rt/rt_fs.h"
#include "rt/rt_loop.h"
#include "rt/rt_net.h"
#include "rt/rt_workqueue.h"

#include "tua_array.h"

#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    void* fn;
    void* env;
} tua_closure_t;

static void tua_call_void0(tua_closure_t c) {
    if (c.fn == NULL) return;
    union { void* p; void (*f)(void*); } u;
    u.p = c.fn;
    u.f(c.env);
}

static void tua_call_void2(tua_closure_t c, int32_t a0, void* a1) {
    if (c.fn == NULL) return;
    union { void* p; void (*f)(void*, int32_t, void*); } u;
    u.p = c.fn;
    u.f(c.env, a0, a1);
}

static void tua_call_void3(tua_closure_t c, int32_t a0, void* a1, int32_t a2) {
    if (c.fn == NULL) return;
    union { void* p; void (*f)(void*, int32_t, void*, int32_t); } u;
    u.p = c.fn;
    u.f(c.env, a0, a1, a2);
}

static void tua_call_void1_i64(tua_closure_t c, int32_t a0, int64_t a1) {
    if (c.fn == NULL) return;
    union { void* p; void (*f)(void*, int32_t, int64_t); } u;
    u.p = c.fn;
    u.f(c.env, a0, a1);
}

static void tua_call_void5_stat(tua_closure_t c, int32_t err, int32_t kind, int64_t size, int64_t mtime_ns, int32_t mode) {
    if (c.fn == NULL) return;
    union { void* p; void (*f)(void*, int32_t, int32_t, int64_t, int64_t, int32_t); } u;
    u.p = c.fn;
    u.f(c.env, err, kind, size, mtime_ns, mode);
}

static int32_t tua_kind_to_i32(tua_fs_kind_t k) {
    switch (k) {
        case TUA_FS_FILE: return 1;
        case TUA_FS_DIR: return 2;
        case TUA_FS_SYMLINK: return 3;
        default: return 0;
    }
}

typedef struct {
    tua_closure_t cb;
} tua_timer_after_ms_ctx_t;

static void tua_timer_after_ms_cb(void* arg) {
    tua_timer_after_ms_ctx_t* ctx = (tua_timer_after_ms_ctx_t*)arg;
    if (ctx == NULL) return;
    tua_call_void0(ctx->cb);
    tua_free(ctx);
}

tua_err_t tua_timer_after_ms_cl(tua_loop_t* loop, int64_t delay_ms, tua_closure_t cb) {
    if (loop == NULL) {
        return TUA_E_INVALID;
    }
    if (delay_ms <= 0) {
        tua_call_void0(cb);
        return TUA_OK;
    }
    tua_timer_after_ms_ctx_t* ctx = (tua_timer_after_ms_ctx_t*)tua_malloc(sizeof(*ctx));
    if (ctx == NULL) {
        return TUA_E_NOMEM;
    }
    ctx->cb = cb;

    tua_err_t err = tua_timer_start(loop, NULL, (uint64_t)delay_ms, 0, tua_timer_after_ms_cb, ctx);
    if (err != TUA_OK) {
        tua_free(ctx);
        return err;
    }
    return TUA_OK;
}

typedef struct {
    tua_closure_t cb;
} tua_connect_cl_ctx_t;

static void tua_connect_cl_cb(tua_err_t err, tua_tcp_socket_t* sock, void* arg) {
    tua_connect_cl_ctx_t* ctx = (tua_connect_cl_ctx_t*)arg;
    tua_call_void2(ctx->cb, (int32_t)err, (void*)sock);
    tua_free(ctx);
}

tua_err_t tua_tcp_connect_async_cl(
    tua_loop_t* loop,
    tua_workqueue_t* wq,
    const char* host_utf8,
    const char* port_utf8,
    tua_deadline_t deadline,
    tua_closure_t cb
) {
    tua_connect_cl_ctx_t* ctx = (tua_connect_cl_ctx_t*)tua_malloc(sizeof(*ctx));
    if (ctx == NULL) {
        return TUA_E_NOMEM;
    }
    ctx->cb = cb;
    tua_err_t err = tua_tcp_connect_async(loop, wq, host_utf8, port_utf8, deadline, tua_connect_cl_cb, ctx);
    if (err != TUA_OK) {
        tua_free(ctx);
    }
    return err;
}

tua_err_t tua_tcp_connect_port_async_cl(
    tua_loop_t* loop,
    tua_workqueue_t* wq,
    const char* host_utf8,
    int32_t port,
    tua_deadline_t deadline,
    tua_closure_t cb
) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", (int)port);
    return tua_tcp_connect_async_cl(loop, wq, host_utf8, buf, deadline, cb);
}

typedef struct {
    tua_tcp_accept_t* inner;
    tua_closure_t cb;
} tua_accept_cl_handle_t;

static void tua_accept_cl_cb(tua_err_t err, tua_tcp_socket_t* sock, void* arg) {
    tua_accept_cl_handle_t* h = (tua_accept_cl_handle_t*)arg;
    tua_call_void2(h->cb, (int32_t)err, (void*)sock);
}

tua_err_t tua_tcp_accept_start_cl(
    tua_loop_t* loop,
    tua_tcp_listener_t* lst,
    tua_closure_t cb,
    void** out_handle
) {
    if (out_handle == NULL) {
        return TUA_E_INVALID;
    }
    tua_accept_cl_handle_t* h = (tua_accept_cl_handle_t*)tua_malloc(sizeof(*h));
    if (h == NULL) {
        return TUA_E_NOMEM;
    }
    h->inner = NULL;
    h->cb = cb;
    tua_err_t err = tua_tcp_accept_start(loop, lst, tua_accept_cl_cb, h, &h->inner);
    if (err != TUA_OK) {
        tua_free(h);
        return err;
    }
    *out_handle = h;
    return TUA_OK;
}

void tua_tcp_accept_cancel_cl(void* handle) {
    tua_accept_cl_handle_t* h = (tua_accept_cl_handle_t*)handle;
    if (h == NULL) return;
    if (h->inner != NULL) {
        tua_tcp_accept_cancel(h->inner);
        h->inner = NULL;
    }
    tua_free(h);
}

typedef struct {
    tua_closure_t cb;
    uint8_t* buf;
    int32_t max;
} tua_read_alloc_ctx_t;

static void tua_read_alloc_cb(tua_err_t err, size_t n, void* arg) {
    tua_read_alloc_ctx_t* ctx = (tua_read_alloc_ctx_t*)arg;
    if (err == TUA_OK) {
        size_t nn = n;
        if (nn > (size_t)ctx->max) nn = (size_t)ctx->max;
        ctx->buf[nn] = 0;
        tua_call_void3(ctx->cb, (int32_t)err, (void*)ctx->buf, (int32_t)nn);
        // caller frees ctx->buf via tua_free
    } else {
        tua_free(ctx->buf);
        ctx->buf = NULL;
        tua_call_void3(ctx->cb, (int32_t)err, NULL, 0);
    }
    tua_free(ctx);
}

tua_err_t tua_tcp_read_alloc_async_cl(
    tua_loop_t* loop,
    tua_tcp_socket_t* sock,
    int32_t max,
    tua_deadline_t deadline,
    tua_closure_t cb
) {
    if (max <= 0) {
        tua_call_void3(cb, TUA_OK, NULL, 0);
        return TUA_OK;
    }
    tua_read_alloc_ctx_t* ctx = (tua_read_alloc_ctx_t*)tua_malloc(sizeof(*ctx));
    if (ctx == NULL) return TUA_E_NOMEM;
    ctx->cb = cb;
    ctx->max = max;
    ctx->buf = (uint8_t*)tua_malloc((size_t)max + 1);
    if (ctx->buf == NULL) {
        tua_free(ctx);
        return TUA_E_NOMEM;
    }
    return tua_tcp_read_async(loop, sock, ctx->buf, (size_t)max, deadline, tua_read_alloc_cb, ctx);
}

typedef struct {
    tua_closure_t cb;
} tua_write_ctx_t;

static void tua_write_cb(tua_err_t err, size_t n, void* arg) {
    tua_write_ctx_t* ctx = (tua_write_ctx_t*)arg;
    tua_call_void1_i64(ctx->cb, (int32_t)err, (int64_t)n);
    tua_free(ctx);
}

tua_err_t tua_tcp_write_str_async_cl(
    tua_loop_t* loop,
    tua_tcp_socket_t* sock,
    const char* s,
    tua_deadline_t deadline,
    tua_closure_t cb
) {
    if (s == NULL) {
        tua_call_void1_i64(cb, TUA_OK, 0);
        return TUA_OK;
    }
    tua_write_ctx_t* ctx = (tua_write_ctx_t*)tua_malloc(sizeof(*ctx));
    if (ctx == NULL) return TUA_E_NOMEM;
    ctx->cb = cb;
    size_t len = strlen(s);
    tua_err_t err = tua_tcp_write_async(loop, sock, (const uint8_t*)s, len, deadline, tua_write_cb, ctx);
    if (err != TUA_OK) {
        tua_free(ctx);
    }
    return err;
}

typedef struct {
    tua_closure_t cb;
} tua_writefile_ctx_t;

static void tua_writefile_cb(tua_err_t err, void* arg) {
    tua_writefile_ctx_t* ctx = (tua_writefile_ctx_t*)arg;
    tua_call_void2(ctx->cb, (int32_t)err, NULL);
    tua_free(ctx);
}

tua_err_t tua_fs_writefile_str_async_cl(
    tua_loop_t* loop,
    tua_workqueue_t* wq,
    const char* path_utf8,
    const char* data,
    tua_closure_t cb
) {
    tua_writefile_ctx_t* ctx = (tua_writefile_ctx_t*)tua_malloc(sizeof(*ctx));
    if (ctx == NULL) return TUA_E_NOMEM;
    ctx->cb = cb;
    const uint8_t* bytes = (const uint8_t*)(data ? data : "");
    size_t len = data ? strlen(data) : 0;
    tua_err_t err = tua_fs_writefile_async(loop, wq, path_utf8, bytes, len, tua_writefile_cb, ctx);
    if (err != TUA_OK) {
        tua_free(ctx);
    }
    return err;
}

typedef struct {
    tua_closure_t cb;
} tua_readfile_cl_ctx_t;

static void tua_readfile_cl_cb(tua_err_t err, uint8_t* data, size_t len, void* arg) {
    tua_readfile_cl_ctx_t* ctx = (tua_readfile_cl_ctx_t*)arg;
    if (err != TUA_OK) {
        if (data) tua_free(data);
        tua_call_void3(ctx->cb, (int32_t)err, NULL, 0);
        tua_free(ctx);
        return;
    }

    uint8_t* z = (uint8_t*)tua_malloc(len + 1);
    if (z == NULL) {
        if (data) tua_free(data);
        tua_call_void3(ctx->cb, (int32_t)TUA_E_NOMEM, NULL, 0);
        tua_free(ctx);
        return;
    }
    if (len > 0) memcpy(z, data, len);
    z[len] = 0;
    if (data) tua_free(data);

    tua_call_void3(ctx->cb, (int32_t)TUA_OK, (void*)z, (int32_t)len);
    // caller frees z via tua_free
    tua_free(ctx);
}

tua_err_t tua_fs_readfile_alloc_async_cl(
    tua_loop_t* loop,
    tua_workqueue_t* wq,
    const char* path_utf8,
    tua_closure_t cb
) {
    tua_readfile_cl_ctx_t* ctx = (tua_readfile_cl_ctx_t*)tua_malloc(sizeof(*ctx));
    if (ctx == NULL) return TUA_E_NOMEM;
    ctx->cb = cb;
    tua_err_t err = tua_fs_readfile_async(loop, wq, path_utf8, tua_readfile_cl_cb, ctx);
    if (err != TUA_OK) {
        tua_free(ctx);
    }
    return err;
}

typedef struct {
    tua_closure_t cb;
} tua_stat_cl_ctx_t;

static void tua_stat_cl_cb(tua_err_t err, tua_fs_stat_t st, void* arg) {
    tua_stat_cl_ctx_t* ctx = (tua_stat_cl_ctx_t*)arg;
    if (err != TUA_OK) {
        tua_call_void5_stat(ctx->cb, (int32_t)err, 0, 0, 0, 0);
    } else {
        tua_call_void5_stat(
            ctx->cb,
            (int32_t)TUA_OK,
            tua_kind_to_i32(st.kind),
            (int64_t)st.size,
            (int64_t)st.mtime_ns,
            (int32_t)st.mode
        );
    }
    tua_free(ctx);
}

tua_err_t tua_fs_stat_async_cl(
    tua_loop_t* loop,
    tua_workqueue_t* wq,
    const char* path_utf8,
    tua_closure_t cb
) {
    tua_stat_cl_ctx_t* ctx = (tua_stat_cl_ctx_t*)tua_malloc(sizeof(*ctx));
    if (ctx == NULL) return TUA_E_NOMEM;
    ctx->cb = cb;
    tua_err_t err = tua_fs_stat_async(loop, wq, path_utf8, tua_stat_cl_cb, ctx);
    if (err != TUA_OK) {
        tua_free(ctx);
    }
    return err;
}

typedef struct {
    tua_closure_t cb;
} tua_readdir_cl_ctx_t;

static void tua_readdir_cl_cb(tua_err_t err, char** names, size_t count, void* arg) {
    tua_readdir_cl_ctx_t* ctx = (tua_readdir_cl_ctx_t*)arg;
    if (err != TUA_OK) {
        if (names) tua_fs_dirlist_free(names, count);
        tua_call_void2(ctx->cb, (int32_t)err, NULL);
        tua_free(ctx);
        return;
    }

    tua_array* arr = tua_array_new(0, (int64_t)count, (int64_t)sizeof(char*), -1);
    if (arr == NULL) {
        tua_fs_dirlist_free(names, count);
        tua_call_void2(ctx->cb, (int32_t)TUA_E_NOMEM, NULL);
        tua_free(ctx);
        return;
    }

    for (size_t i = 0; i < count; i++) {
        char* s = names[i];
        if (s == NULL) continue;
        size_t sl = strlen(s);
        char* copy = (char*)tua_malloc(sl + 1);
        if (copy == NULL) {
            // best-effort: stop adding more
            break;
        }
        memcpy(copy, s, sl + 1);
        (void)tua_array_push(arr, &copy);
    }

    tua_fs_dirlist_free(names, count);
    tua_call_void2(ctx->cb, (int32_t)TUA_OK, (void*)arr);
    tua_free(ctx);
}

tua_err_t tua_fs_readdir_async_cl(
    tua_loop_t* loop,
    tua_workqueue_t* wq,
    const char* path_utf8,
    tua_closure_t cb
) {
    tua_readdir_cl_ctx_t* ctx = (tua_readdir_cl_ctx_t*)tua_malloc(sizeof(*ctx));
    if (ctx == NULL) return TUA_E_NOMEM;
    ctx->cb = cb;
    tua_err_t err = tua_fs_readdir_async(loop, wq, path_utf8, tua_readdir_cl_cb, ctx);
    if (err != TUA_OK) {
        tua_free(ctx);
    }
    return err;
}

void tua_fs_string_array_free(tua_array* arr) {
    if (arr == NULL) return;
    if (arr->data != NULL && arr->elem_size == (int64_t)sizeof(char*)) {
        char** items = (char**)arr->data;
        for (int64_t i = 0; i < arr->len; i++) {
            if (items[i] != NULL) {
                tua_free(items[i]);
            }
        }
    }
    tua_array_free(arr);
}

// Synchronous helper: returns a `tua_array*` of `char*` and stores the error in `out_err`.
tua_array* tua_fs_readdir_arr(const char* path_utf8, int32_t* out_err) {
    if (out_err == NULL) {
        return NULL;
    }
    *out_err = (int32_t)TUA_E_INVALID;
    if (path_utf8 == NULL) {
        return NULL;
    }

    char** names = NULL;
    size_t count = 0;
    tua_err_t err = tua_fs_readdir(path_utf8, &names, &count);
    if (err != TUA_OK) {
        if (names) tua_fs_dirlist_free(names, count);
        *out_err = (int32_t)err;
        return NULL;
    }

    tua_array* arr = tua_array_new(0, (int64_t)count, (int64_t)sizeof(char*), -1);
    if (arr == NULL) {
        tua_fs_dirlist_free(names, count);
        *out_err = (int32_t)TUA_E_NOMEM;
        return NULL;
    }

    for (size_t i = 0; i < count; i++) {
        char* s = names[i];
        if (s == NULL) continue;
        size_t sl = strlen(s);
        char* copy = (char*)tua_malloc(sl + 1);
        if (copy == NULL) {
            break;
        }
        memcpy(copy, s, sl + 1);
        (void)tua_array_push(arr, &copy);
    }

    tua_fs_dirlist_free(names, count);
    *out_err = (int32_t)TUA_OK;
    return arr;
}

#endif
