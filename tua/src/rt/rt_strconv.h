#ifndef _TUA_RT_STRCONV_H_
#define _TUA_RT_STRCONV_H_

#include <stdint.h>

// Allocates a NUL-terminated decimal string for the given int.
// The returned buffer is allocated with `tua_malloc` and must be freed via `tua_free`.
char* tua_int_to_string_alloc(int32_t v);

// Parse a decimal floating point value (best-effort, strict: must consume the full string except whitespace).
// Returns 1 on success and writes to `out`; otherwise returns 0 and writes 0.0.
int32_t tua_parse_double(const char* s, double* out);

#endif
