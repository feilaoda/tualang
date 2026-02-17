#include "rt/rt_platform.h"

#if defined(TUA_OS_POSIX)

#include "rt/rt_thread.h"

#include "rt/rt_alloc.h"

#include <errno.h>
#include <pthread.h>

struct tua_thread {
    pthread_t t;
    tua_thread_fn fn;
    void* arg;
    int started;
};

static void* tua_thread_trampoline(void* p) {
    tua_thread_t* th = (tua_thread_t*)p;
    th->fn(th->arg);
    return NULL;
}

tua_err_t tua_thread_create(tua_thread_t** out, tua_thread_fn fn, void* arg) {
    if (out == NULL || fn == NULL) {
        return TUA_E_INVALID;
    }
    tua_thread_t* th = (tua_thread_t*)tua_malloc(sizeof(*th));
    if (th == NULL) {
        return TUA_E_NOMEM;
    }
    th->fn = fn;
    th->arg = arg;
    th->started = 0;

    int rc = pthread_create(&th->t, NULL, tua_thread_trampoline, th);
    if (rc != 0) {
        tua_free(th);
        return tua_err_from_errno(rc);
    }
    th->started = 1;
    *out = th;
    return TUA_OK;
}

tua_err_t tua_thread_join(tua_thread_t* thread) {
    if (thread == NULL || !thread->started) {
        return TUA_E_INVALID;
    }
    int rc = pthread_join(thread->t, NULL);
    if (rc != 0) {
        return tua_err_from_errno(rc);
    }
    return TUA_OK;
}

void tua_thread_free(tua_thread_t* thread) {
    if (thread == NULL) {
        return;
    }
    tua_free(thread);
}

struct tua_mutex {
    pthread_mutex_t m;
};

tua_err_t tua_mutex_create(tua_mutex_t** out) {
    if (out == NULL) {
        return TUA_E_INVALID;
    }
    tua_mutex_t* m = (tua_mutex_t*)tua_malloc(sizeof(*m));
    if (m == NULL) {
        return TUA_E_NOMEM;
    }
    int rc = pthread_mutex_init(&m->m, NULL);
    if (rc != 0) {
        tua_free(m);
        return tua_err_from_errno(rc);
    }
    *out = m;
    return TUA_OK;
}

void tua_mutex_free(tua_mutex_t* mutex) {
    if (mutex == NULL) {
        return;
    }
    pthread_mutex_destroy(&mutex->m);
    tua_free(mutex);
}

tua_err_t tua_mutex_lock(tua_mutex_t* mutex) {
    if (mutex == NULL) {
        return TUA_E_INVALID;
    }
    int rc = pthread_mutex_lock(&mutex->m);
    if (rc != 0) {
        return tua_err_from_errno(rc);
    }
    return TUA_OK;
}

tua_err_t tua_mutex_unlock(tua_mutex_t* mutex) {
    if (mutex == NULL) {
        return TUA_E_INVALID;
    }
    int rc = pthread_mutex_unlock(&mutex->m);
    if (rc != 0) {
        return tua_err_from_errno(rc);
    }
    return TUA_OK;
}

struct tua_cond {
    pthread_cond_t c;
};

tua_err_t tua_cond_create(tua_cond_t** out) {
    if (out == NULL) {
        return TUA_E_INVALID;
    }
    tua_cond_t* c = (tua_cond_t*)tua_malloc(sizeof(*c));
    if (c == NULL) {
        return TUA_E_NOMEM;
    }
    int rc = pthread_cond_init(&c->c, NULL);
    if (rc != 0) {
        tua_free(c);
        return tua_err_from_errno(rc);
    }
    *out = c;
    return TUA_OK;
}

void tua_cond_free(tua_cond_t* cond) {
    if (cond == NULL) {
        return;
    }
    pthread_cond_destroy(&cond->c);
    tua_free(cond);
}

tua_err_t tua_cond_wait(tua_cond_t* cond, tua_mutex_t* mutex) {
    if (cond == NULL || mutex == NULL) {
        return TUA_E_INVALID;
    }
    int rc = pthread_cond_wait(&cond->c, &mutex->m);
    if (rc != 0) {
        return tua_err_from_errno(rc);
    }
    return TUA_OK;
}

tua_err_t tua_cond_signal(tua_cond_t* cond) {
    if (cond == NULL) {
        return TUA_E_INVALID;
    }
    int rc = pthread_cond_signal(&cond->c);
    if (rc != 0) {
        return tua_err_from_errno(rc);
    }
    return TUA_OK;
}

tua_err_t tua_cond_broadcast(tua_cond_t* cond) {
    if (cond == NULL) {
        return TUA_E_INVALID;
    }
    int rc = pthread_cond_broadcast(&cond->c);
    if (rc != 0) {
        return tua_err_from_errno(rc);
    }
    return TUA_OK;
}

#endif
