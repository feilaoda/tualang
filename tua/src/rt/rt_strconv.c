#include "rt/rt_strconv.h"

#include "rt/rt_alloc.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

char* tua_int_to_string_alloc(int32_t v) {
    char tmp[32];
    int n = snprintf(tmp, sizeof(tmp), "%" PRId32, v);
    if (n < 0) return NULL;
    size_t len = (size_t)n;
    char* out = (char*)tua_malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, tmp, len + 1);
    return out;
}

