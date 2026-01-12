#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "cai/diag.h"
#include "cai/lexer.h"
#include "cai/parser.h"

typedef int (*AiTestFn)(void);

typedef struct AiTestCase {
  const char* name;
  AiTestFn fn;
} AiTestCase;

#define AI_TEST_ASSERT(cond)                                                     \
  do {                                                                           \
    if (!(cond)) {                                                               \
      fprintf(stderr, "assertion failed: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      return 1;                                                                  \
    }                                                                            \
  } while (0)

static inline int ai_test_parse_errors(const char* src) {
  AiDiag diag;
  ai_diag_init(&diag, "<test>");

  AiLexer lex = {0};
  lex.filename = "<test>";
  lex.src = src;
  lex.len = strlen(src);
  lex.i = 0;
  lex.line = 1;
  lex.column = 1;
  lex.diag = &diag;

  AiTokens tokens = {0};
  (void)ai_lex(&lex, &tokens);

  AiParser parser = {0};
  parser.filename = "<test>";
  parser.tokens = &tokens;
  parser.i = 0;
  parser.diag = &diag;
  parser.stop_before_lbrace_suffix = false;
  parser.block_depth = 0;
  parser.async_depth = 0;
  parser.module = NULL;

  (void)ai_parse_compilation_unit(&parser);

  int errors = diag.error_count;
  ai_tokens_free(&tokens);
  return errors;
}

