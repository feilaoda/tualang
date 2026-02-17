#include "rt/rt_alloc.h"
#include "rt/rt_config.h"
#include "rt/rt_state.h"
#include "rt/rt_workqueue.h"
#include "tua_map.h"

#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    int alloc_calls;
    int free_calls;
    int realloc_calls;
} alloc_stats;

static void* counting_alloc(void* ud, void* ptr, size_t old_sz, size_t new_sz) {
    (void)old_sz;
    alloc_stats* st = (alloc_stats*)ud;
    if (!st) return NULL;

    if (new_sz == 0) {
        st->free_calls++;
        free(ptr);
        return NULL;
    }
    if (ptr == NULL) {
        st->alloc_calls++;
        return malloc(new_sz);
    }
    st->realloc_calls++;
    return realloc(ptr, new_sz);
}

typedef struct {
    jmp_buf jb;
    char msg[1024];
} panic_ctx;

static void test_panic(void* ud, const char* msg) {
    panic_ctx* ctx = (panic_ctx*)ud;
    if (!ctx) abort();
    if (!msg) msg = "(null)";
    strncpy(ctx->msg, msg, sizeof(ctx->msg) - 1);
    ctx->msg[sizeof(ctx->msg) - 1] = '\0';
    longjmp(ctx->jb, 1);
}

static void require(int cond, const char* what) {
    if (cond) return;
    fprintf(stderr, "rt_state_smoke: FAILED: %s\n", what ? what : "(unknown)");
    exit(1);
}

typedef struct {
    tua_state* expected;
    volatile int* done;
    volatile int* ok;
} worker_arg;

static void worker_job(void* p) {
    worker_arg* a = (worker_arg*)p;
    if (a && a->ok) {
        *a->ok = (tua_state_get_current() == a->expected) ? 1 : 0;
    }
    void* m = tua_malloc(8);
    if (m) tua_free(m);
    if (a && a->done) *a->done = 1;
    if (a) tua_free(a);
}

int main(void) {
    // Default process-global config (CLI-style).
    tua_rt_configure(NULL);

    alloc_stats st1 = {0}, st2 = {0};
    panic_ctx p1;
    memset(&p1, 0, sizeof(p1));

    tua_config cfg1 = tua_config_default();
    cfg1.allocator.alloc = counting_alloc;
    cfg1.allocator.ud = &st1;
    cfg1.panic = test_panic;
    cfg1.panic_ud = &p1;

    tua_state* s1 = tua_state_new(&cfg1);
    require(s1 != NULL, "tua_state_new(cfg1)");

    tua_state_set_current(s1);
    int a1_before = st1.alloc_calls;
    void* p = tua_malloc(16);
    require(p != NULL, "tua_malloc under s1");
    tua_free(p);
    require(st1.alloc_calls > a1_before, "allocator from current state is used (s1)");

    // Verify panic uses TLS current loc + handler.
    if (setjmp(p1.jb) == 0) {
        tua_set_loc("smoke.tua", 12, 3);
        tua_panic("boom");
        require(0, "tua_panic should longjmp");
    } else {
        require(strstr(p1.msg, "smoke.tua:12:3") != NULL, "panic message contains loc from current state");
        require(strstr(p1.msg, "boom") != NULL, "panic message contains payload");
    }

    // Second state with a different allocator.
    tua_config cfg2 = tua_config_default();
    cfg2.allocator.alloc = counting_alloc;
    cfg2.allocator.ud = &st2;
    tua_state* s2 = tua_state_new(&cfg2);
    require(s2 != NULL, "tua_state_new(cfg2)");

    tua_state_set_current(s2);
    int a2_before = st2.alloc_calls;
    void* q = tua_malloc(32);
    require(q != NULL, "tua_malloc under s2");
    tua_free(q);
    require(st2.alloc_calls > a2_before, "allocator from current state is used (s2)");

    // Verify worker threads inherit the creating thread's current state.
    tua_workqueue_t* wq = NULL;
    require(tua_workqueue_create(&wq, 1) == TUA_OK, "tua_workqueue_create");
    volatile int done = 0;
    volatile int ok = 0;
    worker_arg* wa = (worker_arg*)tua_malloc(sizeof(*wa));
    require(wa != NULL, "alloc worker_arg");
    wa->expected = s2;
    wa->done = &done;
    wa->ok = &ok;
    require(tua_workqueue_post(wq, worker_job, wa) == TUA_OK, "tua_workqueue_post");
    for (int i = 0; i < 2000 && !done; i++) {
        usleep(1000);
    }
    require(done, "worker job completed");
    require(ok, "worker TLS current state matches");
    tua_workqueue_free(wq);

    // Ensure switching state stops using the previous allocator (best-effort signal).
    require(st1.alloc_calls > 0, "s1 allocator was used");
    require(st2.alloc_calls > 0, "s2 allocator was used");

    tua_state_free(s2);
    tua_state_set_current(NULL);
    tua_state_free(s1);

    return 0;
}
