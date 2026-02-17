#include "rt/rt_config.h"

#include "rt/rt_state.h"

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
        tua_global_config = tua_config_default();
        return;
    }

    tua_global_config.allocator = cfg->allocator.alloc ? cfg->allocator : tua_allocator_default();

    tua_global_config.panic = cfg->panic ? cfg->panic : tua_default_panic;
    tua_global_config.panic_ud = cfg->panic_ud;
}

tua_config tua_rt_get_config(void) {
    tua_config_init_default_if_needed();
    tua_state* s = tua_state_get_current();
    if (s) {
        const tua_config* c = tua_state_config(s);
        if (c) return *c;
    }
    return tua_global_config;
}

tua_config tua_rt_get_global_config(void) {
    tua_config_init_default_if_needed();
    return tua_global_config;
}
