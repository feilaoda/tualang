#include "rt/rt.h"

#include <stdio.h>

typedef struct {
    tua_loop_t* loop;
    int ok;
} ctx_t;

static void on_read(tua_err_t err, uint8_t* data, size_t len, void* p) {
    ctx_t* ctx = (ctx_t*)p;
    if (err == TUA_OK && data != NULL && len > 0) {
        ctx->ok = 1;
    } else {
        ctx->ok = 0;
    }
    tua_free(data);
    tua_loop_stop(ctx->loop);
}

int main(void) {
    tua_loop_t* loop = NULL;
    tua_err_t err = tua_loop_create(&loop);
    if (err != TUA_OK) {
        fprintf(stderr, "tua_loop_create failed: %s\n", tua_err_name(err));
        return 1;
    }

    tua_workqueue_t* wq = NULL;
    err = tua_workqueue_create(&wq, 1);
    if (err != TUA_OK) {
        fprintf(stderr, "tua_workqueue_create failed: %s\n", tua_err_name(err));
        tua_loop_free(loop);
        return 1;
    }

    ctx_t ctx = {.loop = loop, .ok = 0};
    err = tua_fs_readfile_async(loop, wq, "README.md", on_read, &ctx);
    if (err != TUA_OK) {
        fprintf(stderr, "tua_fs_readfile_async failed: %s\n", tua_err_name(err));
        tua_workqueue_free(wq);
        tua_loop_free(loop);
        return 1;
    }

    err = tua_loop_run(loop);
    if (err != TUA_OK) {
        fprintf(stderr, "tua_loop_run failed: %s\n", tua_err_name(err));
        tua_workqueue_free(wq);
        tua_loop_free(loop);
        return 1;
    }

    tua_workqueue_free(wq);
    tua_loop_free(loop);
    return ctx.ok ? 0 : 1;
}

