#include "rt/rt_cancel.h"

#include <stdatomic.h>

void tua_cancel_init(tua_cancel_t* cancel) {
    atomic_store_explicit(&cancel->requested, 0, memory_order_release);
}

void tua_cancel_request(tua_cancel_t* cancel) {
    atomic_store_explicit(&cancel->requested, 1, memory_order_release);
}

int tua_cancel_is_requested(const tua_cancel_t* cancel) {
    return atomic_load_explicit(&cancel->requested, memory_order_acquire) != 0;
}

