#pragma once

#include <stddef.h>

/*
 * Diagnostics for the compiler.
 *
 * `AiDiag` is the central sink for reporting compiler errors (and, later,
 * warnings). It is intentionally small:
 * - attaches a filename for display
 * - counts errors so callers can decide success/failure
 * - prints formatted diagnostics with a source location (`AiSpan`)
 *
 * Today diagnostics are printed to stderr directly; once we add a richer driver
 * or IDE integration, `AiDiag` can be extended to support configurable sinks.
 */

// A source span in a UTF-8 byte stream.
typedef struct AiSpan {
  size_t offset; // 0-based byte offset into the source buffer
  size_t length; // byte length
  int line;      // 1-based
  int column;    // 1-based (UTF-8 byte column)
} AiSpan;

typedef enum AiDiagKind {
  AI_DIAG_ERROR = 0,
} AiDiagKind;

typedef void (*AiDiagSinkFn)(void* user, AiDiagKind kind, const char* filename, AiSpan span,
                             const char* message);

typedef struct AiDiag {
  const char* filename;
  int error_count;
  AiDiagSinkFn sink;
  void* sink_user;
} AiDiag;

void ai_diag_init(AiDiag* diag, const char* filename);
void ai_diag_set_sink(AiDiag* diag, AiDiagSinkFn sink, void* user);
void ai_diag_error(AiDiag* diag, AiSpan span, const char* fmt, ...);
