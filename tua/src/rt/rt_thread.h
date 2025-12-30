#ifndef _TUA_RT_THREAD_H_
#define _TUA_RT_THREAD_H_

#include "rt/rt_err.h"

typedef struct tua_thread tua_thread_t;
typedef struct tua_mutex tua_mutex_t;
typedef struct tua_cond tua_cond_t;

typedef void (*tua_thread_fn)(void* arg);

tua_err_t tua_thread_create(tua_thread_t** out, tua_thread_fn fn, void* arg);
tua_err_t tua_thread_join(tua_thread_t* thread);
void tua_thread_free(tua_thread_t* thread);

tua_err_t tua_mutex_create(tua_mutex_t** out);
void tua_mutex_free(tua_mutex_t* mutex);
tua_err_t tua_mutex_lock(tua_mutex_t* mutex);
tua_err_t tua_mutex_unlock(tua_mutex_t* mutex);

tua_err_t tua_cond_create(tua_cond_t** out);
void tua_cond_free(tua_cond_t* cond);
tua_err_t tua_cond_wait(tua_cond_t* cond, tua_mutex_t* mutex);
tua_err_t tua_cond_signal(tua_cond_t* cond);
tua_err_t tua_cond_broadcast(tua_cond_t* cond);

#endif

