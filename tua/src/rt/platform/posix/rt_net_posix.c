#include "rt/rt_platform.h"

#if defined(TUA_OS_POSIX)

#include "rt/rt_net.h"

#include "rt/rt_alloc.h"
#include "rt/rt_loop.h"
#include "rt/rt_workqueue.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

struct tua_tcp_socket {
    tua_fd_t fd;
};

struct tua_tcp_listener {
    tua_fd_t fd;
    uint16_t port;
};

struct tua_tcp_accept {
    tua_loop_t* loop;
    tua_tcp_listener_t* lst;
    tua_tcp_accept_cb cb;
    void* arg;
    tua_io_t* io;
    int active;
};

tua_fd_t tua_tcp_socket_fd(const tua_tcp_socket_t* sock) {
    return sock == NULL ? (tua_fd_t)-1 : sock->fd;
}

void tua_tcp_socket_close(tua_tcp_socket_t* sock) {
    if (sock == NULL) {
        return;
    }
    if ((int)sock->fd >= 0) {
        close((int)sock->fd);
    }
    tua_free(sock);
}

static tua_err_t tua_set_nonblocking_cloexec(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return tua_err_from_errno(errno);
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        return tua_err_from_errno(errno);
    }

    int fdflags = fcntl(fd, F_GETFD, 0);
    if (fdflags < 0) {
        return tua_err_from_errno(errno);
    }
    if (fcntl(fd, F_SETFD, fdflags | FD_CLOEXEC) != 0) {
        return tua_err_from_errno(errno);
    }
    return TUA_OK;
}

typedef struct {
    tua_loop_t* loop;
    char* host;
    char* port;
    tua_deadline_t deadline;
    tua_tcp_connect_cb cb;
    void* arg;
} tua_connect_job_t;

typedef struct {
    tua_loop_t* loop;
    tua_deadline_t deadline;
    tua_tcp_connect_cb cb;
    void* arg;
    struct addrinfo* ai;
} tua_connect_state_t;

static void tua_connect_state_free(tua_connect_state_t* st) {
    if (st == NULL) {
        return;
    }
    if (st->ai != NULL) {
        freeaddrinfo(st->ai);
    }
    tua_free(st);
}

typedef struct {
    tua_connect_state_t* st;
    struct addrinfo* cur;
    int fd;
    tua_io_t* io;
    tua_timer_t* timer;
} tua_connect_attempt_t;

static void tua_connect_attempt_cleanup(tua_connect_attempt_t* at) {
    if (at->io != NULL) {
        tua_io_cancel(at->io);
        at->io = NULL;
    }
    if (at->timer != NULL) {
        tua_timer_cancel(at->timer);
        at->timer = NULL;
    }
    if (at->fd >= 0) {
        close(at->fd);
        at->fd = -1;
    }
}

static int tua_sockaddr_len(const struct addrinfo* ai) {
    return (int)ai->ai_addrlen;
}

static void tua_connect_try_next(void* p);

static void tua_connect_finish(tua_connect_state_t* st, tua_err_t err, int fd) {
    tua_tcp_socket_t* sock = NULL;
    if (err == TUA_OK) {
        sock = (tua_tcp_socket_t*)tua_malloc(sizeof(*sock));
        if (sock == NULL) {
            err = TUA_E_NOMEM;
            if (fd >= 0) {
                close(fd);
                fd = -1;
            }
        } else {
            sock->fd = (tua_fd_t)fd;
            fd = -1;
        }
    } else if (fd >= 0) {
        close(fd);
        fd = -1;
    }
    st->cb(err, sock, st->arg);
    tua_connect_state_free(st);
}

static void tua_connect_on_timeout(void* p) {
    tua_connect_attempt_t* at = (tua_connect_attempt_t*)p;
    tua_connect_state_t* st = at->st;
    tua_connect_attempt_cleanup(at);
    tua_free(at);
    tua_connect_finish(st, TUA_E_TIMEDOUT, -1);
}

static void tua_connect_on_writable(int revents, void* p) {
    tua_connect_attempt_t* at = (tua_connect_attempt_t*)p;
    tua_connect_state_t* st = at->st;

    if (revents & (TUA_IO_ERR | TUA_IO_HUP)) {
        tua_connect_attempt_cleanup(at);
        at->cur = at->cur->ai_next;
        tua_connect_try_next(at);
        return;
    }

    int soerr = 0;
    socklen_t slen = (socklen_t)sizeof(soerr);
    if (getsockopt(at->fd, SOL_SOCKET, SO_ERROR, &soerr, &slen) != 0) {
        soerr = errno;
    }
    if (soerr != 0) {
        tua_connect_attempt_cleanup(at);
        at->cur = at->cur->ai_next;
        tua_connect_try_next(at);
        return;
    }

    int fd = at->fd;
    at->fd = -1;
    if (at->io != NULL) {
        tua_io_cancel(at->io);
        at->io = NULL;
    }
    if (at->timer != NULL) {
        tua_timer_cancel(at->timer);
        at->timer = NULL;
    }
    tua_free(at);
    tua_connect_finish(st, TUA_OK, fd);
}

static void tua_connect_try_next(void* p) {
    tua_connect_attempt_t* at = (tua_connect_attempt_t*)p;
    tua_connect_state_t* st = at->st;

    if (tua_deadline_is_expired(st->deadline)) {
        tua_free(at);
        tua_connect_finish(st, TUA_E_TIMEDOUT, -1);
        return;
    }

    while (at->cur != NULL) {
        int fd = socket(at->cur->ai_family, at->cur->ai_socktype, at->cur->ai_protocol);
        if (fd < 0) {
            at->cur = at->cur->ai_next;
            continue;
        }
        (void)tua_set_nonblocking_cloexec(fd);

        int rc = connect(fd, at->cur->ai_addr, tua_sockaddr_len(at->cur));
        if (rc == 0) {
            tua_free(at);
            tua_connect_finish(st, TUA_OK, fd);
            return;
        }
        if (rc != 0 && errno != EINPROGRESS) {
            close(fd);
            at->cur = at->cur->ai_next;
            continue;
        }

        at->fd = fd;
        tua_err_t err = tua_io_start(st->loop, &at->io, (tua_fd_t)fd, TUA_IO_WRITE, tua_connect_on_writable, at);
        if (err != TUA_OK) {
            tua_connect_attempt_cleanup(at);
            at->cur = at->cur->ai_next;
            continue;
        }

        if (!tua_deadline_is_none(st->deadline)) {
            uint64_t delay_ms = tua_deadline_to_delay_ms(st->deadline);
            err = tua_timer_start(st->loop, &at->timer, delay_ms, 0, tua_connect_on_timeout, at);
            if (err != TUA_OK) {
                tua_connect_attempt_cleanup(at);
                at->cur = at->cur->ai_next;
                continue;
            }
        }
        return;
    }

    tua_free(at);
    tua_connect_finish(st, TUA_E_NOTFOUND, -1);
}

static void tua_connect_resolved(void* p) {
    tua_connect_state_t* st = (tua_connect_state_t*)p;

    if (tua_deadline_is_expired(st->deadline)) {
        tua_connect_finish(st, TUA_E_TIMEDOUT, -1);
        return;
    }

    if (st->ai == NULL) {
        tua_connect_finish(st, TUA_E_NOTFOUND, -1);
        return;
    }

    tua_connect_attempt_t* at = (tua_connect_attempt_t*)tua_malloc(sizeof(*at));
    if (at == NULL) {
        tua_connect_finish(st, TUA_E_NOMEM, -1);
        return;
    }
    at->st = st;
    at->cur = st->ai;
    at->fd = -1;
    at->io = NULL;
    at->timer = NULL;

    tua_connect_try_next(at);
}

static void tua_connect_worker(void* p) {
    tua_connect_job_t* job = (tua_connect_job_t*)p;

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    struct addrinfo* res = NULL;
    int rc = getaddrinfo(job->host, job->port, &hints, &res);
    if (rc != 0) {
        res = NULL;
    }

    tua_connect_state_t* st = (tua_connect_state_t*)tua_malloc(sizeof(*st));
    if (st == NULL) {
        if (res != NULL) {
            freeaddrinfo(res);
        }
        job->cb(TUA_E_NOMEM, NULL, job->arg);
        tua_free(job->host);
        tua_free(job->port);
        tua_free(job);
        return;
    }
    st->loop = job->loop;
    st->deadline = job->deadline;
    st->cb = job->cb;
    st->arg = job->arg;
    st->ai = res;

    tua_err_t perr = tua_loop_post(job->loop, tua_connect_resolved, st);
    if (perr != TUA_OK) {
        tua_connect_state_free(st);
        job->cb(perr, NULL, job->arg);
    }

    tua_free(job->host);
    tua_free(job->port);
    tua_free(job);
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
    if (loop == NULL || wq == NULL || host_utf8 == NULL || port_utf8 == NULL || cb == NULL) {
        return TUA_E_INVALID;
    }

    size_t hlen = strlen(host_utf8);
    size_t plen = strlen(port_utf8);
    char* host = (char*)tua_malloc(hlen + 1);
    char* port = (char*)tua_malloc(plen + 1);
    if (host == NULL || port == NULL) {
        tua_free(host);
        tua_free(port);
        return TUA_E_NOMEM;
    }
    memcpy(host, host_utf8, hlen + 1);
    memcpy(port, port_utf8, plen + 1);

    tua_connect_job_t* job = (tua_connect_job_t*)tua_malloc(sizeof(*job));
    if (job == NULL) {
        tua_free(host);
        tua_free(port);
        return TUA_E_NOMEM;
    }
    job->loop = loop;
    job->host = host;
    job->port = port;
    job->deadline = deadline;
    job->cb = cb;
    job->arg = arg;

    return tua_workqueue_post(wq, tua_connect_worker, job);
}

typedef struct {
    tua_loop_t* loop;
    tua_tcp_socket_t* sock;
    uint8_t* buf;
    size_t len;
    tua_deadline_t deadline;
    tua_tcp_io_cb cb;
    void* arg;
    tua_io_t* io;
    tua_timer_t* timer;
    int done;
} tua_read_op_t;

static void tua_read_finish(tua_read_op_t* op, tua_err_t err, size_t n) {
    if (op->io != NULL) {
        tua_io_cancel(op->io);
        op->io = NULL;
    }
    if (op->timer != NULL) {
        tua_timer_cancel(op->timer);
        op->timer = NULL;
    }
    op->done = 1;
    op->cb(err, n, op->arg);
    tua_free(op);
}

static void tua_read_on_timeout(void* p) {
    tua_read_op_t* op = (tua_read_op_t*)p;
    if (op->done) {
        return;
    }
    tua_read_finish(op, TUA_E_TIMEDOUT, 0);
}

static void tua_read_on_ready(int revents, void* p) {
    tua_read_op_t* op = (tua_read_op_t*)p;
    if (op->done) {
        return;
    }
    if (revents & (TUA_IO_ERR | TUA_IO_HUP)) {
        tua_read_finish(op, TUA_E_IO, 0);
        return;
    }
    for (;;) {
        ssize_t n = read((int)op->sock->fd, op->buf, op->len);
        if (n > 0) {
            tua_read_finish(op, TUA_OK, (size_t)n);
            return;
        }
        if (n == 0) {
            tua_read_finish(op, TUA_E_IO, 0);
            return;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
        tua_read_finish(op, tua_err_from_errno(errno), 0);
        return;
    }
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
    if (loop == NULL || sock == NULL || buf == NULL || cb == NULL) {
        return TUA_E_INVALID;
    }
    if (len == 0) {
        cb(TUA_OK, 0, arg);
        return TUA_OK;
    }

    for (;;) {
        ssize_t n = read((int)sock->fd, buf, len);
        if (n > 0) {
            cb(TUA_OK, (size_t)n, arg);
            return TUA_OK;
        }
        if (n == 0) {
            cb(TUA_E_IO, 0, arg);
            return TUA_OK;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        cb(tua_err_from_errno(errno), 0, arg);
        return TUA_OK;
    }

    tua_read_op_t* op = (tua_read_op_t*)tua_malloc(sizeof(*op));
    if (op == NULL) {
        return TUA_E_NOMEM;
    }
    op->loop = loop;
    op->sock = sock;
    op->buf = buf;
    op->len = len;
    op->deadline = deadline;
    op->cb = cb;
    op->arg = arg;
    op->io = NULL;
    op->timer = NULL;
    op->done = 0;

    tua_err_t err = tua_io_start(loop, &op->io, sock->fd, TUA_IO_READ, tua_read_on_ready, op);
    if (err != TUA_OK) {
        tua_free(op);
        return err;
    }

    if (!tua_deadline_is_none(deadline)) {
        uint64_t delay_ms = tua_deadline_to_delay_ms(deadline);
        err = tua_timer_start(loop, &op->timer, delay_ms, 0, tua_read_on_timeout, op);
        if (err != TUA_OK) {
            tua_io_cancel(op->io);
            tua_free(op);
            return err;
        }
    }
    return TUA_OK;
}

typedef struct {
    tua_loop_t* loop;
    tua_tcp_socket_t* sock;
    const uint8_t* buf;
    size_t len;
    size_t off;
    tua_deadline_t deadline;
    tua_tcp_io_cb cb;
    void* arg;
    tua_io_t* io;
    tua_timer_t* timer;
    int done;
} tua_write_op_t;

static void tua_write_finish(tua_write_op_t* op, tua_err_t err, size_t n) {
    if (op->io != NULL) {
        tua_io_cancel(op->io);
        op->io = NULL;
    }
    if (op->timer != NULL) {
        tua_timer_cancel(op->timer);
        op->timer = NULL;
    }
    op->done = 1;
    op->cb(err, n, op->arg);
    tua_free(op);
}

static void tua_write_on_timeout(void* p) {
    tua_write_op_t* op = (tua_write_op_t*)p;
    if (op->done) {
        return;
    }
    tua_write_finish(op, TUA_E_TIMEDOUT, op->off);
}

static void tua_write_on_ready(int revents, void* p) {
    tua_write_op_t* op = (tua_write_op_t*)p;
    if (op->done) {
        return;
    }
    if (revents & (TUA_IO_ERR | TUA_IO_HUP)) {
        tua_write_finish(op, TUA_E_IO, op->off);
        return;
    }
    while (op->off < op->len) {
        ssize_t n = write((int)op->sock->fd, op->buf + op->off, op->len - op->off);
        if (n > 0) {
            op->off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        }
        tua_write_finish(op, tua_err_from_errno(errno), op->off);
        return;
    }
    tua_write_finish(op, TUA_OK, op->off);
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
    if (loop == NULL || sock == NULL || buf == NULL || cb == NULL) {
        return TUA_E_INVALID;
    }
    if (len == 0) {
        cb(TUA_OK, 0, arg);
        return TUA_OK;
    }

    size_t off = 0;
    while (off < len) {
        ssize_t n = write((int)sock->fd, buf + off, len - off);
        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }
        cb(tua_err_from_errno(errno), off, arg);
        return TUA_OK;
    }
    if (off == len) {
        cb(TUA_OK, off, arg);
        return TUA_OK;
    }

    tua_write_op_t* op = (tua_write_op_t*)tua_malloc(sizeof(*op));
    if (op == NULL) {
        return TUA_E_NOMEM;
    }
    op->loop = loop;
    op->sock = sock;
    op->buf = buf;
    op->len = len;
    op->off = off;
    op->deadline = deadline;
    op->cb = cb;
    op->arg = arg;
    op->io = NULL;
    op->timer = NULL;
    op->done = 0;

    tua_err_t err = tua_io_start(loop, &op->io, sock->fd, TUA_IO_WRITE, tua_write_on_ready, op);
    if (err != TUA_OK) {
        tua_free(op);
        return err;
    }
    if (!tua_deadline_is_none(deadline)) {
        uint64_t delay_ms = tua_deadline_to_delay_ms(deadline);
        err = tua_timer_start(loop, &op->timer, delay_ms, 0, tua_write_on_timeout, op);
        if (err != TUA_OK) {
            tua_io_cancel(op->io);
            tua_free(op);
            return err;
        }
    }

    return TUA_OK;
}

tua_err_t tua_tcp_listen(
    const char* host_utf8,
    const char* port_utf8,
    int backlog,
    tua_tcp_listener_t** out
) {
    if (out == NULL || port_utf8 == NULL) {
        return TUA_E_INVALID;
    }
    if (backlog <= 0) {
        backlog = 128;
    }

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = host_utf8 == NULL ? AI_PASSIVE : 0;

    struct addrinfo* res = NULL;
    int rc = getaddrinfo(host_utf8, port_utf8, &hints, &res);
    if (rc != 0 || res == NULL) {
        return TUA_E_NOTFOUND;
    }

    int fd = -1;
    for (struct addrinfo* ai = res; ai != NULL; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            continue;
        }
        int yes = 1;
        (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, (socklen_t)sizeof(yes));
#ifdef SO_REUSEPORT
        (void)setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &yes, (socklen_t)sizeof(yes));
#endif
        if (ai->ai_family == AF_INET6) {
            int no = 0;
            (void)setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &no, (socklen_t)sizeof(no));
        }
        if (bind(fd, ai->ai_addr, (socklen_t)ai->ai_addrlen) != 0) {
            close(fd);
            fd = -1;
            continue;
        }
        if (listen(fd, backlog) != 0) {
            close(fd);
            fd = -1;
            continue;
        }
        (void)tua_set_nonblocking_cloexec(fd);
        break;
    }
    freeaddrinfo(res);

    if (fd < 0) {
        return TUA_E_IO;
    }

    struct sockaddr_storage ss;
    socklen_t slen = (socklen_t)sizeof(ss);
    memset(&ss, 0, sizeof(ss));
    uint16_t port = 0;
    if (getsockname(fd, (struct sockaddr*)&ss, &slen) == 0) {
        if (ss.ss_family == AF_INET) {
            port = ntohs(((struct sockaddr_in*)&ss)->sin_port);
        } else if (ss.ss_family == AF_INET6) {
            port = ntohs(((struct sockaddr_in6*)&ss)->sin6_port);
        }
    }

    tua_tcp_listener_t* lst = (tua_tcp_listener_t*)tua_malloc(sizeof(*lst));
    if (lst == NULL) {
        close(fd);
        return TUA_E_NOMEM;
    }
    lst->fd = (tua_fd_t)fd;
    lst->port = port;
    *out = lst;
    return TUA_OK;
}

int32_t tua_tcp_listener_local_port(const tua_tcp_listener_t* lst) {
    return lst == NULL ? 0 : (int32_t)lst->port;
}

void tua_tcp_listener_close(tua_tcp_listener_t* lst) {
    if (lst == NULL) {
        return;
    }
    if ((int)lst->fd >= 0) {
        close((int)lst->fd);
    }
    tua_free(lst);
}

static void tua_accept_on_ready(int revents, void* p) {
    (void)revents;
    tua_tcp_accept_t* ac = (tua_tcp_accept_t*)p;
    if (!ac->active) {
        return;
    }

    for (;;) {
        struct sockaddr_storage ss;
        socklen_t slen = (socklen_t)sizeof(ss);
        int cfd = accept((int)ac->lst->fd, (struct sockaddr*)&ss, &slen);
        if (cfd >= 0) {
            (void)tua_set_nonblocking_cloexec(cfd);
            tua_tcp_socket_t* sock = (tua_tcp_socket_t*)tua_malloc(sizeof(*sock));
            if (sock == NULL) {
                close(cfd);
                ac->cb(TUA_E_NOMEM, NULL, ac->arg);
                continue;
            }
            sock->fd = (tua_fd_t)cfd;
            ac->cb(TUA_OK, sock, ac->arg);
            continue;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
        ac->cb(tua_err_from_errno(errno), NULL, ac->arg);
        return;
    }
}

tua_err_t tua_tcp_accept_start(
    tua_loop_t* loop,
    tua_tcp_listener_t* lst,
    tua_tcp_accept_cb cb,
    void* arg,
    tua_tcp_accept_t** out
) {
    if (loop == NULL || lst == NULL || cb == NULL) {
        return TUA_E_INVALID;
    }
    tua_tcp_accept_t* ac = (tua_tcp_accept_t*)tua_malloc(sizeof(*ac));
    if (ac == NULL) {
        return TUA_E_NOMEM;
    }
    ac->loop = loop;
    ac->lst = lst;
    ac->cb = cb;
    ac->arg = arg;
    ac->io = NULL;
    ac->active = 1;

    tua_err_t err = tua_io_start(loop, &ac->io, lst->fd, TUA_IO_READ, tua_accept_on_ready, ac);
    if (err != TUA_OK) {
        tua_free(ac);
        return err;
    }
    if (out != NULL) {
        *out = ac;
    }
    return TUA_OK;
}

void tua_tcp_accept_cancel(tua_tcp_accept_t* accept) {
    if (accept == NULL) {
        return;
    }
    accept->active = 0;
    if (accept->io != NULL) {
        tua_io_cancel(accept->io);
        accept->io = NULL;
    }
    tua_free(accept);
}

#endif
