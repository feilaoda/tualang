#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "cai/diag/diag.h"
#include "cai/lexer/lexer.h"
#include "cai/module/module.h"

/*
 * Parser front-end for `caic`.
 *
 * At the current stage the parser is a syntax checker plus a lightweight
 * signature collector for `emit-llvm`. Later stages (AST + sema + MIR) will
 * extend this interface.
 */

typedef struct AiParser {
  const char* filename;
  const AiTokens* tokens;
  size_t i;
  AiDiag* diag;

  // Expression parsing needs this to disambiguate `if expr { ... }` from
  // `expr { ... }` as a struct-init suffix.
  bool stop_before_lbrace_suffix;

  // Optional signature collection sink.
  AiModule* module;

  // Simple context tracking.
  int block_depth;
  int async_depth;
} AiParser;

bool ai_parse_compilation_unit(AiParser* p);

