#ifndef _TUA_RT_ERR_H_
#define _TUA_RT_ERR_H_

#include <stdint.h>

typedef int32_t tua_err_t;

enum {
    TUA_OK = 0,
    TUA_E_UNKNOWN = 1,
    TUA_E_INVALID = 2,
    TUA_E_NOMEM = 3,
    TUA_E_AGAIN = 4,
    TUA_E_NOTFOUND = 5,
    TUA_E_ACCESS = 6,
    TUA_E_IO = 7,
    TUA_E_INTR = 8,
    TUA_E_TIMEDOUT = 9,
    TUA_E_NOTSUP = 10,
    TUA_E_CANCELED = 11,
};

tua_err_t tua_err_from_errno(int e);
const char* tua_err_name(tua_err_t err);

#endif

