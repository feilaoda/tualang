#ifndef TUA_STR_H
#define TUA_STR_H

#include "rt/rt_err.h"

#include <stdint.h>

// Tua `string` runtime helpers.
//
// Current LLVM ABI still represents `string` as `char*` (`i8*`), but the runtime
// may attach metadata (refcount/length/hash) to managed strings. Unmanaged C
// strings (e.g. literals) are supported as inputs; retain/release are no-ops for
// unmanaged strings.

int64_t tua_str_byte_len(const char* s);
int32_t tua_str_len(const char* s);
int32_t tua_str_eq(const char* a, const char* b);
uint32_t tua_str_hash32(const char* s);

void tua_str_retain(const char* s);
void tua_str_release(const char* s);

// Concatenate (with `null` treated as "null"), returns a managed string.
char* tua_str_concat(const char* a, const char* b);

// substring by codepoint index (0-based); negative indices count from end.
// Panics on out-of-range (including i==len()).
char* tua_str_substring(const char* s, int32_t i);

// Decode best-effort UTF-8 bytes into a managed string.
// Invalid sequences are replaced with U+FFFD.
char* tua_str_from_utf8_bytes_replace(const uint8_t* data, int64_t len);

typedef struct tua_bytes tua_bytes;

// Encode/decode between `string` and `bytes` via a named encoding.
// Encoding names are case-insensitive; `NULL`/"" defaults to UTF-8.
// - "UTF-8": round-trips raw UTF-8 bytes; invalid byte sequences decode to U+FFFD.
// - "GBK": decode invalid sequences to U+FFFD; encode unrepresentable codepoints to '?' (0x3F).
//
// Unsupported encodings return `TUA_E_NOTSUP`.
tua_err_t tua_str_to_bytes_encoding(const char* s, const char* encoding, tua_bytes** out_bytes);
tua_err_t tua_str_from_bytes_encoding(tua_bytes* b, const char* encoding, char** out_s);

#endif
