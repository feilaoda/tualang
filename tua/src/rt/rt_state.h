#ifndef _TUA_RT_STATE_H_
#define _TUA_RT_STATE_H_

#include "rt/rt_config.h"

#include <stdint.h>

typedef struct tua_state tua_state;

tua_state* tua_state_new(const tua_config* cfg);
void tua_state_free(tua_state* s);

void tua_state_set_current(tua_state* s);
tua_state* tua_state_get_current(void);

const tua_config* tua_state_config(const tua_state* s);

void tua_state_set_line(tua_state* s, int32_t line);
void tua_state_set_loc(tua_state* s, const char* file, int32_t line, int32_t col);
void tua_state_get_loc(const tua_state* s, const char** out_file, int32_t* out_line, int32_t* out_col);

#endif
