#include "rt/rt_platform.h"

#if defined(TUA_RT_LOOP_BACKEND_KQUEUE)

#include "rt/rt_loop.h"

#include "rt/rt_alloc.h"
#include "rt/rt_thread.h"
#include "rt/rt_time.h"

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <sys/event.h>
#include <sys/time.h>
#include <unistd.h>

typedef struct tua_task {
    tua_task_fn fn;
    void* arg;
    struct tua_task* next;
} tua_task_t;

struct tua_timer {
    struct tua_loop* loop;
    uint64_t due_ns;
    uint64_t repeat_ns;
    tua_task_fn fn;
    void* arg;
    int active;
    struct tua_timer* next;
};

struct tua_io {
    struct tua_loop* loop;
    tua_handle_t handle;
    int fd;
    int events;
    tua_io_fn fn;
    void* arg;
    int active;
    struct tua_io* next;
};

struct tua_loop {
    tua_mutex_t* mu;
    int wake_r;
    int wake_w;
    int kq;
    atomic_int stop;

    tua_task_t* task_head;
    tua_task_t* task_tail;

    tua_timer_t* timers;
    tua_io_t* ios;
};

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

static tua_err_t tua_set_cloexec(int fd) {
    int fdflags = fcntl(fd, F_GETFD, 0);
    if (fdflags < 0) {
        return tua_err_from_errno(errno);
    }
    if (fcntl(fd, F_SETFD, fdflags | FD_CLOEXEC) != 0) {
        return tua_err_from_errno(errno);
    }
    return TUA_OK;
}

static tua_err_t tua_loop_wakeup(tua_loop_t* loop) {
    uint8_t b = 1;
    ssize_t n = write(loop->wake_w, &b, 1);
    if (n == 1) {
        return TUA_OK;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return TUA_OK;
    }
    if (n < 0 && errno == EINTR) {
        return TUA_OK;
    }
    return TUA_E_IO;
}

static void tua_loop_drain_wakeup(tua_loop_t* loop) {
    uint8_t buf[128];
    for (;;) {
        ssize_t n = read(loop->wake_r, buf, sizeof(buf));
        if (n > 0) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        return;
    }
}

static void tua_timer_insert_sorted(tua_loop_t* loop, tua_timer_t* timer) {
    tua_timer_t** cur = &loop->timers;
    while (*cur != NULL && (*cur)->due_ns <= timer->due_ns) {
        cur = &(*cur)->next;
    }
    timer->next = *cur;
    *cur = timer;
}

static void tua_loop_prune_timers_locked(tua_loop_t* loop) {
    tua_timer_t** cur = &loop->timers;
    while (*cur != NULL) {
        if ((*cur)->active) {
            cur = &(*cur)->next;
            continue;
        }
        tua_timer_t* dead = *cur;
        *cur = dead->next;
        tua_free(dead);
    }
}

static void tua_loop_prune_ios_locked(tua_loop_t* loop) {
    tua_io_t** cur = &loop->ios;
    while (*cur != NULL) {
        if ((*cur)->active) {
            cur = &(*cur)->next;
            continue;
        }
        tua_io_t* dead = *cur;
        *cur = dead->next;
        tua_free(dead);
    }
}

static void tua_loop_run_tasks(tua_loop_t* loop) {
    tua_task_t* head = NULL;
    tua_mutex_lock(loop->mu);
    head = loop->task_head;
    loop->task_head = NULL;
    loop->task_tail = NULL;
    tua_mutex_unlock(loop->mu);

    while (head != NULL) {
        tua_task_t* next = head->next;
        head->fn(head->arg);
        tua_free(head);
        head = next;
    }
}

static int tua_loop_collect_due_timers(tua_loop_t* loop, tua_timer_t** out_due) {
    *out_due = NULL;

    uint64_t now = tua_time_mono_ns();
    tua_mutex_lock(loop->mu);
    tua_loop_prune_timers_locked(loop);
    while (loop->timers != NULL) {
        tua_timer_t* t = loop->timers;
        if (t->due_ns > now) {
            break;
        }
        loop->timers = t->next;
        t->next = *out_due;
        *out_due = t;
    }
    tua_mutex_unlock(loop->mu);

    return *out_due != NULL;
}

static int64_t tua_loop_timer_timeout_ns(tua_loop_t* loop) {
    uint64_t now = tua_time_mono_ns();
    tua_mutex_lock(loop->mu);
    tua_loop_prune_timers_locked(loop);
    tua_timer_t* t = loop->timers;
    tua_mutex_unlock(loop->mu);

    if (t == NULL) {
        return -1;
    }
    if (t->due_ns <= now) {
        return 0;
    }
    return (int64_t)(t->due_ns - now);
}

tua_err_t tua_loop_create(tua_loop_t** out) {
    if (out == NULL) {
        return TUA_E_INVALID;
    }

    tua_loop_t* loop = (tua_loop_t*)tua_malloc(sizeof(*loop));
    if (loop == NULL) {
        return TUA_E_NOMEM;
    }

    loop->mu = NULL;
    loop->wake_r = -1;
    loop->wake_w = -1;
    loop->kq = -1;
    atomic_store_explicit(&loop->stop, 0, memory_order_release);
    loop->task_head = NULL;
    loop->task_tail = NULL;
    loop->timers = NULL;
    loop->ios = NULL;

    tua_err_t err = tua_mutex_create(&loop->mu);
    if (err != TUA_OK) {
        tua_free(loop);
        return err;
    }

    int fds[2];
    if (pipe(fds) != 0) {
        tua_mutex_free(loop->mu);
        tua_free(loop);
        return tua_err_from_errno(errno);
    }
    loop->wake_r = fds[0];
    loop->wake_w = fds[1];

    err = tua_set_nonblocking_cloexec(loop->wake_r);
    if (err != TUA_OK) {
        tua_loop_free(loop);
        return err;
    }
    err = tua_set_nonblocking_cloexec(loop->wake_w);
    if (err != TUA_OK) {
        tua_loop_free(loop);
        return err;
    }

    loop->kq = kqueue();
    if (loop->kq < 0) {
        tua_loop_free(loop);
        return tua_err_from_errno(errno);
    }
    err = tua_set_cloexec(loop->kq);
    if (err != TUA_OK) {
        tua_loop_free(loop);
        return err;
    }

    struct kevent kev;
    EV_SET(&kev, (uintptr_t)loop->wake_r, EVFILT_READ, EV_ADD | EV_ENABLE | EV_CLEAR, 0, 0, NULL);
    if (kevent(loop->kq, &kev, 1, NULL, 0, NULL) != 0) {
        tua_loop_free(loop);
        return tua_err_from_errno(errno);
    }

    *out = loop;
    return TUA_OK;
}

void tua_loop_free(tua_loop_t* loop) {
    if (loop == NULL) {
        return;
    }
    if (loop->kq >= 0) {
        close(loop->kq);
    }
    if (loop->wake_r >= 0) {
        close(loop->wake_r);
    }
    if (loop->wake_w >= 0) {
        close(loop->wake_w);
    }

    tua_mutex_lock(loop->mu);
    tua_task_t* t = loop->task_head;
    while (t != NULL) {
        tua_task_t* next = t->next;
        tua_free(t);
        t = next;
    }
    tua_timer_t* timer = loop->timers;
    while (timer != NULL) {
        tua_timer_t* next = timer->next;
        tua_free(timer);
        timer = next;
    }
    tua_io_t* io = loop->ios;
    while (io != NULL) {
        tua_io_t* next = io->next;
        tua_free(io);
        io = next;
    }
    tua_mutex_unlock(loop->mu);

    tua_mutex_free(loop->mu);
    tua_free(loop);
}

void tua_loop_stop(tua_loop_t* loop) {
    if (loop == NULL) {
        return;
    }
    atomic_store_explicit(&loop->stop, 1, memory_order_release);
    tua_loop_wakeup(loop);
}

tua_err_t tua_loop_post(tua_loop_t* loop, tua_task_fn fn, void* arg) {
    if (loop == NULL || fn == NULL) {
        return TUA_E_INVALID;
    }
    tua_task_t* task = (tua_task_t*)tua_malloc(sizeof(*task));
    if (task == NULL) {
        return TUA_E_NOMEM;
    }
    task->fn = fn;
    task->arg = arg;
    task->next = NULL;

    tua_mutex_lock(loop->mu);
    if (loop->task_tail == NULL) {
        loop->task_head = task;
        loop->task_tail = task;
    } else {
        loop->task_tail->next = task;
        loop->task_tail = task;
    }
    tua_mutex_unlock(loop->mu);

    return tua_loop_wakeup(loop);
}

static void tua_loop_kevent_delete_io(tua_loop_t* loop, tua_io_t* io) {
    struct kevent changes[2];
    int n = 0;
    if (io->events & TUA_IO_READ) {
        EV_SET(&changes[n++], (uintptr_t)io->fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    }
    if (io->events & TUA_IO_WRITE) {
        EV_SET(&changes[n++], (uintptr_t)io->fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    }
    if (n > 0) {
        (void)kevent(loop->kq, changes, n, NULL, 0, NULL);
    }
}

tua_err_t tua_io_start_handle(
    tua_loop_t* loop,
    tua_io_t** out,
    tua_handle_t handle,
    int events,
    tua_io_fn fn,
    void* arg
) {
    if (loop == NULL || fn == NULL) {
        return TUA_E_INVALID;
    }
    if ((events & (TUA_IO_READ | TUA_IO_WRITE)) == 0) {
        return TUA_E_INVALID;
    }
    int fd = -1;
    if (!tua_handle_to_fd(handle, &fd) || fd < 0) {
        return TUA_E_INVALID;
    }

    tua_io_t* io = (tua_io_t*)tua_malloc(sizeof(*io));
    if (io == NULL) {
        return TUA_E_NOMEM;
    }
    io->loop = loop;
    io->handle = handle;
    io->fd = fd;
    io->events = events;
    io->fn = fn;
    io->arg = arg;
    io->active = 1;
    io->next = NULL;

    struct kevent changes[2];
    int n = 0;
    if (events & TUA_IO_READ) {
        EV_SET(&changes[n++], (uintptr_t)fd, EVFILT_READ, EV_ADD | EV_ENABLE | EV_CLEAR, 0, 0, io);
    }
    if (events & TUA_IO_WRITE) {
        EV_SET(&changes[n++], (uintptr_t)fd, EVFILT_WRITE, EV_ADD | EV_ENABLE | EV_CLEAR, 0, 0, io);
    }

    tua_mutex_lock(loop->mu);
    io->next = loop->ios;
    loop->ios = io;
    tua_mutex_unlock(loop->mu);

    if (kevent(loop->kq, changes, n, NULL, 0, NULL) != 0) {
        tua_mutex_lock(loop->mu);
        io->active = 0;
        tua_mutex_unlock(loop->mu);
        tua_loop_wakeup(loop);
        return tua_err_from_errno(errno);
    }

    tua_loop_wakeup(loop);
    if (out != NULL) {
        *out = io;
    }
    return TUA_OK;
}

tua_err_t tua_io_start(
    tua_loop_t* loop,
    tua_io_t** out,
    tua_fd_t fd,
    int events,
    tua_io_fn fn,
    void* arg
) {
    return tua_io_start_handle(loop, out, tua_handle_from_fd(fd), events, fn, arg);
}

void tua_io_cancel(tua_io_t* io) {
    if (io == NULL || io->loop == NULL) {
        return;
    }
    tua_loop_t* loop = io->loop;
    tua_mutex_lock(loop->mu);
    io->active = 0;
    tua_mutex_unlock(loop->mu);
    tua_loop_kevent_delete_io(loop, io);
    tua_loop_wakeup(loop);
}

tua_err_t tua_timer_start(
    tua_loop_t* loop,
    tua_timer_t** out,
    uint64_t delay_ms,
    uint64_t repeat_ms,
    tua_task_fn fn,
    void* arg
) {
    if (loop == NULL || fn == NULL) {
        return TUA_E_INVALID;
    }
    tua_timer_t* timer = (tua_timer_t*)tua_malloc(sizeof(*timer));
    if (timer == NULL) {
        return TUA_E_NOMEM;
    }
    timer->loop = loop;
    timer->due_ns = tua_time_mono_ns() + delay_ms * 1000000ull;
    timer->repeat_ns = repeat_ms * 1000000ull;
    timer->fn = fn;
    timer->arg = arg;
    timer->active = 1;
    timer->next = NULL;

    tua_mutex_lock(loop->mu);
    tua_timer_insert_sorted(loop, timer);
    tua_mutex_unlock(loop->mu);

    tua_loop_wakeup(loop);

    if (out != NULL) {
        *out = timer;
    }
    return TUA_OK;
}

void tua_timer_cancel(tua_timer_t* timer) {
    if (timer == NULL || timer->loop == NULL) {
        return;
    }
    tua_loop_t* loop = timer->loop;
    tua_mutex_lock(loop->mu);
    timer->active = 0;
    tua_mutex_unlock(loop->mu);
    tua_loop_wakeup(loop);
}

tua_err_t tua_loop_run(tua_loop_t* loop) {
    if (loop == NULL) {
        return TUA_E_INVALID;
    }

    struct kevent events[64];

    while (atomic_load_explicit(&loop->stop, memory_order_acquire) == 0) {
        int64_t timeout_ns = tua_loop_timer_timeout_ns(loop);
        struct timespec ts;
        struct timespec* tsp = NULL;
        if (timeout_ns >= 0) {
            ts.tv_sec = (time_t)(timeout_ns / 1000000000ll);
            ts.tv_nsec = (long)(timeout_ns % 1000000000ll);
            tsp = &ts;
        }

        int rc = kevent(loop->kq, NULL, 0, events, (int)(sizeof(events) / sizeof(events[0])), tsp);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return tua_err_from_errno(errno);
        }

        for (int i = 0; i < rc; i++) {
            struct kevent* ev = &events[i];
            if (ev->udata == NULL && (int)ev->ident == loop->wake_r && ev->filter == EVFILT_READ) {
                tua_loop_drain_wakeup(loop);
                continue;
            }

            tua_io_t* io = (tua_io_t*)ev->udata;
            if (io == NULL) {
                continue;
            }

            int re = 0;
            if (ev->filter == EVFILT_READ) {
                re |= TUA_IO_READ;
            } else if (ev->filter == EVFILT_WRITE) {
                re |= TUA_IO_WRITE;
            }
            if (ev->flags & EV_EOF) {
                re |= TUA_IO_HUP;
            }
            if (ev->flags & EV_ERROR) {
                re |= TUA_IO_ERR;
            }

            int active = 0;
            tua_mutex_lock(loop->mu);
            active = io->active;
            tua_mutex_unlock(loop->mu);
            if (active) {
                io->fn(re, io->arg);
            }
        }

        tua_loop_run_tasks(loop);

        tua_timer_t* due = NULL;
        if (tua_loop_collect_due_timers(loop, &due)) {
            while (due != NULL) {
                tua_timer_t* next = due->next;
                int active = 0;
                tua_mutex_lock(loop->mu);
                active = due->active;
                tua_mutex_unlock(loop->mu);
                if (active) {
                    due->fn(due->arg);
                }
                if (active && due->repeat_ns > 0) {
                    due->due_ns = tua_time_mono_ns() + due->repeat_ns;
                    due->next = NULL;
                    tua_mutex_lock(loop->mu);
                    tua_timer_insert_sorted(loop, due);
                    tua_mutex_unlock(loop->mu);
                } else {
                    tua_free(due);
                }
                due = next;
            }
        }

        tua_mutex_lock(loop->mu);
        tua_loop_prune_ios_locked(loop);
        tua_mutex_unlock(loop->mu);
    }

    return TUA_OK;
}

#endif
