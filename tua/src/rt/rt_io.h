#ifndef _TUA_RT_IO_H_
#define _TUA_RT_IO_H_

#include "rt/rt_err.h"

#include <stdint.h>

// Reads one line from stdin into a newly allocated, NUL-terminated buffer.
// - Strips trailing '\n' and optional '\r'.
// - On EOF: returns TUA_OK with `*out_data=NULL` and `*out_len=0`.
// On success: `*out_data` must be freed via `tua_free` and `*out_len` is set (bytes excluding trailing NUL).
tua_err_t tua_io_readline_alloc(char** out_data, int32_t* out_len);

#endif

