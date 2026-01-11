#include "rt/rt_state.h"

#include "rt/rt_alloc.h"

#include <string.h>

struct tua_state {
    tua_config cfg;

    const char* loc_file;
    int32_t loc_line;
    int32_t loc_col;
};

static _Thread_local tua_state* tua_tls_state = NULL;

tua_state* tua_state_new(const tua_config* cfg) {
    tua_config c = cfg ? *cfg : tua_config_default();
    if (c.allocator.alloc == NULL) {
        c.allocator = tua_allocator_default();
    }
    if (c.panic == NULL) {
        c.panic = tua_config_default().panic;
        c.panic_ud = tua_config_default().panic_ud;
    }

    tua_state* s = (tua_state*)tua_alloc(&c.allocator, NULL, 0, sizeof(*s));
    if (!s) return NULL;
    memset(s, 0, sizeof(*s));
    s->cfg = c;
    return s;
}

void tua_state_free(tua_state* s) {
    if (!s) return;
    if (tua_tls_state == s) {
        tua_tls_state = NULL;
    }
    tua_allocator a = s->cfg.allocator;
    (void)tua_alloc(&a, s, sizeof(*s), 0);
}

void tua_state_set_current(tua_state* s) {
    tua_tls_state = s;
}

tua_state* tua_state_get_current(void) {
    return tua_tls_state;
}

const tua_config* tua_state_config(const tua_state* s) {
    if (!s) return NULL;
    return &s->cfg;
}

void tua_state_set_line(tua_state* s, int32_t line) {
    if (!s) return;
    s->loc_line = line;
}

void tua_state_set_loc(tua_state* s, const char* file, int32_t line, int32_t col) {
    if (!s) return;
    if (file) s->loc_file = file;
    s->loc_line = line;
    s->loc_col = col;
}

void tua_state_get_loc(const tua_state* s, const char** out_file, int32_t* out_line, int32_t* out_col) {
    if (!s) return;
    if (out_file) *out_file = s->loc_file;
    if (out_line) *out_line = s->loc_line;
    if (out_col) *out_col = s->loc_col;
}

