#ifndef _TUA_RT_CANCEL_H_
#define _TUA_RT_CANCEL_H_

#include <stdint.h>
#include <stdatomic.h>

typedef struct tua_cancel {
    _Atomic int requested;
} tua_cancel_t;

void tua_cancel_init(tua_cancel_t* cancel);
void tua_cancel_request(tua_cancel_t* cancel);
int tua_cancel_is_requested(const tua_cancel_t* cancel);

typedef uint64_t tua_deadline_t;

enum {
    TUA_DEADLINE_NONE = 0,
};

tua_deadline_t tua_deadline_after_ms(uint64_t ms);
int tua_deadline_is_none(tua_deadline_t deadline);
int tua_deadline_is_expired(tua_deadline_t deadline);
uint64_t tua_deadline_to_delay_ms(tua_deadline_t deadline);

#endif
