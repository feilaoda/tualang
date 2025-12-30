#ifndef _TUA_RT_LOOP_H_
#define _TUA_RT_LOOP_H_

#include "rt/rt_err.h"
#include "rt/rt_handle.h"

#include <stdint.h>

typedef struct tua_loop tua_loop_t;
typedef struct tua_timer tua_timer_t;
typedef struct tua_io tua_io_t;

typedef void (*tua_task_fn)(void* arg);
typedef void (*tua_io_fn)(int revents, void* arg);

enum {
    TUA_IO_READ = 1 << 0,
    TUA_IO_WRITE = 1 << 1,
    TUA_IO_ERR = 1 << 2,
    TUA_IO_HUP = 1 << 3,
};

tua_err_t tua_loop_create(tua_loop_t** out);
void tua_loop_free(tua_loop_t* loop);

tua_err_t tua_loop_run(tua_loop_t* loop);
void tua_loop_stop(tua_loop_t* loop);

tua_err_t tua_loop_post(tua_loop_t* loop, tua_task_fn fn, void* arg);

tua_err_t tua_io_start(
    tua_loop_t* loop,
    tua_io_t** out,
    tua_fd_t fd,
    int events,
    tua_io_fn fn,
    void* arg
);
void tua_io_cancel(tua_io_t* io);

tua_err_t tua_timer_start(
    tua_loop_t* loop,
    tua_timer_t** out,
    uint64_t delay_ms,
    uint64_t repeat_ms,
    tua_task_fn fn,
    void* arg
);
void tua_timer_cancel(tua_timer_t* timer);

#endif
