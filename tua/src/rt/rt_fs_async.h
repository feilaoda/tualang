#ifndef _TUA_RT_FS_ASYNC_H_
#define _TUA_RT_FS_ASYNC_H_

#include "rt/rt_err.h"

#include <stddef.h>
#include <stdint.h>

typedef struct tua_loop tua_loop_t;
typedef struct tua_workqueue tua_workqueue_t;

typedef void (*tua_fs_readfile_cb)(tua_err_t err, uint8_t* data, size_t len, void* arg);
typedef void (*tua_fs_writefile_cb)(tua_err_t err, void* arg);

typedef enum {
    TUA_FS_UNKNOWN = 0,
    TUA_FS_FILE = 1,
    TUA_FS_DIR = 2,
    TUA_FS_SYMLINK = 3,
} tua_fs_kind_t;

typedef struct {
    tua_fs_kind_t kind;
    uint64_t size;
    uint64_t mtime_ns;
    uint32_t mode;
} tua_fs_stat_t;

typedef void (*tua_fs_stat_cb)(tua_err_t err, tua_fs_stat_t st, void* arg);
typedef void (*tua_fs_readdir_cb)(tua_err_t err, char** names, size_t count, void* arg);

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

tua_err_t tua_fs_stat_async(
    tua_loop_t* loop,
    tua_workqueue_t* wq,
    const char* path_utf8,
    tua_fs_stat_cb cb,
    void* arg
);

tua_err_t tua_fs_readdir_async(
    tua_loop_t* loop,
    tua_workqueue_t* wq,
    const char* path_utf8,
    tua_fs_readdir_cb cb,
    void* arg
);

void tua_fs_dirlist_free(char** names, size_t count);

#endif
