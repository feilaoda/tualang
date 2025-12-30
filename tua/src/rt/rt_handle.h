#ifndef _TUA_RT_HANDLE_H_
#define _TUA_RT_HANDLE_H_

#include <stdint.h>

#ifdef _WIN32
typedef uintptr_t tua_fd_t;
#else
typedef int tua_fd_t;
#endif

#endif

