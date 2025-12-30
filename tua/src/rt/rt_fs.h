#ifndef _TUA_RT_FS_H_
#define _TUA_RT_FS_H_

#include "rt/rt_err.h"

#include <stddef.h>
#include <stdint.h>

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

// Reads an entire file into a newly allocated, NUL-terminated buffer.
// On success: `*out_data` must be freed via `tua_free` and `*out_len` is set (bytes excluding trailing NUL).
tua_err_t tua_fs_readfile_alloc(const char* path_utf8, char** out_data, int32_t* out_len);

// Writes a NUL-terminated string (without the trailing NUL) to a file, truncating or creating it.
tua_err_t tua_fs_writefile_str(const char* path_utf8, const char* data_utf8);

// Stat with scalar out-params to keep FFI simple.
tua_err_t tua_fs_stat_simple(
    const char* path_utf8,
    int32_t* out_kind,
    int64_t* out_size,
    int64_t* out_mtime_ns,
    int32_t* out_mode
);

// Creates a directory. If it already exists, returns TUA_OK.
tua_err_t tua_fs_mkdir(const char* path_utf8, int32_t mode);

// Resolves an absolute path. Output is allocated with `tua_malloc` and must be freed via `tua_free`.
tua_err_t tua_fs_realpath_alloc(const char* path_utf8, char** out_path_utf8);

// Directory listing. `*out_names` is an array of `count` NUL-terminated strings.
// Free with `tua_fs_dirlist_free`.
tua_err_t tua_fs_readdir(const char* path_utf8, char*** out_names, size_t* out_count);
void tua_fs_dirlist_free(char** names, size_t count);

#endif

