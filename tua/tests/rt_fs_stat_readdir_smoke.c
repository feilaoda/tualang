#include "rt/rt.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    tua_loop_t* loop;
    tua_workqueue_t* wq;
    int pending;
    int ok;
} ctx_t;

static void done_one(ctx_t* ctx, int ok) {
    if (!ok) {
        ctx->ok = 0;
    }
    ctx->pending--;
    if (ctx->pending == 0) {
        tua_loop_stop(ctx->loop);
    }
}

static void on_stat(tua_err_t err, tua_fs_stat_t st, void* p) {
    ctx_t* ctx = (ctx_t*)p;
    int ok = (err == TUA_OK) && (st.kind == TUA_FS_FILE) && (st.size > 0);
    done_one(ctx, ok);
}

static void on_readdir(tua_err_t err, char** names, size_t count, void* p) {
    ctx_t* ctx = (ctx_t*)p;
    int ok = 0;
    if (err == TUA_OK && names != NULL && count > 0) {
        for (size_t i = 0; i < count; i++) {
            if (names[i] != NULL && strcmp(names[i], "README.md") == 0) {
                ok = 1;
                break;
            }
        }
    }
    tua_fs_dirlist_free(names, count);
    done_one(ctx, ok);
}

int main(void) {
    ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.ok = 1;
    ctx.pending = 2;

    tua_err_t err = tua_loop_create(&ctx.loop);
    if (err != TUA_OK) {
        fprintf(stderr, "tua_loop_create failed: %s\n", tua_err_name(err));
        return 1;
    }
    err = tua_workqueue_create(&ctx.wq, 1);
    if (err != TUA_OK) {
        fprintf(stderr, "tua_workqueue_create failed: %s\n", tua_err_name(err));
        tua_loop_free(ctx.loop);
        return 1;
    }

    err = tua_fs_stat_async(ctx.loop, ctx.wq, "README.md", on_stat, &ctx);
    if (err != TUA_OK) {
        fprintf(stderr, "tua_fs_stat_async failed: %s\n", tua_err_name(err));
        tua_workqueue_free(ctx.wq);
        tua_loop_free(ctx.loop);
        return 1;
    }
    err = tua_fs_readdir_async(ctx.loop, ctx.wq, ".", on_readdir, &ctx);
    if (err != TUA_OK) {
        fprintf(stderr, "tua_fs_readdir_async failed: %s\n", tua_err_name(err));
        tua_workqueue_free(ctx.wq);
        tua_loop_free(ctx.loop);
        return 1;
    }

    err = tua_loop_run(ctx.loop);
    if (err != TUA_OK) {
        fprintf(stderr, "tua_loop_run failed: %s\n", tua_err_name(err));
        ctx.ok = 0;
    }

    tua_workqueue_free(ctx.wq);
    tua_loop_free(ctx.loop);
    return ctx.ok ? 0 : 1;
}

