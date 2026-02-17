#include "tua_array.h"

#include <stdint.h>

// Example external library function:
// - Accepts Tua `long[]` as `tua_array*` (elem_size must be 8)
// - Writes error code to outErr (0 ok, non-zero error)
int64_t ext_sum_i64(tua_array* ids, int32_t* outErr) {
    if (outErr) *outErr = 1;
    if (!outErr) return 0;
    if (!ids) return 0;
    if (ids->elem_size != (int64_t)sizeof(int64_t)) return 0;
    if (ids->len < 0) return 0;
    if (ids->len > 0 && !ids->data) return 0;

    const int64_t* p = (const int64_t*)ids->data;
    int64_t sum = 0;
    for (int64_t i = 0; i < ids->len; i++) sum += p[i];

    *outErr = 0;
    return sum;
}

