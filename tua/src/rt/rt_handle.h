#ifndef _TUA_RT_HANDLE_H_
#define _TUA_RT_HANDLE_H_

#include <stddef.h>
#include <stdint.h>

typedef enum {
    TUA_HANDLE_INVALID = 0,
    TUA_HANDLE_FD = 1,
    TUA_HANDLE_WIN_HANDLE = 2,
    TUA_HANDLE_WIN_SOCKET = 3,
} tua_handle_kind_t;

typedef struct {
    uint32_t kind;
    uint32_t flags;
    intptr_t raw;
} tua_handle_t;

#if defined(_WIN32)
typedef uintptr_t tua_fd_t;
#else
typedef int tua_fd_t;
#endif

static inline tua_handle_t tua_handle_invalid(void) {
    tua_handle_t h;
    h.kind = (uint32_t)TUA_HANDLE_INVALID;
    h.flags = 0;
    h.raw = -1;
    return h;
}

static inline tua_handle_t tua_handle_from_fd(tua_fd_t fd) {
    tua_handle_t h;
    h.kind = (uint32_t)TUA_HANDLE_FD;
    h.flags = 0;
    h.raw = (intptr_t)fd;
    return h;
}

static inline int tua_handle_to_fd(tua_handle_t h, int* out_fd) {
    if (out_fd == NULL) return 0;
    if (h.kind != (uint32_t)TUA_HANDLE_FD) return 0;
    *out_fd = (int)h.raw;
    return 1;
}

#endif
