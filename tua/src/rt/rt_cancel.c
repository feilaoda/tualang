#include "rt/rt_cancel.h"

#include <stdint.h>
#include <stdatomic.h>

#include "rt/rt_time.h"

void tua_cancel_init(tua_cancel_t* cancel) {
    atomic_store_explicit(&cancel->requested, 0, memory_order_release);
}

void tua_cancel_request(tua_cancel_t* cancel) {
    atomic_store_explicit(&cancel->requested, 1, memory_order_release);
}

int tua_cancel_is_requested(const tua_cancel_t* cancel) {
    return atomic_load_explicit(&cancel->requested, memory_order_acquire) != 0;
}

tua_deadline_t tua_deadline_after_ms(uint64_t ms) {
    if (ms == 0) {
        return tua_time_mono_ns();
    }
    uint64_t now = tua_time_mono_ns();
    uint64_t delta_ns = ms * 1000000ull;
    uint64_t d = now + delta_ns;
    if (d < now) {
        return UINT64_MAX;
    }
    return d;
}

int tua_deadline_is_none(tua_deadline_t deadline) {
    return deadline == TUA_DEADLINE_NONE;
}

int tua_deadline_is_expired(tua_deadline_t deadline) {
    if (deadline == TUA_DEADLINE_NONE) {
        return 0;
    }
    return tua_time_mono_ns() >= deadline;
}

uint64_t tua_deadline_to_delay_ms(tua_deadline_t deadline) {
    if (deadline == TUA_DEADLINE_NONE) {
        return UINT64_MAX;
    }
    uint64_t now = tua_time_mono_ns();
    if (deadline <= now) {
        return 0;
    }
    uint64_t diff_ns = deadline - now;
    uint64_t ms = diff_ns / 1000000ull;
    if (ms == 0) {
        return 1;
    }
    return ms;
}
