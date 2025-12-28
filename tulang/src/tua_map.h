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

#endif

