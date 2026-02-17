#include "rt/rt_platform.h"

#if defined(TUA_OS_POSIX)

#include "rt/rt_fs_async.h"

#include "rt/rt_alloc.h"
#include "rt/rt_loop.h"
#include "rt/rt_workqueue.h"

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    tua_loop_t* loop;
    char* path;
    tua_fs_readfile_cb cb;
    void* arg;
    tua_err_t err;
    uint8_t* data;
    size_t len;
} tua_readfile_job_t;

static void tua_readfile_done_task(void* p) {
    tua_readfile_job_t* job = (tua_readfile_job_t*)p;
    job->cb(job->err, job->data, job->len, job->arg);
    tua_free(job->path);
    tua_free(job);
}

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

static void tua_readfile_worker(void* p) {
    tua_readfile_job_t* job = (tua_readfile_job_t*)p;

    int fd = open(job->path, O_RDONLY);
    if (fd < 0) {
        job->err = tua_err_from_errno(errno);
        job->data = NULL;
        job->len = 0;
    } else {
        job->err = tua_read_all_fd(fd, &job->data, &job->len);
        close(fd);
    }

    tua_err_t perr = tua_loop_post(job->loop, tua_readfile_done_task, job);
    if (perr != TUA_OK) {
        job->cb(job->err, job->data, job->len, job->arg);
        tua_free(job->path);
        tua_free(job);
    }
}

tua_err_t tua_fs_readfile_async(
    tua_loop_t* loop,
    tua_workqueue_t* wq,
    const char* path_utf8,
    tua_fs_readfile_cb cb,
    void* arg
) {
    if (loop == NULL || wq == NULL || path_utf8 == NULL || cb == NULL) {
        return TUA_E_INVALID;
    }

    size_t plen = strlen(path_utf8);
    char* path = (char*)tua_malloc(plen + 1);
    if (path == NULL) {
        return TUA_E_NOMEM;
    }
    memcpy(path, path_utf8, plen + 1);

    tua_readfile_job_t* job = (tua_readfile_job_t*)tua_malloc(sizeof(*job));
    if (job == NULL) {
        tua_free(path);
        return TUA_E_NOMEM;
    }
    job->loop = loop;
    job->path = path;
    job->cb = cb;
    job->arg = arg;
    job->err = TUA_OK;
    job->data = NULL;
    job->len = 0;

    tua_err_t err = tua_workqueue_post(wq, tua_readfile_worker, job);
    if (err != TUA_OK) {
        tua_free(path);
        tua_free(job);
        return err;
    }
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

typedef struct {
    tua_loop_t* loop;
    tua_fs_writefile_cb cb;
    void* arg;
    tua_err_t err;
    uint8_t* data_copy;
    size_t len;
    char* path;
} tua_writefile_job_t;

static void tua_writefile_done_task(void* p) {
    tua_writefile_job_t* job = (tua_writefile_job_t*)p;
    job->cb(job->err, job->arg);
    tua_free(job->data_copy);
    tua_free(job->path);
    tua_free(job);
}

static void tua_writefile_worker(void* p) {
    tua_writefile_job_t* job = (tua_writefile_job_t*)p;

    int fd = open(job->path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        job->err = tua_err_from_errno(errno);
    } else {
        job->err = tua_write_all_fd(fd, job->data_copy, job->len);
        close(fd);
    }

    tua_err_t perr = tua_loop_post(job->loop, tua_writefile_done_task, job);
    if (perr != TUA_OK) {
        job->cb(job->err, job->arg);
        tua_free(job->data_copy);
        tua_free(job->path);
        tua_free(job);
    }
}

tua_err_t tua_fs_writefile_async(
    tua_loop_t* loop,
    tua_workqueue_t* wq,
    const char* path_utf8,
    const uint8_t* data,
    size_t len,
    tua_fs_writefile_cb cb,
    void* arg
) {
    if (loop == NULL || wq == NULL || path_utf8 == NULL || cb == NULL) {
        return TUA_E_INVALID;
    }

    size_t plen = strlen(path_utf8);
    char* path = (char*)tua_malloc(plen + 1);
    if (path == NULL) {
        return TUA_E_NOMEM;
    }
    memcpy(path, path_utf8, plen + 1);

    uint8_t* copy = NULL;
    if (len > 0) {
        copy = (uint8_t*)tua_malloc(len);
        if (copy == NULL) {
            tua_free(path);
            return TUA_E_NOMEM;
        }
        memcpy(copy, data, len);
    }

    tua_writefile_job_t* job = (tua_writefile_job_t*)tua_malloc(sizeof(*job));
    if (job == NULL) {
        tua_free(copy);
        tua_free(path);
        return TUA_E_NOMEM;
    }
    job->loop = loop;
    job->cb = cb;
    job->arg = arg;
    job->err = TUA_OK;
    job->data_copy = copy;
    job->len = len;
    job->path = path;

    tua_err_t err = tua_workqueue_post(wq, tua_writefile_worker, job);
    if (err != TUA_OK) {
        tua_free(copy);
        tua_free(path);
        tua_free(job);
        return err;
    }
    return TUA_OK;
}

static uint64_t tua_timespec_to_ns(struct timespec ts) {
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

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

typedef struct {
    tua_loop_t* loop;
    char* path;
    tua_fs_stat_cb cb;
    void* arg;
    tua_err_t err;
    tua_fs_stat_t st;
} tua_stat_job_t;

static void tua_stat_done_task(void* p) {
    tua_stat_job_t* job = (tua_stat_job_t*)p;
    job->cb(job->err, job->st, job->arg);
    tua_free(job->path);
    tua_free(job);
}

static void tua_stat_worker(void* p) {
    tua_stat_job_t* job = (tua_stat_job_t*)p;
    memset(&job->st, 0, sizeof(job->st));
    job->st.kind = TUA_FS_UNKNOWN;

    struct stat st;
    if (stat(job->path, &st) != 0) {
        job->err = tua_err_from_errno(errno);
    } else {
        job->err = TUA_OK;
        job->st.size = (uint64_t)st.st_size;
        job->st.mode = (uint32_t)st.st_mode;
#if defined(TUA_OS_DARWIN)
        job->st.mtime_ns = tua_timespec_to_ns(st.st_mtimespec);
#else
        job->st.mtime_ns = tua_timespec_to_ns(st.st_mtim);
#endif
        if (S_ISREG(st.st_mode)) {
            job->st.kind = TUA_FS_FILE;
        } else if (S_ISDIR(st.st_mode)) {
            job->st.kind = TUA_FS_DIR;
        } else if (S_ISLNK(st.st_mode)) {
            job->st.kind = TUA_FS_SYMLINK;
        } else {
            job->st.kind = TUA_FS_UNKNOWN;
        }
    }

    tua_err_t perr = tua_loop_post(job->loop, tua_stat_done_task, job);
    if (perr != TUA_OK) {
        job->cb(job->err, job->st, job->arg);
        tua_free(job->path);
        tua_free(job);
    }
}

tua_err_t tua_fs_stat_async(
    tua_loop_t* loop,
    tua_workqueue_t* wq,
    const char* path_utf8,
    tua_fs_stat_cb cb,
    void* arg
) {
    if (loop == NULL || wq == NULL || path_utf8 == NULL || cb == NULL) {
        return TUA_E_INVALID;
    }
    size_t plen = strlen(path_utf8);
    char* path = (char*)tua_malloc(plen + 1);
    if (path == NULL) {
        return TUA_E_NOMEM;
    }
    memcpy(path, path_utf8, plen + 1);

    tua_stat_job_t* job = (tua_stat_job_t*)tua_malloc(sizeof(*job));
    if (job == NULL) {
        tua_free(path);
        return TUA_E_NOMEM;
    }
    job->loop = loop;
    job->path = path;
    job->cb = cb;
    job->arg = arg;
    job->err = TUA_OK;
    memset(&job->st, 0, sizeof(job->st));

    tua_err_t err = tua_workqueue_post(wq, tua_stat_worker, job);
    if (err != TUA_OK) {
        tua_free(path);
        tua_free(job);
        return err;
    }
    return TUA_OK;
}

typedef struct {
    tua_loop_t* loop;
    char* path;
    tua_fs_readdir_cb cb;
    void* arg;
    tua_err_t err;
    char** names;
    size_t count;
} tua_readdir_job_t;

static void tua_readdir_done_task(void* p) {
    tua_readdir_job_t* job = (tua_readdir_job_t*)p;
    job->cb(job->err, job->names, job->count, job->arg);
    tua_free(job->path);
    tua_free(job);
}

static int tua_is_dot_or_dotdot(const char* name) {
    return (name[0] == '.' && name[1] == '\0') || (name[0] == '.' && name[1] == '.' && name[2] == '\0');
}

static void tua_readdir_worker(void* p) {
    tua_readdir_job_t* job = (tua_readdir_job_t*)p;
    job->names = NULL;
    job->count = 0;

    DIR* d = opendir(job->path);
    if (d == NULL) {
        job->err = tua_err_from_errno(errno);
    } else {
        job->err = TUA_OK;
        size_t cap = 0;
        for (;;) {
            errno = 0;
            struct dirent* ent = readdir(d);
            if (ent == NULL) {
                if (errno != 0) {
                    job->err = tua_err_from_errno(errno);
                }
                break;
            }
            if (tua_is_dot_or_dotdot(ent->d_name)) {
                continue;
            }
            size_t nlen = strlen(ent->d_name);
            char* copy = (char*)tua_malloc(nlen + 1);
            if (copy == NULL) {
                job->err = TUA_E_NOMEM;
                break;
            }
            memcpy(copy, ent->d_name, nlen + 1);

            if (job->count == cap) {
                size_t newcap = cap == 0 ? 32 : cap * 2;
                char** nn = (char**)tua_realloc(job->names, newcap * sizeof(*nn));
                if (nn == NULL) {
                    tua_free(copy);
                    job->err = TUA_E_NOMEM;
                    break;
                }
                job->names = nn;
                cap = newcap;
            }
            job->names[job->count++] = copy;
        }
        closedir(d);

        if (job->err != TUA_OK) {
            tua_fs_dirlist_free(job->names, job->count);
            job->names = NULL;
            job->count = 0;
        }
    }

    tua_err_t perr = tua_loop_post(job->loop, tua_readdir_done_task, job);
    if (perr != TUA_OK) {
        job->cb(job->err, job->names, job->count, job->arg);
        if (job->names != NULL) {
            tua_fs_dirlist_free(job->names, job->count);
        }
        tua_free(job->path);
        tua_free(job);
    }
}

tua_err_t tua_fs_readdir_async(
    tua_loop_t* loop,
    tua_workqueue_t* wq,
    const char* path_utf8,
    tua_fs_readdir_cb cb,
    void* arg
) {
    if (loop == NULL || wq == NULL || path_utf8 == NULL || cb == NULL) {
        return TUA_E_INVALID;
    }
    size_t plen = strlen(path_utf8);
    char* path = (char*)tua_malloc(plen + 1);
    if (path == NULL) {
        return TUA_E_NOMEM;
    }
    memcpy(path, path_utf8, plen + 1);

    tua_readdir_job_t* job = (tua_readdir_job_t*)tua_malloc(sizeof(*job));
    if (job == NULL) {
        tua_free(path);
        return TUA_E_NOMEM;
    }
    job->loop = loop;
    job->path = path;
    job->cb = cb;
    job->arg = arg;
    job->err = TUA_OK;
    job->names = NULL;
    job->count = 0;

    tua_err_t err = tua_workqueue_post(wq, tua_readdir_worker, job);
    if (err != TUA_OK) {
        tua_free(path);
        tua_free(job);
        return err;
    }
    return TUA_OK;
}

#endif
