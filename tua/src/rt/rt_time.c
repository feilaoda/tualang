#include "rt/rt_time.h"

#include <errno.h>
#include <time.h>

#if defined(__APPLE__)
#include <mach/mach_time.h>
#include <sys/time.h>
#endif

#if !defined(__APPLE__)
static uint64_t tua_timespec_to_ns(struct timespec ts) {
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
#endif

uint64_t tua_time_mono_ns(void) {
#if defined(__APPLE__)
    static mach_timebase_info_data_t info;
    static int inited = 0;
    if (!inited) {
        mach_timebase_info(&info);
        inited = 1;
    }
    uint64_t t = mach_absolute_time();
    return (t * (uint64_t)info.numer) / (uint64_t)info.denom;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return tua_timespec_to_ns(ts);
#endif
}

uint64_t tua_time_real_ns(void) {
#if defined(__APPLE__)
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000000ull + (uint64_t)tv.tv_usec * 1000ull;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return tua_timespec_to_ns(ts);
#endif
}

void tua_sleep_ns(uint64_t ns) {
    struct timespec req;
    req.tv_sec = (time_t)(ns / 1000000000ull);
    req.tv_nsec = (long)(ns % 1000000000ull);
    while (nanosleep(&req, &req) != 0) {
        if (errno != EINTR) {
            return;
        }
    }
}
