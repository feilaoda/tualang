#ifdef _WIN32

#include "rt/rt_loop.h"
#include "rt/rt_thread.h"

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
    (void)out;
    return TUA_E_NOTSUP;
}

void tua_loop_free(tua_loop_t* loop) {
    (void)loop;
}

tua_err_t tua_loop_run(tua_loop_t* loop) {
    (void)loop;
    return TUA_E_NOTSUP;
}

void tua_loop_stop(tua_loop_t* loop) {
    (void)loop;
}

tua_err_t tua_loop_post(tua_loop_t* loop, tua_task_fn fn, void* arg) {
    (void)loop;
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
    (void)loop;
    (void)out;
    (void)fd;
    (void)events;
    (void)fn;
    (void)arg;
    return TUA_E_NOTSUP;
}

void tua_io_cancel(tua_io_t* io) {
    (void)io;
}

tua_err_t tua_timer_start(
    tua_loop_t* loop,
    tua_timer_t** out,
    uint64_t delay_ms,
    uint64_t repeat_ms,
    tua_task_fn fn,
    void* arg
) {
    (void)loop;
    (void)out;
    (void)delay_ms;
    (void)repeat_ms;
    (void)fn;
    (void)arg;
    return TUA_E_NOTSUP;
}

void tua_timer_cancel(tua_timer_t* timer) {
    (void)timer;
}

#endif
