#ifndef _TUA_RT_TIME_H_
#define _TUA_RT_TIME_H_

#include <stdint.h>

uint64_t tua_time_mono_ns(void);
uint64_t tua_time_real_ns(void);
void tua_sleep_ns(uint64_t ns);

#endif

