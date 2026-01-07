#ifndef TUA_BYTES_H
#define TUA_BYTES_H

#include "rt/rt_err.h"

#include <stdint.h>

// Opaque owning byte buffer (may be backed by malloc/free or mmap).
typedef struct tua_bytes tua_bytes;

// Allocates a new zero-initialized buffer of length `len`.
tua_bytes* tua_bytes_new(int64_t len);

// Allocates a new uninitialized buffer of length `len` (contents are unspecified).
tua_bytes* tua_bytes_new_uninit(int64_t len);

// Allocates a new buffer and copies `len` bytes from `data`.
tua_bytes* tua_bytes_from_copy(const void* data, int64_t len);

// Convenience: copies bytes from a NUL-terminated string.
tua_bytes* tua_bytes_from_string_copy(const char* s);

// Memory-maps a file read-only into a `bytes` handle.
// On success, returned bytes will unmap on drop/free.
tua_err_t tua_bytes_mmap_file(const char* path_utf8, tua_bytes** out_bytes);

int64_t tua_bytes_len(tua_bytes* b);
uint8_t* tua_bytes_data(tua_bytes* b);
// Returns `tua_bytes_data(b) + off` when within bounds; otherwise NULL.
uint8_t* tua_bytes_data_at(tua_bytes* b, int64_t off);
int32_t tua_bytes_is_readonly(tua_bytes* b);

// Safe-by-construction helpers for language bindings.
tua_err_t tua_bytes_get_u8(tua_bytes* b, int64_t idx, int32_t* out);
tua_err_t tua_bytes_set_u8(tua_bytes* b, int64_t idx, int32_t v);

// Copies `n` bytes from `src[src_off..]` into `dst[dst_off..]`.
// Returns TUA_E_ACCESS if `dst` is readonly; TUA_E_INVALID on bounds/NULL.
tua_err_t tua_bytes_copy(tua_bytes* dst, int64_t dst_off, tua_bytes* src, int64_t src_off, int64_t n);

// Allocates a NUL-terminated string by copying `b[off..off+len)`.
// Note: if the byte range contains '\0', the resulting C-string will be truncated when used as `string`.
char* tua_str_from_bytes_copy(tua_bytes* b, int64_t off, int64_t len);
// Returns the underlying C string pointer (for FFI helpers like memcmp).
void* tua_str_ptr(const char* s);

// Bit-cast helpers for binary formats.
double tua_f32_from_u32_bits(uint32_t bits);
double tua_f64_from_u64_bits(uint64_t bits);

// Vectorized dtype conversions (for model weights, etc.).
// `src_off`/`dst_off` are in bytes. `n` is number of elements.
// Returns TUA_E_ACCESS if dst is readonly; TUA_E_INVALID on bounds/NULL.
tua_err_t tua_bytes_bf16_to_f32(tua_bytes* src, int64_t src_off, tua_bytes* dst, int64_t dst_off, int64_t n);

void tua_bytes_free(tua_bytes* b);

#endif
