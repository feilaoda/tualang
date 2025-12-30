#include "rt/rt_platform.h"

#if defined(TUA_OS_POSIX)

#include "rt/rt_fs.h"

#include "rt/rt_alloc.h"

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static tua_err_t tua_read_all_fd(int fd, uint8_t** out, size_t* out_len) {
    size_t cap = 8192;
    size_t len = 0;
    uint8_t* buf = (uint8_t*)tua_malloc(cap);
    if (buf == NULL) {
        return TUA_E_NOMEM;
    }

    for (;;) {
        if (len == cap) {
            size_t newcap = cap * 2;
            if (newcap < cap) {
                tua_free(buf);
                return TUA_E_NOMEM;
            }
            uint8_t* nb = (uint8_t*)tua_realloc(buf, newcap);
            if (nb == NULL) {
                tua_free(buf);
                return TUA_E_NOMEM;
            }
            buf = nb;
            cap = newcap;
        }
        ssize_t n = read(fd, buf + len, cap - len);
        if (n > 0) {
            len += (size_t)n;
            continue;
        }
        if (n == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        tua_err_t err = tua_err_from_errno(errno);
        tua_free(buf);
        return err;
    }

    *out = buf;
    *out_len = len;
    return TUA_OK;
}

tua_err_t tua_fs_readfile_alloc(const char* path_utf8, char** out_data, int32_t* out_len) {
    if (path_utf8 == NULL || out_data == NULL || out_len == NULL) {
        return TUA_E_INVALID;
    }

    int fd = open(path_utf8, O_RDONLY);
    if (fd < 0) {
        return tua_err_from_errno(errno);
    }

    uint8_t* data = NULL;
    size_t len = 0;
    tua_err_t err = tua_read_all_fd(fd, &data, &len);
    close(fd);
    if (err != TUA_OK) {
        return err;
    }

    if (len > (size_t)INT32_MAX) {
        tua_free(data);
        return TUA_E_NOMEM;
    }

    // Ensure NUL-terminated.
    uint8_t* with_nul = (uint8_t*)tua_realloc(data, len + 1);
    if (with_nul == NULL) {
        tua_free(data);
        return TUA_E_NOMEM;
    }
    with_nul[len] = 0;

    *out_data = (char*)with_nul;
    *out_len = (int32_t)len;
    return TUA_OK;
}

static tua_err_t tua_write_all_fd(int fd, const uint8_t* data, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, data + off, len - off);
        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        return tua_err_from_errno(errno);
    }
    return TUA_OK;
}

tua_err_t tua_fs_writefile_str(const char* path_utf8, const char* data_utf8) {
    if (path_utf8 == NULL || data_utf8 == NULL) {
        return TUA_E_INVALID;
    }

    int fd = open(path_utf8, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return tua_err_from_errno(errno);
    }
    size_t len = strlen(data_utf8);
    tua_err_t err = tua_write_all_fd(fd, (const uint8_t*)data_utf8, len);
    close(fd);
    return err;
}

static uint64_t tua_timespec_to_ns(struct timespec ts) {
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

tua_err_t tua_fs_stat_simple(
    const char* path_utf8,
    int32_t* out_kind,
    int64_t* out_size,
    int64_t* out_mtime_ns,
    int32_t* out_mode
) {
    if (path_utf8 == NULL || out_kind == NULL || out_size == NULL || out_mtime_ns == NULL || out_mode == NULL) {
        return TUA_E_INVALID;
    }

    struct stat st;
    if (lstat(path_utf8, &st) != 0) {
        return tua_err_from_errno(errno);
    }

    *out_size = (int64_t)st.st_size;
    *out_mode = (int32_t)st.st_mode;
#if defined(TUA_OS_DARWIN)
    *out_mtime_ns = (int64_t)tua_timespec_to_ns(st.st_mtimespec);
#else
    *out_mtime_ns = (int64_t)tua_timespec_to_ns(st.st_mtim);
#endif

    if (S_ISREG(st.st_mode)) {
        *out_kind = (int32_t)TUA_FS_FILE;
    } else if (S_ISDIR(st.st_mode)) {
        *out_kind = (int32_t)TUA_FS_DIR;
    } else if (S_ISLNK(st.st_mode)) {
        *out_kind = (int32_t)TUA_FS_SYMLINK;
    } else {
        *out_kind = (int32_t)TUA_FS_UNKNOWN;
    }
    return TUA_OK;
}

tua_err_t tua_fs_mkdir(const char* path_utf8, int32_t mode) {
    if (path_utf8 == NULL) {
        return TUA_E_INVALID;
    }
    if (mkdir(path_utf8, (mode_t)mode) == 0) {
        return TUA_OK;
    }
    if (errno == EEXIST) {
        return TUA_OK;
    }
    return tua_err_from_errno(errno);
}

tua_err_t tua_fs_realpath_alloc(const char* path_utf8, char** out_path_utf8) {
    if (path_utf8 == NULL || out_path_utf8 == NULL) {
        return TUA_E_INVALID;
    }
    char* rp = realpath(path_utf8, NULL);
    if (rp == NULL) {
        return tua_err_from_errno(errno);
    }
    size_t n = strlen(rp);
    char* copy = (char*)tua_malloc(n + 1);
    if (copy == NULL) {
        free(rp);
        return TUA_E_NOMEM;
    }
    memcpy(copy, rp, n + 1);
    free(rp);
    *out_path_utf8 = copy;
    return TUA_OK;
}

tua_err_t tua_fs_readdir(const char* path_utf8, char*** out_names, size_t* out_count) {
    if (path_utf8 == NULL || out_names == NULL || out_count == NULL) {
        return TUA_E_INVALID;
    }

    DIR* d = opendir(path_utf8);
    if (d == NULL) {
        return tua_err_from_errno(errno);
    }

    size_t cap = 32;
    size_t count = 0;
    char** names = (char**)tua_malloc(sizeof(char*) * cap);
    if (names == NULL) {
        closedir(d);
        return TUA_E_NOMEM;
    }

    for (;;) {
        errno = 0;
        struct dirent* ent = readdir(d);
        if (ent == NULL) {
            if (errno != 0) {
                tua_err_t err = tua_err_from_errno(errno);
                closedir(d);
                tua_fs_dirlist_free(names, count);
                return err;
            }
            break;
        }
        const char* n = ent->d_name;
        if (n[0] == '.' && (n[1] == '\0' || (n[1] == '.' && n[2] == '\0'))) {
            continue;
        }

        if (count == cap) {
            size_t newcap = cap * 2;
            if (newcap < cap) {
                closedir(d);
                tua_fs_dirlist_free(names, count);
                return TUA_E_NOMEM;
            }
            char** nn = (char**)tua_realloc(names, sizeof(char*) * newcap);
            if (nn == NULL) {
                closedir(d);
                tua_fs_dirlist_free(names, count);
                return TUA_E_NOMEM;
            }
            names = nn;
            cap = newcap;
        }

        size_t sl = strlen(n);
        char* copy = (char*)tua_malloc(sl + 1);
        if (copy == NULL) {
            closedir(d);
            tua_fs_dirlist_free(names, count);
            return TUA_E_NOMEM;
        }
        memcpy(copy, n, sl + 1);
        names[count++] = copy;
    }

    closedir(d);
    *out_names = names;
    *out_count = count;
    return TUA_OK;
}

void tua_fs_dirlist_free(char** names, size_t count) {
    if (names == NULL) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        tua_free(names[i]);
    }
    tua_free(names);
}

#endif
