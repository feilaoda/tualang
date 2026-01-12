#include "test_common.h"

int test_lexer_keywords(void) {
  const char* src = "yield\nasync\nawait\n";

  AiDiag diag;
  ai_diag_init(&diag, "<test>");

  AiLexer lex = {0};
  lex.filename = "<test>";
  lex.src = src;
  lex.len = strlen(src);
  lex.line = 1;
  lex.column = 1;
  lex.diag = &diag;

  AiTokens tokens = {0};
  bool ok = ai_lex(&lex, &tokens);
  AI_TEST_ASSERT(ok);
  AI_TEST_ASSERT(diag.error_count == 0);

  // `yield\nasync\nawait\n` => KW_YIELD, SEMI, KW_ASYNC, SEMI, KW_AWAIT, SEMI, EOF
  AI_TEST_ASSERT(tokens.len >= 7);
  AI_TEST_ASSERT(tokens.data[0].kind == AI_KW_YIELD);
  AI_TEST_ASSERT(tokens.data[1].kind == AI_TOK_SEMI);
  AI_TEST_ASSERT(tokens.data[2].kind == AI_KW_ASYNC);
  AI_TEST_ASSERT(tokens.data[3].kind == AI_TOK_SEMI);
  AI_TEST_ASSERT(tokens.data[4].kind == AI_KW_AWAIT);
  AI_TEST_ASSERT(tokens.data[5].kind == AI_TOK_SEMI);
  AI_TEST_ASSERT(tokens.data[tokens.len - 1].kind == AI_TOK_EOF);

  ai_tokens_free(&tokens);
  return 0;
}

