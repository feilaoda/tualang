#ifdef _WIN32

#include "rt/rt_fs.h"
#include "rt/rt_alloc.h"
#include "rt/rt_loop.h"
#include "rt/rt_net.h"
#include "rt/rt_thread.h"

#include "tua_array.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef struct tua_task {
    tua_task_fn fn;
    void* arg;
} tua_task_t;

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

struct tua_loop {
    HANDLE iocp;
    HANDLE timerq;
    volatile LONG stop;
};

struct tua_timer {
    tua_loop_t* loop;
    HANDLE h;
    volatile LONG active;
    uint64_t repeat_ms;
    tua_task_fn fn;
    void* arg;
};

static void tua_win32_timer_cleanup_common(tua_timer_t* timer, HANDLE completion_event) {
    if (timer == NULL) {
        return;
    }
    if (timer->h != NULL && timer->loop != NULL && timer->loop->timerq != NULL) {
        (void)DeleteTimerQueueTimer(timer->loop->timerq, timer->h, completion_event);
    }
    tua_free(timer);
}

static void tua_win32_timer_cleanup_task(void* p) {
    tua_win32_timer_cleanup_common((tua_timer_t*)p, INVALID_HANDLE_VALUE);
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
} tua_post_cl_ctx_t;

static void tua_post_cl_task(void* arg) {
    tua_post_cl_ctx_t* ctx = (tua_post_cl_ctx_t*)arg;
    if (ctx == NULL) return;
    tua_call_void0(ctx->cb);
    tua_free(ctx);
}

tua_err_t tua_loop_post_cl(tua_loop_t* loop, tua_closure_t cb) {
    if (loop == NULL) {
        return TUA_E_INVALID;
    }
    tua_post_cl_ctx_t* ctx = (tua_post_cl_ctx_t*)tua_malloc(sizeof(*ctx));
    if (ctx == NULL) {
        return TUA_E_NOMEM;
    }
    ctx->cb = cb;
    tua_err_t err = tua_loop_post(loop, tua_post_cl_task, ctx);
    if (err != TUA_OK) {
        tua_free(ctx);
        return err;
    }
    return TUA_OK;
}

typedef struct {
    tua_loop_t* loop;
    tua_timer_t* timer;
    tua_closure_t cb;
} tua_timer_every_cl_handle_t;

static void tua_timer_every_cl_cb(void* arg) {
    tua_timer_every_cl_handle_t* h = (tua_timer_every_cl_handle_t*)arg;
    if (h == NULL) return;
    tua_call_void0(h->cb);
}

static void tua_timer_every_free_task(void* arg) {
    tua_timer_every_cl_handle_t* h = (tua_timer_every_cl_handle_t*)arg;
    if (h == NULL) return;
    tua_free(h);
}

tua_err_t tua_timer_every_ms_cl(
    tua_loop_t* loop,
    int64_t interval_ms,
    tua_closure_t cb,
    void** out_handle
) {
    if (loop == NULL || out_handle == NULL) {
        return TUA_E_INVALID;
    }
    if (interval_ms <= 0) {
        return TUA_E_INVALID;
    }
    tua_timer_every_cl_handle_t* h = (tua_timer_every_cl_handle_t*)tua_malloc(sizeof(*h));
    if (h == NULL) {
        return TUA_E_NOMEM;
    }
    h->loop = loop;
    h->timer = NULL;
    h->cb = cb;

    tua_err_t err = tua_timer_start(loop, &h->timer, (uint64_t)interval_ms, (uint64_t)interval_ms, tua_timer_every_cl_cb, h);
    if (err != TUA_OK) {
        tua_free(h);
        return err;
    }
    *out_handle = h;
    return TUA_OK;
}

void tua_timer_every_cancel_cl(void* handle) {
    tua_timer_every_cl_handle_t* h = (tua_timer_every_cl_handle_t*)handle;
    if (h == NULL) return;

    if (h->timer != NULL) {
        tua_timer_cancel(h->timer);
        h->timer = NULL;
    }
    if (h->loop == NULL) {
        return;
    }
    if (tua_loop_post(h->loop, tua_timer_every_free_task, h) != TUA_OK) {
        return;
    }
}

tua_err_t tua_fs_readfile_alloc(const char* path_utf8, char** out_data, int32_t* out_len) {
    (void)path_utf8;
    if (out_data) *out_data = NULL;
    if (out_len) *out_len = 0;
    return TUA_E_NOTSUP;
}

tua_err_t tua_fs_writefile_str(const char* path_utf8, const char* data_utf8) {
    (void)path_utf8;
    (void)data_utf8;
    return TUA_E_NOTSUP;
}

tua_err_t tua_fs_stat_simple(
    const char* path_utf8,
    int32_t* out_kind,
    int64_t* out_size,
    int64_t* out_mtime_ns,
    int32_t* out_mode
) {
    (void)path_utf8;
    if (out_kind) *out_kind = 0;
    if (out_size) *out_size = 0;
    if (out_mtime_ns) *out_mtime_ns = 0;
    if (out_mode) *out_mode = 0;
    return TUA_E_NOTSUP;
}

tua_err_t tua_fs_mkdir(const char* path_utf8, int32_t mode) {
    (void)path_utf8;
    (void)mode;
    return TUA_E_NOTSUP;
}

tua_err_t tua_fs_realpath_alloc(const char* path_utf8, char** out_path_utf8) {
    (void)path_utf8;
    if (out_path_utf8) *out_path_utf8 = NULL;
    return TUA_E_NOTSUP;
}

tua_err_t tua_fs_readdir(const char* path_utf8, char*** out_names, size_t* out_count) {
    (void)path_utf8;
    if (out_names) *out_names = NULL;
    if (out_count) *out_count = 0;
    return TUA_E_NOTSUP;
}

void tua_fs_dirlist_free(char** names, size_t count) {
    (void)names;
    (void)count;
}

tua_array* tua_fs_readdir_arr(const char* path_utf8, int32_t* out_err) {
    (void)path_utf8;
    if (out_err) *out_err = TUA_E_NOTSUP;
    return NULL;
}

void tua_fs_string_array_free(tua_array* arr) {
    (void)arr;
}

tua_err_t tua_thread_create(tua_thread_t** out, tua_thread_fn fn, void* arg) {
    (void)out;
    (void)fn;
    (void)arg;
    return TUA_E_NOTSUP;
}

tua_err_t tua_thread_join(tua_thread_t* thread) {
    (void)thread;
    return TUA_E_NOTSUP;
}

void tua_thread_free(tua_thread_t* thread) {
    (void)thread;
}

tua_err_t tua_mutex_create(tua_mutex_t** out) {
    (void)out;
    return TUA_E_NOTSUP;
}

void tua_mutex_free(tua_mutex_t* mutex) {
    (void)mutex;
}

tua_err_t tua_mutex_lock(tua_mutex_t* mutex) {
    (void)mutex;
    return TUA_E_NOTSUP;
}

tua_err_t tua_mutex_unlock(tua_mutex_t* mutex) {
    (void)mutex;
    return TUA_E_NOTSUP;
}

tua_err_t tua_cond_create(tua_cond_t** out) {
    (void)out;
    return TUA_E_NOTSUP;
}

void tua_cond_free(tua_cond_t* cond) {
    (void)cond;
}

tua_err_t tua_cond_wait(tua_cond_t* cond, tua_mutex_t* mutex) {
    (void)cond;
    (void)mutex;
    return TUA_E_NOTSUP;
}

tua_err_t tua_cond_signal(tua_cond_t* cond) {
    (void)cond;
    return TUA_E_NOTSUP;
}

tua_err_t tua_cond_broadcast(tua_cond_t* cond) {
    (void)cond;
    return TUA_E_NOTSUP;
}

tua_err_t tua_loop_create(tua_loop_t** out) {
    if (out == NULL) {
        return TUA_E_INVALID;
    }
    tua_loop_t* loop = (tua_loop_t*)tua_malloc(sizeof(*loop));
    if (loop == NULL) {
        return TUA_E_NOMEM;
    }
    loop->iocp = NULL;
    loop->timerq = NULL;
    loop->stop = 0;

    loop->iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
    if (loop->iocp == NULL) {
        tua_free(loop);
        return TUA_E_IO;
    }
    loop->timerq = CreateTimerQueue();
    if (loop->timerq == NULL) {
        CloseHandle(loop->iocp);
        tua_free(loop);
        return TUA_E_IO;
    }

    *out = loop;
    return TUA_OK;
}

void tua_loop_free(tua_loop_t* loop) {
    if (loop == NULL) {
        return;
    }
    (void)InterlockedExchange(&loop->stop, 1);
    if (loop->timerq != NULL) {
        (void)DeleteTimerQueueEx(loop->timerq, INVALID_HANDLE_VALUE);
        loop->timerq = NULL;
    }
    if (loop->iocp != NULL) {
        CloseHandle(loop->iocp);
        loop->iocp = NULL;
    }
    tua_free(loop);
}

tua_err_t tua_loop_run(tua_loop_t* loop) {
    if (loop == NULL || loop->iocp == NULL) {
        return TUA_E_INVALID;
    }

    while (InterlockedCompareExchange(&loop->stop, 0, 0) == 0) {
        DWORD bytes = 0;
        ULONG_PTR key = 0;
        LPOVERLAPPED ov = NULL;
        BOOL ok = GetQueuedCompletionStatus(loop->iocp, &bytes, &key, &ov, INFINITE);
        (void)ok;
        (void)bytes;
        (void)ov;

        if (key == 0) {
            continue;
        }
        tua_task_t* task = (tua_task_t*)key;
        if (task->fn) {
            task->fn(task->arg);
        }
        tua_free(task);
    }

    return TUA_OK;
}

void tua_loop_stop(tua_loop_t* loop) {
    if (loop == NULL || loop->iocp == NULL) {
        return;
    }
    (void)InterlockedExchange(&loop->stop, 1);
    (void)PostQueuedCompletionStatus(loop->iocp, 0, 0, NULL);
}

tua_err_t tua_loop_post(tua_loop_t* loop, tua_task_fn fn, void* arg) {
    if (loop == NULL || loop->iocp == NULL || fn == NULL) {
        return TUA_E_INVALID;
    }
    if (InterlockedCompareExchange(&loop->stop, 0, 0) != 0) {
        return TUA_E_CANCELED;
    }
    tua_task_t* task = (tua_task_t*)tua_malloc(sizeof(*task));
    if (task == NULL) {
        return TUA_E_NOMEM;
    }
    task->fn = fn;
    task->arg = arg;
    if (!PostQueuedCompletionStatus(loop->iocp, 0, (ULONG_PTR)task, NULL)) {
        tua_free(task);
        return TUA_E_IO;
    }
    return TUA_OK;
}

tua_err_t tua_io_start_handle(
    tua_loop_t* loop,
    tua_io_t** out,
    tua_handle_t handle,
    int events,
    tua_io_fn fn,
    void* arg
) {
    (void)loop;
    (void)out;
    (void)handle;
    (void)events;
    (void)fn;
    (void)arg;
    return TUA_E_NOTSUP;
}

tua_err_t tua_io_start(
    tua_loop_t* loop,
    tua_io_t** out,
    tua_fd_t fd,
    int events,
    tua_io_fn fn,
    void* arg
) {
    return tua_io_start_handle(loop, out, tua_handle_from_fd(fd), events, fn, arg);
}

void tua_io_cancel(tua_io_t* io) {
    (void)io;
}

static VOID CALLBACK tua_timerqueue_cb(PVOID param, BOOLEAN fired) {
    (void)fired;
    tua_timer_t* timer = (tua_timer_t*)param;
    if (timer == NULL) {
        return;
    }
    if (InterlockedCompareExchange(&timer->active, 0, 0) == 0) {
        return;
    }
    (void)tua_loop_post(timer->loop, timer->fn, timer->arg);
    if (timer->repeat_ms == 0) {
        (void)InterlockedExchange(&timer->active, 0);
        // One-shot: clean up on the loop thread.
        if (tua_loop_post(timer->loop, tua_win32_timer_cleanup_task, timer) != TUA_OK) {
            // Best-effort cleanup without waiting (avoid deadlock inside callback).
            tua_win32_timer_cleanup_common(timer, NULL);
        }
    }
}

tua_err_t tua_timer_start(
    tua_loop_t* loop,
    tua_timer_t** out,
    uint64_t delay_ms,
    uint64_t repeat_ms,
    tua_task_fn fn,
    void* arg
) {
    if (loop == NULL || loop->timerq == NULL || fn == NULL) {
        return TUA_E_INVALID;
    }
    tua_timer_t* timer = (tua_timer_t*)tua_malloc(sizeof(*timer));
    if (timer == NULL) {
        return TUA_E_NOMEM;
    }
    timer->loop = loop;
    timer->h = NULL;
    timer->active = 1;
    timer->repeat_ms = repeat_ms;
    timer->fn = fn;
    timer->arg = arg;

    DWORD due = (delay_ms > 0xffffffffu) ? 0xffffffffu : (DWORD)delay_ms;
    DWORD period = (repeat_ms > 0xffffffffu) ? 0xffffffffu : (DWORD)repeat_ms;
    if (!CreateTimerQueueTimer(&timer->h, loop->timerq, tua_timerqueue_cb, timer, due, period, WT_EXECUTEDEFAULT)) {
        tua_free(timer);
        return TUA_E_IO;
    }

    if (out) {
        *out = timer;
    }
    return TUA_OK;
}

void tua_timer_cancel(tua_timer_t* timer) {
    if (timer == NULL) {
        return;
    }
    (void)InterlockedExchange(&timer->active, 0);
    // Ensure cleanup happens on the loop thread for consistency.
    if (tua_loop_post(timer->loop, tua_win32_timer_cleanup_task, timer) != TUA_OK) {
        tua_win32_timer_cleanup_common(timer, INVALID_HANDLE_VALUE);
    }
}

tua_handle_t tua_tcp_socket_handle(const tua_tcp_socket_t* sock) {
    (void)sock;
    return tua_handle_invalid();
}

tua_handle_t tua_tcp_listener_handle(const tua_tcp_listener_t* lst) {
    (void)lst;
    return tua_handle_invalid();
}

tua_fd_t tua_tcp_socket_fd(const tua_tcp_socket_t* sock) {
    (void)sock;
    return (tua_fd_t)0;
}

void tua_tcp_socket_close(tua_tcp_socket_t* sock) {
    (void)sock;
}

tua_err_t tua_tcp_connect_async(
    tua_loop_t* loop,
    tua_workqueue_t* wq,
    const char* host_utf8,
    const char* port_utf8,
    tua_deadline_t deadline,
    tua_tcp_connect_cb cb,
    void* arg
) {
    (void)loop;
    (void)wq;
    (void)host_utf8;
    (void)port_utf8;
    (void)deadline;
    (void)cb;
    (void)arg;
    return TUA_E_NOTSUP;
}

tua_err_t tua_tcp_read_async(
    tua_loop_t* loop,
    tua_tcp_socket_t* sock,
    uint8_t* buf,
    size_t len,
    tua_deadline_t deadline,
    tua_tcp_io_cb cb,
    void* arg
) {
    (void)loop;
    (void)sock;
    (void)buf;
    (void)len;
    (void)deadline;
    (void)cb;
    (void)arg;
    return TUA_E_NOTSUP;
}

tua_err_t tua_tcp_write_async(
    tua_loop_t* loop,
    tua_tcp_socket_t* sock,
    const uint8_t* buf,
    size_t len,
    tua_deadline_t deadline,
    tua_tcp_io_cb cb,
    void* arg
) {
    (void)loop;
    (void)sock;
    (void)buf;
    (void)len;
    (void)deadline;
    (void)cb;
    (void)arg;
    return TUA_E_NOTSUP;
}

tua_err_t tua_tcp_listen(
    const char* host_utf8,
    const char* port_utf8,
    int backlog,
    tua_tcp_listener_t** out
) {
    (void)host_utf8;
    (void)port_utf8;
    (void)backlog;
    (void)out;
    return TUA_E_NOTSUP;
}

int32_t tua_tcp_listener_local_port(const tua_tcp_listener_t* lst) {
    (void)lst;
    return 0;
}

void tua_tcp_listener_close(tua_tcp_listener_t* lst) {
    (void)lst;
}

tua_err_t tua_tcp_accept_start(
    tua_loop_t* loop,
    tua_tcp_listener_t* lst,
    tua_tcp_accept_cb cb,
    void* arg,
    tua_tcp_accept_t** out
) {
    (void)loop;
    (void)lst;
    (void)cb;
    (void)arg;
    (void)out;
    return TUA_E_NOTSUP;
}

void tua_tcp_accept_cancel(tua_tcp_accept_t* accept) {
    (void)accept;
}

#endif
