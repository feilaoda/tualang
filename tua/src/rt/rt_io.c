#include "rt/rt_io.h"

#include "rt/rt_alloc.h"

#include <stdio.h>
#include <string.h>

tua_err_t tua_io_readline_alloc(char** out_data, int32_t* out_len) {
    if (!out_data || !out_len) return TUA_E_INVALID;
    *out_data = NULL;
    *out_len = 0;

    // Fixed buffer; for typical REPL usage this is enough and avoids complex realloc logic.
    // If needed later we can grow dynamically.
    char buf[8192];
    if (!fgets(buf, (int)sizeof(buf), stdin)) {
        // EOF or error; treat as EOF for now.
        return TUA_OK;
    }
    size_t n = strlen(buf);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) n--;

    char* out = (char*)tua_malloc(n + 1);
    if (!out) return TUA_E_NOMEM;
    if (n) memcpy(out, buf, n);
    out[n] = '\0';
    *out_data = out;
    *out_len = (int32_t)n;
    return TUA_OK;
}

tua_err_t tua_io_flush_stdout(void) {
    fflush(stdout);
    return TUA_OK;
}
