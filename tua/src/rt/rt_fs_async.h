#ifndef _TUA_RT_FS_ASYNC_H_
#define _TUA_RT_FS_ASYNC_H_

#include "rt/rt_err.h"

#include <stddef.h>
#include <stdint.h>

typedef struct tua_loop tua_loop_t;
typedef struct tua_workqueue tua_workqueue_t;

typedef void (*tua_fs_readfile_cb)(tua_err_t err, uint8_t* data, size_t len, void* arg);
typedef void (*tua_fs_writefile_cb)(tua_err_t err, void* arg);

tua_err_t tua_fs_readfile_async(
    tua_loop_t* loop,
    tua_workqueue_t* wq,
    const char* path_utf8,
    tua_fs_readfile_cb cb,
    void* arg
);

tua_err_t tua_fs_writefile_async(
    tua_loop_t* loop,
    tua_workqueue_t* wq,
    const char* path_utf8,
    const uint8_t* data,
    size_t len,
    tua_fs_writefile_cb cb,
    void* arg
);

#endif

