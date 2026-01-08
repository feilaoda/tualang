#include "rt/rt_strconv.h"

#include "rt/rt_alloc.h"

#include <inttypes.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
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

int32_t tua_parse_double(const char* s, double* out) {
    if (!out) return 0;
    *out = 0.0;
    if (!s) return 0;

    const unsigned char* p = (const unsigned char*)s;
    while (*p && isspace(*p)) p++;
    if (!*p) return 0;

    char* endp = NULL;
    double v = strtod((const char*)p, &endp);
    if (!endp || endp == (char*)p) return 0;

    const unsigned char* q = (const unsigned char*)endp;
    while (*q && isspace(*q)) q++;
    if (*q) return 0;

    *out = v;
    return 1;
}
