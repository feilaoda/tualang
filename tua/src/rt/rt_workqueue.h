#ifndef _TUA_RT_WORKQUEUE_H_
#define _TUA_RT_WORKQUEUE_H_

#include "rt/rt_err.h"

typedef struct tua_workqueue tua_workqueue_t;

tua_err_t tua_workqueue_create(tua_workqueue_t** out, int threads);
void tua_workqueue_free(tua_workqueue_t* wq);

tua_err_t tua_workqueue_post(tua_workqueue_t* wq, void (*fn)(void*), void* arg);

#endif

