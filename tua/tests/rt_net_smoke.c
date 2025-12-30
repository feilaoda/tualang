#include "rt/rt.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    tua_loop_t* loop;
    tua_workqueue_t* wq;
    tua_tcp_listener_t* lst;
    tua_tcp_accept_t* accept;
    tua_tcp_socket_t* client;
    tua_tcp_socket_t* server;
    uint8_t server_buf[8];
    int ok;
} ctx_t;

static void on_server_read(tua_err_t err, size_t n, void* p) {
    ctx_t* ctx = (ctx_t*)p;
    if (err == TUA_OK && n == 5 && memcmp(ctx->server_buf, "hello", 5) == 0) {
        ctx->ok = 1;
    }
    if (ctx->server != NULL) {
        tua_tcp_socket_close(ctx->server);
        ctx->server = NULL;
    }
    tua_loop_stop(ctx->loop);
}

static void on_accept(tua_err_t err, tua_tcp_socket_t* sock, void* p) {
    ctx_t* ctx = (ctx_t*)p;
    if (err != TUA_OK || sock == NULL) {
        ctx->ok = 0;
        tua_loop_stop(ctx->loop);
        return;
    }
    ctx->server = sock;
    (void)tua_tcp_read_async(ctx->loop, sock, ctx->server_buf, 5, tua_deadline_after_ms(2000), on_server_read, ctx);
}

static void on_client_write(tua_err_t err, size_t n, void* p) {
    (void)p;
    if (err != TUA_OK || n != 5) {
        return;
    }
}

static void on_connect(tua_err_t err, tua_tcp_socket_t* sock, void* p) {
    ctx_t* ctx = (ctx_t*)p;
    if (err != TUA_OK || sock == NULL) {
        ctx->ok = 0;
        tua_loop_stop(ctx->loop);
        return;
    }
    ctx->client = sock;
    const uint8_t msg[5] = {'h', 'e', 'l', 'l', 'o'};
    (void)tua_tcp_write_async(ctx->loop, sock, msg, 5, tua_deadline_after_ms(2000), on_client_write, ctx);
}

int main(void) {
    ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    tua_err_t err = tua_loop_create(&ctx.loop);
    if (err != TUA_OK) {
        fprintf(stderr, "tua_loop_create: %s\n", tua_err_name(err));
        return 1;
    }
    err = tua_workqueue_create(&ctx.wq, 1);
    if (err != TUA_OK) {
        fprintf(stderr, "tua_workqueue_create: %s\n", tua_err_name(err));
        tua_loop_free(ctx.loop);
        return 1;
    }

    err = tua_tcp_listen(NULL, "0", 16, &ctx.lst);
    if (err != TUA_OK) {
        fprintf(stderr, "tua_tcp_listen: %s\n", tua_err_name(err));
        tua_workqueue_free(ctx.wq);
        tua_loop_free(ctx.loop);
        return 1;
    }

    err = tua_tcp_accept_start(ctx.loop, ctx.lst, on_accept, &ctx, &ctx.accept);
    if (err != TUA_OK) {
        fprintf(stderr, "tua_tcp_accept_start: %s\n", tua_err_name(err));
        tua_tcp_listener_close(ctx.lst);
        tua_workqueue_free(ctx.wq);
        tua_loop_free(ctx.loop);
        return 1;
    }

    char port[16];
    snprintf(port, sizeof(port), "%u", (unsigned)tua_tcp_listener_local_port(ctx.lst));
    err = tua_tcp_connect_async(ctx.loop, ctx.wq, "127.0.0.1", port, tua_deadline_after_ms(2000), on_connect, &ctx);
    if (err != TUA_OK) {
        fprintf(stderr, "tua_tcp_connect_async: %s\n", tua_err_name(err));
        tua_tcp_accept_cancel(ctx.accept);
        tua_tcp_listener_close(ctx.lst);
        tua_workqueue_free(ctx.wq);
        tua_loop_free(ctx.loop);
        return 1;
    }

    err = tua_loop_run(ctx.loop);
    if (err != TUA_OK) {
        fprintf(stderr, "tua_loop_run: %s\n", tua_err_name(err));
        ctx.ok = 0;
    }

    if (ctx.client != NULL) {
        tua_tcp_socket_close(ctx.client);
    }
    if (ctx.server != NULL) {
        tua_tcp_socket_close(ctx.server);
    }
    tua_tcp_accept_cancel(ctx.accept);
    tua_tcp_listener_close(ctx.lst);
    tua_workqueue_free(ctx.wq);
    tua_loop_free(ctx.loop);
    return ctx.ok ? 0 : 1;
}
