#ifndef TUA_TAILREC_H
#define TUA_TAILREC_H

#include "compiler.h"
#include "parser.h"

// Tries to rewrite a return statement of the form:
//   return f(args...)
// into a self tail-call loop:
//   store args into param slots; br tailrecLoop;
// Returns 1 if rewritten, 0 otherwise.
int tailrecTryRewriteReturn(Compiler* compiler, ReturnStmt* stmt);

#endif

