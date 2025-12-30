#include "rt/rt.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

typedef struct {
    tua_loop_t* loop;
    int rfd;
    int wfd;
} ctx_t;

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void write_byte(void* p) {
    ctx_t* ctx = (ctx_t*)p;
    uint8_t b = 7;
    (void)write(ctx->wfd, &b, 1);
}

static void on_read(int revents, void* p) {
    (void)revents;
    ctx_t* ctx = (ctx_t*)p;
    uint8_t buf[64];
    for (;;) {
        ssize_t n = read(ctx->rfd, buf, sizeof(buf));
        if (n > 0) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }
        break;
    }
    tua_loop_stop(ctx->loop);
}

int main(void) {
    tua_loop_t* loop = NULL;
    tua_err_t err = tua_loop_create(&loop);
    if (err != TUA_OK) {
        fprintf(stderr, "tua_loop_create failed: %s\n", tua_err_name(err));
        return 1;
    }

    int fds[2];
    if (pipe(fds) != 0) {
        fprintf(stderr, "pipe failed\n");
        tua_loop_free(loop);
        return 1;
    }
    ctx_t ctx = {
        .loop = loop,
        .rfd = fds[0],
        .wfd = fds[1],
    };
    (void)set_nonblocking(ctx.rfd);
    (void)set_nonblocking(ctx.wfd);

    err = tua_io_start_handle(loop, NULL, tua_handle_from_fd(ctx.rfd), TUA_IO_READ, on_read, &ctx);
    if (err != TUA_OK) {
        fprintf(stderr, "tua_io_start failed: %s\n", tua_err_name(err));
        close(ctx.rfd);
        close(ctx.wfd);
        tua_loop_free(loop);
        return 1;
    }

    err = tua_timer_start(loop, NULL, 10, 0, write_byte, &ctx);
    if (err != TUA_OK) {
        fprintf(stderr, "tua_timer_start failed: %s\n", tua_err_name(err));
        close(ctx.rfd);
        close(ctx.wfd);
        tua_loop_free(loop);
        return 1;
    }

    err = tua_loop_run(loop);
    if (err != TUA_OK) {
        fprintf(stderr, "tua_loop_run failed: %s\n", tua_err_name(err));
        close(ctx.rfd);
        close(ctx.wfd);
        tua_loop_free(loop);
        return 1;
    }

    close(ctx.rfd);
    close(ctx.wfd);
    tua_loop_free(loop);
    return 0;
}
