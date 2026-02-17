#ifndef TUA_MAP_H
#define TUA_MAP_H

#include <stdint.h>

typedef struct tua_value {
    int32_t tag;
    uint64_t payload;
} tua_value;

// Runtime value tags (ABI-stable for FFI helpers).
// Keep in sync with `src/tua_map.c`.
enum {
    TUA_VAL_NIL = 0,
    TUA_VAL_INT = 1,
    TUA_VAL_LONG = 2,
    TUA_VAL_DOUBLE = 3,
    TUA_VAL_BOOL = 4,
    TUA_VAL_STRING = 5,
    TUA_VAL_PTR = 6
};

// Map implementation kind (internal; used for typed-map backends).
enum {
    TUA_MAP_KIND_GENERIC = 1,
    TUA_MAP_KIND_IMAP = 2
};

// Convenience constructors for external C libraries.
static inline tua_value tua_value_nil(void) {
    tua_value v;
    v.tag = TUA_VAL_NIL;
    v.payload = 0;
    return v;
}
static inline tua_value tua_value_int(int32_t x) {
    tua_value v;
    v.tag = TUA_VAL_INT;
    v.payload = (uint64_t)(uint32_t)x;
    return v;
}
static inline tua_value tua_value_long(int64_t x) {
    tua_value v;
    v.tag = TUA_VAL_LONG;
    v.payload = (uint64_t)x;
    return v;
}
static inline tua_value tua_value_double_bits(uint64_t bits) {
    tua_value v;
    v.tag = TUA_VAL_DOUBLE;
    v.payload = bits;
    return v;
}
static inline tua_value tua_value_bool(int32_t b) {
    tua_value v;
    v.tag = TUA_VAL_BOOL;
    v.payload = b ? 1u : 0u;
    return v;
}
static inline tua_value tua_value_string(const char* s) {
    tua_value v;
    v.tag = TUA_VAL_STRING;
    v.payload = (uint64_t)(uintptr_t)s;
    return v;
}
static inline tua_value tua_value_ptr(const void* p) {
    tua_value v;
    v.tag = TUA_VAL_PTR;
    v.payload = (uint64_t)(uintptr_t)p;
    return v;
}

typedef struct tua_map tua_map;

tua_map* tua_map_new(void);
tua_value tua_map_get(tua_map* map, tua_value key);
// Returns value and writes ok=1 if key exists (even if value is null), else ok=0 and returns null.
tua_value tua_map_get_with_ok(tua_map* map, tua_value key, int32_t* outOk);
// Returns a pointer to the stored value and writes ok=1 if key exists (even if value is nil),
// else ok=0 and returns NULL.
// Note: the returned pointer is only valid until the map is mutated in a way that may rehash/relocate entries.
tua_value* tua_map_get_ref_with_ok(tua_map* map, tua_value key, int32_t* outOk);
void tua_map_set(tua_map* map, tua_value key, tua_value value);
int32_t tua_map_delete(tua_map* map, tua_value key);
void tua_map_clear(tua_map* map);
int32_t tua_map_has(tua_map* map, tua_value key);
int32_t tua_map_len(tua_map* map);
int32_t tua_map_iter_next(tua_map* map, int32_t* index, tua_value* outKey, tua_value* outValue);
void tua_map_free(tua_map* map);

// Integer-key typed-map backend (internal; used by compiler codegen).
// Keys are normalized to signed i64 (same as `tuaValueFromKey`: int/long are treated uniformly).
// Values are stored as raw `tua_value.payload` bits and interpreted based on `value_tag`.
tua_map* tua_imap_new(int32_t value_tag, int32_t hint);
tua_map* tua_imap_new_i32(int32_t value_tag, int32_t hint);
uint64_t tua_imap_get_payload_with_ok(tua_map* map, int64_t key, int32_t* outOk);
void tua_imap_set_payload(tua_map* map, int64_t key, uint64_t payload);
int32_t tua_imap_delete(tua_map* map, int64_t key);
void tua_imap_clear(tua_map* map);
int32_t tua_imap_has(tua_map* map, int64_t key);
int32_t tua_imap_len(tua_map* map);
int32_t tua_imap_iter_next(tua_map* map, int32_t* index, tua_value* outKey, tua_value* outValue);
void tua_imap_free(tua_map* map);

void tua_panic(const char* msg);
void tua_set_loc(const char* file, int32_t line, int32_t col);
void tua_set_line(int32_t line);

void tua_print_value(tua_value value, int32_t newline);

void tua_assert_fail(const char* msg, int32_t line);

int32_t tua_value_to_int(tua_value v);
int64_t tua_value_to_long(tua_value v);
double tua_value_to_double(tua_value v);
int32_t tua_value_to_bool(tua_value v);
char* tua_value_to_string(tua_value v);

// Parse base-10 `int` from string.
// Returns 1 on success and writes to `out`, otherwise returns 0 and leaves `out` unchanged.
int32_t tua_parse_int(const char* s, int32_t* out);

// String runtime helpers (see `src/tua_str.c`).
int64_t tua_str_byte_len(const char* s);
int32_t tua_str_len(const char* s);
int32_t tua_str_eq(const char* a, const char* b);
uint32_t tua_str_hash32(const char* s);
void tua_str_retain(const char* s);
void tua_str_release(const char* s);
char* tua_str_concat(const char* a, const char* b);
char* tua_str_substring(const char* s, int32_t i);

#endif
