#ifdef _WIN32

#include "rt/rt_fs.h"
#include "rt/rt_loop.h"
#include "rt/rt_net.h"
#include "rt/rt_thread.h"

#include "tua_array.h"

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
