#ifndef TUA_JSON_H
#define TUA_JSON_H

#include <stdint.h>

typedef struct tua_bytes tua_bytes;
typedef struct tua_map tua_map;
typedef struct tua_array tua_array;

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

// Scan a nested JSON object by dotted key path (e.g. "a.b.c") and extract a string value.
// Returns:
// - 0: found, writes newly-allocated string to *out_str
// - 2: not found (out_str set to NULL)
// - 1: parse error/type mismatch/invalid input (out_str set to NULL)
int32_t tua_json_scan_key_path_string(tua_bytes* b, const char* path, char** out_str);

// Scan a nested JSON object by dotted key path (e.g. "a.b.c") and extract a strict integer value.
// Returns:
// - 0: found, writes value to *out_val
// - 2: not found (out_val set to 0)
// - 1: parse error/type mismatch/invalid input (out_val set to 0)
int32_t tua_json_scan_key_path_long(tua_bytes* b, const char* path, int64_t* out_val);

// Scan a nested JSON object by dotted key path (e.g. "a.b.c") and extract a boolean value.
// Returns:
// - 0: found, writes 0/1 to *out_bool
// - 2: not found (out_bool set to 0)
// - 1: parse error/type mismatch/invalid input (out_bool set to 0)
int32_t tua_json_scan_key_path_bool(tua_bytes* b, const char* path, int32_t* out_bool);

// Parse a nested JSON object at dotted key path (e.g. "a.b.vocab") into a `map<string,long>`.
// Values must be strict integers (no fraction/exponent).
// Returns:
// - 0: found, writes a newly-allocated map to *out_map
// - 2: not found (*out_map set to NULL)
// - 1: parse error/type mismatch (*out_map set to NULL)
int32_t tua_json_parse_key_path_string_long_map(tua_bytes* b, const char* path, tua_map** out_map);

// Parse a nested JSON array at dotted key path (e.g. "a.b.merges") into a `string[]`.
// Returns array and writes error to *out_err:
// - out_err=0: ok, return non-NULL array (possibly empty)
// - out_err=2: not found, return NULL
// - out_err=1: parse error/type mismatch, return NULL
tua_array* tua_json_parse_key_path_string_array(tua_bytes* b, const char* path, int32_t* out_err);

// Parse HF `tokenizer.json` BPE merges at `path` (e.g. "model.merges") into:
// - *out_rank: map<long,long> where key=(idA<<32)|idB, value=rank (0..)
// - *out_merge_id: map<long,long> where key=(idA<<32)|idB, value=mergedTokenId
// Requires `vocab` as a `map<string,long>` (token string -> id).
// Returns:
// - 0: ok
// - 2: not found (out maps set to NULL, out_count set to 0)
// - 1: parse error/type mismatch (out maps set to NULL, out_count set to 0)
int32_t tua_json_parse_key_path_bpe_merges_pair_maps(
    tua_bytes* b,
    const char* path,
    tua_map* vocab,
    tua_map** out_rank,
    tua_map** out_merge_id,
    int64_t* out_count
);

// Parse HF `tokenizer.json` top-level `added_tokens` into a `map<string,long>` (content -> id).
// Returns:
// - 0: ok (writes newly allocated map to *out_map)
// - 2: not found (*out_map set to NULL)
// - 1: parse error/type mismatch (*out_map set to NULL)
int32_t tua_json_parse_top_level_added_tokens_content_id_map(tua_bytes* b, tua_map** out_map);

#endif
