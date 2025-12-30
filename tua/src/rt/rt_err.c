#include "rt/rt_err.h"

#include <errno.h>

tua_err_t tua_err_from_errno(int e) {
    switch (e) {
        case 0: return TUA_OK;
        case EINVAL: return TUA_E_INVALID;
        case ENOMEM: return TUA_E_NOMEM;
#ifdef EAGAIN
        case EAGAIN: return TUA_E_AGAIN;
#endif
#if defined(EWOULDBLOCK) && (!defined(EAGAIN) || (EWOULDBLOCK != EAGAIN))
        case EWOULDBLOCK: return TUA_E_AGAIN;
#endif
        case ENOENT: return TUA_E_NOTFOUND;
        case EACCES: return TUA_E_ACCESS;
#ifdef EPERM
        case EPERM: return TUA_E_ACCESS;
#endif
#ifdef EINTR
        case EINTR: return TUA_E_INTR;
#endif
#ifdef ETIMEDOUT
        case ETIMEDOUT: return TUA_E_TIMEDOUT;
#endif
#ifdef ENOTSUP
        case ENOTSUP: return TUA_E_NOTSUP;
#endif
#ifdef EOPNOTSUPP
        case EOPNOTSUPP: return TUA_E_NOTSUP;
#endif
        default: return TUA_E_IO;
    }
}

const char* tua_err_name(tua_err_t err) {
    switch (err) {
        case TUA_OK: return "TUA_OK";
        case TUA_E_UNKNOWN: return "TUA_E_UNKNOWN";
        case TUA_E_INVALID: return "TUA_E_INVALID";
        case TUA_E_NOMEM: return "TUA_E_NOMEM";
        case TUA_E_AGAIN: return "TUA_E_AGAIN";
        case TUA_E_NOTFOUND: return "TUA_E_NOTFOUND";
        case TUA_E_ACCESS: return "TUA_E_ACCESS";
        case TUA_E_IO: return "TUA_E_IO";
        case TUA_E_INTR: return "TUA_E_INTR";
        case TUA_E_TIMEDOUT: return "TUA_E_TIMEDOUT";
        case TUA_E_NOTSUP: return "TUA_E_NOTSUP";
        case TUA_E_CANCELED: return "TUA_E_CANCELED";
        default: return "TUA_E_???";
    }
}
