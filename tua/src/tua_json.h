#ifndef TUA_JSON_H
#define TUA_JSON_H

#include <stdint.h>

typedef struct tua_bytes tua_bytes;

// Scan a top-level JSON object and extract a string value for `key`.
// Returns:
// - 0: found, writes newly-allocated string to *out_str
// - 2: not found (out_str set to NULL)
// - 1: parse error/invalid input (out_str set to NULL)
int32_t tua_json_scan_top_level_string(tua_bytes* b, const char* key, char** out_str);

// Scan a top-level JSON object and extract an integer value for `key`.
// Returns:
// - 0: found, writes value to *out_val
// - 2: not found (out_val set to 0)
// - 1: parse error/invalid input (out_val set to 0)
int32_t tua_json_scan_top_level_long(tua_bytes* b, const char* key, int64_t* out_val);

// Scan a top-level JSON object and extract a boolean value for `key`.
// Returns:
// - 0: found, writes 0/1 to *out_bool
// - 2: not found (out_bool set to 0)
// - 1: parse error/invalid input (out_bool set to 0)
int32_t tua_json_scan_top_level_bool(tua_bytes* b, const char* key, int32_t* out_bool);

#endif
