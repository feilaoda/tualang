#ifndef TUA_MAP_H
#define TUA_MAP_H

#include <stdint.h>

typedef struct tua_value {
    int32_t tag;
    uint64_t payload;
} tua_value;

typedef struct tua_map tua_map;

tua_map* tua_map_new(void);
tua_value tua_map_get(tua_map* map, tua_value key);
// Returns value and writes ok=1 if key exists (even if value is null), else ok=0 and returns null.
tua_value tua_map_get_with_ok(tua_map* map, tua_value key, int32_t* outOk);
void tua_map_set(tua_map* map, tua_value key, tua_value value);
int32_t tua_map_delete(tua_map* map, tua_value key);
void tua_map_clear(tua_map* map);
int32_t tua_map_has(tua_map* map, tua_value key);
int32_t tua_map_len(tua_map* map);
int32_t tua_map_iter_next(tua_map* map, int32_t* index, tua_value* outKey, tua_value* outValue);

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

char* tua_str_concat(const char* a, const char* b);

#endif
