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
void tua_map_set(tua_map* map, tua_value key, tua_value value);

void tua_print_value(tua_value value, int32_t newline);

void tua_assert_fail(const char* msg, int32_t line);

int32_t tua_value_to_int(tua_value v);
int64_t tua_value_to_long(tua_value v);
double tua_value_to_double(tua_value v);
int32_t tua_value_to_bool(tua_value v);
char* tua_value_to_string(tua_value v);

#endif
