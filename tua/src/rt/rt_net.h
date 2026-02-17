#ifndef _TUA_RT_NET_H_
#define _TUA_RT_NET_H_

#include "rt/rt_cancel.h"
#include "rt/rt_err.h"
#include "rt/rt_handle.h"

#include <stddef.h>
#include <stdint.h>

typedef struct tua_loop tua_loop_t;
typedef struct tua_workqueue tua_workqueue_t;

typedef struct tua_tcp_socket tua_tcp_socket_t;
typedef struct tua_tcp_listener tua_tcp_listener_t;
typedef struct tua_tcp_accept tua_tcp_accept_t;

typedef void (*tua_tcp_connect_cb)(tua_err_t err, tua_tcp_socket_t* sock, void* arg);
typedef void (*tua_tcp_accept_cb)(tua_err_t err, tua_tcp_socket_t* sock, void* arg);
typedef void (*tua_tcp_io_cb)(tua_err_t err, size_t n, void* arg);

tua_handle_t tua_tcp_socket_handle(const tua_tcp_socket_t* sock);
tua_handle_t tua_tcp_listener_handle(const tua_tcp_listener_t* lst);

// Compatibility API (POSIX-only semantics): expose an underlying fd.
// Prefer `tua_tcp_socket_handle` to keep the API portable to Windows/IOCP.
tua_fd_t tua_tcp_socket_fd(const tua_tcp_socket_t* sock);
void tua_tcp_socket_close(tua_tcp_socket_t* sock);

tua_err_t tua_tcp_connect_async(
    tua_loop_t* loop,
    tua_workqueue_t* wq,
    const char* host_utf8,
    const char* port_utf8,
    tua_deadline_t deadline,
    tua_tcp_connect_cb cb,
    void* arg
);

tua_err_t tua_tcp_read_async(
    tua_loop_t* loop,
    tua_tcp_socket_t* sock,
    uint8_t* buf,
    size_t len,
    tua_deadline_t deadline,
    tua_tcp_io_cb cb,
    void* arg
);

tua_err_t tua_tcp_write_async(
    tua_loop_t* loop,
    tua_tcp_socket_t* sock,
    const uint8_t* buf,
    size_t len,
    tua_deadline_t deadline,
    tua_tcp_io_cb cb,
    void* arg
);

tua_err_t tua_tcp_listen(
    const char* host_utf8,
    const char* port_utf8,
    int backlog,
    tua_tcp_listener_t** out
);

int32_t tua_tcp_listener_local_port(const tua_tcp_listener_t* lst);
void tua_tcp_listener_close(tua_tcp_listener_t* lst);

tua_err_t tua_tcp_accept_start(
    tua_loop_t* loop,
    tua_tcp_listener_t* lst,
    tua_tcp_accept_cb cb,
    void* arg,
    tua_tcp_accept_t** out
);
void tua_tcp_accept_cancel(tua_tcp_accept_t* accept);

#endif
