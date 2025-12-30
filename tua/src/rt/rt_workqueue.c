#include "rt/rt_workqueue.h"

#include "rt/rt_alloc.h"
#include "rt/rt_thread.h"

typedef struct tua_work_item {
    void (*fn)(void*);
    void* arg;
    struct tua_work_item* next;
} tua_work_item_t;

struct tua_workqueue {
    tua_mutex_t* mu;
    tua_cond_t* cv;
    tua_thread_t** threads;
    int nthreads;

    tua_work_item_t* head;
    tua_work_item_t* tail;
    int stop;
};

static void tua_worker_main(void* p) {
    tua_workqueue_t* wq = (tua_workqueue_t*)p;
    for (;;) {
        tua_work_item_t* item = NULL;

        tua_mutex_lock(wq->mu);
        while (!wq->stop && wq->head == NULL) {
            tua_cond_wait(wq->cv, wq->mu);
        }
        if (wq->stop && wq->head == NULL) {
            tua_mutex_unlock(wq->mu);
            return;
        }
        item = wq->head;
        wq->head = item->next;
        if (wq->head == NULL) {
            wq->tail = NULL;
        }
        tua_mutex_unlock(wq->mu);

        item->fn(item->arg);
        tua_free(item);
    }
}

tua_err_t tua_workqueue_create(tua_workqueue_t** out, int threads) {
    if (out == NULL) {
        return TUA_E_INVALID;
    }
    if (threads <= 0) {
        threads = 1;
    }

    tua_workqueue_t* wq = (tua_workqueue_t*)tua_malloc(sizeof(*wq));
    if (wq == NULL) {
        return TUA_E_NOMEM;
    }
    wq->mu = NULL;
    wq->cv = NULL;
    wq->threads = NULL;
    wq->nthreads = threads;
    wq->head = NULL;
    wq->tail = NULL;
    wq->stop = 0;

    tua_err_t err = tua_mutex_create(&wq->mu);
    if (err != TUA_OK) {
        tua_free(wq);
        return err;
    }
    err = tua_cond_create(&wq->cv);
    if (err != TUA_OK) {
        tua_mutex_free(wq->mu);
        tua_free(wq);
        return err;
    }

    wq->threads = (tua_thread_t**)tua_malloc(sizeof(*wq->threads) * (size_t)threads);
    if (wq->threads == NULL) {
        tua_cond_free(wq->cv);
        tua_mutex_free(wq->mu);
        tua_free(wq);
        return TUA_E_NOMEM;
    }
    for (int i = 0; i < threads; i++) {
        wq->threads[i] = NULL;
    }

    for (int i = 0; i < threads; i++) {
        err = tua_thread_create(&wq->threads[i], tua_worker_main, wq);
        if (err != TUA_OK) {
            wq->nthreads = i;
            tua_workqueue_free(wq);
            return err;
        }
    }

    *out = wq;
    return TUA_OK;
}

void tua_workqueue_free(tua_workqueue_t* wq) {
    if (wq == NULL) {
        return;
    }

    tua_mutex_lock(wq->mu);
    wq->stop = 1;
    tua_cond_broadcast(wq->cv);
    tua_mutex_unlock(wq->mu);

    for (int i = 0; i < wq->nthreads; i++) {
        if (wq->threads[i] != NULL) {
            tua_thread_join(wq->threads[i]);
            tua_thread_free(wq->threads[i]);
        }
    }

    tua_mutex_lock(wq->mu);
    tua_work_item_t* it = wq->head;
    while (it != NULL) {
        tua_work_item_t* next = it->next;
        tua_free(it);
        it = next;
    }
    wq->head = NULL;
    wq->tail = NULL;
    tua_mutex_unlock(wq->mu);

    tua_free(wq->threads);
    tua_cond_free(wq->cv);
    tua_mutex_free(wq->mu);
    tua_free(wq);
}

tua_err_t tua_workqueue_post(tua_workqueue_t* wq, void (*fn)(void*), void* arg) {
    if (wq == NULL || fn == NULL) {
        return TUA_E_INVALID;
    }
    tua_work_item_t* item = (tua_work_item_t*)tua_malloc(sizeof(*item));
    if (item == NULL) {
        return TUA_E_NOMEM;
    }
    item->fn = fn;
    item->arg = arg;
    item->next = NULL;

    tua_mutex_lock(wq->mu);
    if (wq->stop) {
        tua_mutex_unlock(wq->mu);
        tua_free(item);
        return TUA_E_CANCELED;
    }
    if (wq->tail == NULL) {
        wq->head = item;
        wq->tail = item;
    } else {
        wq->tail->next = item;
        wq->tail = item;
    }
    tua_cond_signal(wq->cv);
    tua_mutex_unlock(wq->mu);
    return TUA_OK;
}

