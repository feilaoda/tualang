#include "rt/rt_config.h"

#include <stdio.h>
#include <stdlib.h>

static void tua_default_panic(void* ud, const char* msg) {
    (void)ud;
    if (msg && msg[0] != '\0') {
        fputs(msg, stderr);
    } else {
        fputs("error\n", stderr);
    }
    fflush(stderr);
    exit(1);
}

static tua_config tua_global_config = {0};
static int tua_global_config_inited = 0;

static void tua_config_init_default_if_needed(void) {
    if (tua_global_config_inited) return;
    tua_global_config = tua_config_default();
    tua_global_config_inited = 1;
}

tua_config tua_config_default(void) {
    tua_config c;
    c.allocator = tua_allocator_default();
    c.panic = tua_default_panic;
    c.panic_ud = NULL;
    return c;
}

void tua_rt_configure(const tua_config* cfg) {
    tua_config_init_default_if_needed();

    if (cfg == NULL) {
        tua_allocator_set_global(tua_allocator_default());
        tua_global_config = tua_config_default();
        return;
    }

    if (cfg->allocator.alloc == NULL) {
        tua_allocator_set_global(tua_allocator_default());
        tua_global_config.allocator = tua_allocator_default();
    } else {
        tua_allocator_set_global(cfg->allocator);
        tua_global_config.allocator = cfg->allocator;
    }

    tua_global_config.panic = cfg->panic ? cfg->panic : tua_default_panic;
    tua_global_config.panic_ud = cfg->panic_ud;
}

tua_config tua_rt_get_config(void) {
    tua_config_init_default_if_needed();
    return tua_global_config;
}

