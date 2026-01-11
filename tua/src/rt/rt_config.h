#ifndef _TUA_RT_CONFIG_H_
#define _TUA_RT_CONFIG_H_

#include "rt/rt_alloc.h"

#include <stddef.h>

typedef void (*tua_panic_fn)(void* ud, const char* msg);

typedef struct tua_config {
    tua_allocator allocator; // `.alloc == NULL` => default allocator
    tua_panic_fn panic;      // NULL => default panic handler (prints + exit(1))
    void* panic_ud;
} tua_config;

tua_config tua_config_default(void);

// Configures the runtime for the current process.
// Not thread-safe; call once during startup (before running Tua code).
void tua_rt_configure(const tua_config* cfg);
tua_config tua_rt_get_config(void);

#endif
