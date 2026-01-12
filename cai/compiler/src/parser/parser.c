#include "parser/parser_internal.h"

/*
 * `caic` parser entry point.
 *
 * The implementation is split across multiple small `parser_*.c` files.
 * This file only contains the compilation-unit driver.
 */

bool ai_parse_compilation_unit(AiParser* p) {
  ai_p_skip_seps(p);
  while (!ai_p_at(p, AI_TOK_EOF)) {
    if (ai_p_consume_if(p, AI_TOK_SEMI)) continue;

    if (ai_p_parse_declaration(p)) {
      ai_p_skip_seps(p);
      continue;
    }
    if (ai_p_parse_statement(p)) {
      ai_p_skip_seps(p);
      continue;
    }

    ai_diag_error(p->diag, ai_p_tok(p)->span, "unexpected token");
    p->i += 1;
    ai_p_skip_seps(p);
  }
  return p->diag->error_count == 0;
}
