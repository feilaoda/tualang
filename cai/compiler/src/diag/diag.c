#include "cai/diag.h"

#include <stdarg.h>
#include <stdio.h>

/*
 * Diagnostics implementation.
 *
 * A single call to `ai_diag_error` reports an error message and increments
 * `error_count`. The driver uses `error_count` to decide the process exit code.
 *
 * Current design note:
 * - diagnostics are printed directly to stderr (good enough for now)
 * - later we can add a configurable sink (e.g. JSON, LSP, IDE integration)
 */

void ai_diag_init(AiDiag* diag, const char* filename) {
  diag->filename = filename;
  diag->error_count = 0;
}

void ai_diag_error(AiDiag* diag, AiSpan span, const char* fmt, ...) {
  diag->error_count += 1;
  fprintf(stderr, "%s:%d:%d: error: ", diag->filename ? diag->filename : "<input>", span.line,
          span.column);

  va_list args;
  va_start(args, fmt);
  vfprintf(stderr, fmt, args);
  va_end(args);

  fprintf(stderr, "\n");
}
