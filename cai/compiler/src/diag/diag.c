#include "cai/diag.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Diagnostics implementation.
 *
 * A single call to `ai_diag_error` reports an error message and increments
 * `error_count`. The driver uses `error_count` to decide the process exit code.
 *
 * Current design note:
 * - diagnostics go through an injectable sink/callback
 * - the default sink prints to stderr (good enough for CLI usage)
 */

static void diag_stderr_sink(void* user, AiDiagKind kind, const char* filename, AiSpan span,
                             const char* message) {
  (void)user;
  const char* kind_str = "error";
  if (kind != AI_DIAG_ERROR) {
    kind_str = "error";
  }
  fprintf(stderr, "%s:%d:%d: %s: %s\n", filename ? filename : "<input>", span.line, span.column,
          kind_str, message ? message : "");
}

void ai_diag_init(AiDiag* diag, const char* filename) {
  diag->filename = filename;
  diag->error_count = 0;
  diag->sink = diag_stderr_sink;
  diag->sink_user = NULL;
}

void ai_diag_set_sink(AiDiag* diag, AiDiagSinkFn sink, void* user) {
  diag->sink = sink ? sink : diag_stderr_sink;
  diag->sink_user = user;
}

void ai_diag_error(AiDiag* diag, AiSpan span, const char* fmt, ...) {
  diag->error_count += 1;

  va_list args;
  va_start(args, fmt);

  char stack_buf[512];
  va_list args_copy;
  va_copy(args_copy, args);
  int needed = vsnprintf(stack_buf, sizeof(stack_buf), fmt, args_copy);
  va_end(args_copy);

  if (needed < 0) {
    va_end(args);
    if (diag->sink) diag->sink(diag->sink_user, AI_DIAG_ERROR, diag->filename, span, "diagnostic");
    return;
  }

  const char* msg = stack_buf;
  char* heap_msg = NULL;
  if ((size_t)needed >= sizeof(stack_buf)) {
    heap_msg = (char*)malloc((size_t)needed + 1);
    if (!heap_msg) {
      va_end(args);
      if (diag->sink) diag->sink(diag->sink_user, AI_DIAG_ERROR, diag->filename, span,
                                 "out of memory while formatting diagnostic");
      return;
    }
    (void)vsnprintf(heap_msg, (size_t)needed + 1, fmt, args);
    msg = heap_msg;
  } else {
    // We already formatted into stack_buf.
    (void)needed;
  }

  va_end(args);

  if (diag->sink) {
    diag->sink(diag->sink_user, AI_DIAG_ERROR, diag->filename, span, msg);
  }
  free(heap_msg);
}
