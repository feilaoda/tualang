#include "parser/parser_internal.h"

/*
 * Statement parsing.
 *
 * This module is responsible for block structure and statement-level constructs.
 * Many statement forms from the full grammar are not implemented yet; the goal
 * for now is to provide a reliable syntax checker and a base for MIR lowering.
 */

bool ai_p_parse_block(AiParser* p) {
  if (!ai_p_consume(p, AI_TOK_LBRACE)) return false;
  p->block_depth += 1;
  ai_p_skip_seps(p);
  while (!ai_p_at(p, AI_TOK_RBRACE) && !ai_p_at(p, AI_TOK_EOF)) {
    // Inside blocks, allow declarations or statements; for now treat everything as a statement.
    if (!ai_p_parse_statement(p)) {
      // Recovery: skip to next separator or '}'
      while (!ai_p_at(p, AI_TOK_SEMI) && !ai_p_at(p, AI_TOK_RBRACE) && !ai_p_at(p, AI_TOK_EOF)) {
        p->i += 1;
      }
    }
    ai_p_skip_seps(p);
  }
  bool ok = ai_p_consume(p, AI_TOK_RBRACE);
  p->block_depth -= 1;
  return ok;
}

static bool parse_match_stmt(AiParser* p) {
  if (!ai_p_consume(p, AI_KW_MATCH)) return false;
  bool saved = p->stop_before_lbrace_suffix;
  p->stop_before_lbrace_suffix = true;
  if (!ai_p_parse_expression(p)) return false;
  p->stop_before_lbrace_suffix = saved;

  ai_p_skip_seps(p);
  if (!ai_p_consume(p, AI_TOK_LBRACE)) return false;
  ai_p_skip_seps(p);
  while (!ai_p_at(p, AI_TOK_RBRACE) && !ai_p_at(p, AI_TOK_EOF)) {
    // pattern: '_' | literal | qualifiedName ('(' bindlist? ')')?
    if (ai_p_consume_if(p, AI_TOK_UNDERSCORE)) {
    } else if (ai_p_at(p, AI_TOK_INT_LIT) || ai_p_at(p, AI_TOK_LONG_LIT) || ai_p_at(p, AI_TOK_FLOAT_LIT) ||
               ai_p_at(p, AI_TOK_STRING_LIT) || ai_p_at(p, AI_KW_NULL) || ai_p_at(p, AI_KW_TRUE) ||
               ai_p_at(p, AI_KW_FALSE)) {
      p->i += 1;
    } else {
      if (!ai_p_consume(p, AI_TOK_IDENTIFIER)) return false;
      while (ai_p_consume_if(p, AI_TOK_DOT)) {
        if (!ai_p_consume(p, AI_TOK_IDENTIFIER)) return false;
      }
      if (ai_p_consume_if(p, AI_TOK_LPAREN)) {
        if (!ai_p_at(p, AI_TOK_RPAREN)) {
          if (!ai_p_consume(p, AI_TOK_IDENTIFIER)) return false;
          while (ai_p_consume_if(p, AI_TOK_COMMA)) {
            if (!ai_p_consume(p, AI_TOK_IDENTIFIER)) return false;
          }
        }
        if (!ai_p_consume(p, AI_TOK_RPAREN)) return false;
      }
    }
    if (!ai_p_consume(p, AI_TOK_FATARROW)) return false;
    if (!ai_p_parse_block(p)) return false;
    ai_p_skip_seps(p);
  }
  return ai_p_consume(p, AI_TOK_RBRACE);
}

static bool parse_if_stmt(AiParser* p) {
  if (!ai_p_consume(p, AI_KW_IF)) return false;

  if (ai_p_consume_if(p, AI_TOK_LPAREN)) {
    if (!ai_p_parse_expression(p)) return false;
    if (!ai_p_consume(p, AI_TOK_RPAREN)) return false;
  } else {
    bool saved = p->stop_before_lbrace_suffix;
    p->stop_before_lbrace_suffix = true;
    if (!ai_p_parse_expression(p)) return false;
    p->stop_before_lbrace_suffix = saved;
  }

  if (!ai_p_parse_statement(p)) return false;
  ai_p_skip_seps(p);
  if (ai_p_consume_if(p, AI_KW_ELSE)) {
    if (ai_p_at(p, AI_KW_IF)) return parse_if_stmt(p);
    return ai_p_parse_statement(p);
  }
  return true;
}

static bool parse_return_stmt(AiParser* p) {
  if (!ai_p_consume(p, AI_KW_RETURN)) return false;
  if (ai_p_at(p, AI_TOK_SEMI)) {
    p->i += 1;
    return true;
  }
  if (!ai_p_parse_expression(p)) return false;
  while (ai_p_consume_if(p, AI_TOK_COMMA)) {
    if (!ai_p_parse_expression(p)) return false;
  }
  ai_p_consume_if(p, AI_TOK_SEMI);
  return true;
}

bool ai_p_parse_statement(AiParser* p) {
  if (ai_p_consume_if(p, AI_TOK_SEMI)) return true;

  if (ai_p_at(p, AI_KW_YIELD)) {
    if (p->async_depth == 0) {
      ai_diag_error(p->diag, ai_p_tok(p)->span, "`yield` is only allowed inside async functions");
      p->i += 1;
      ai_p_consume_if(p, AI_TOK_SEMI);
      return true;
    }
    p->i += 1;
    ai_p_consume_if(p, AI_TOK_SEMI);
    return true;
  }

  if (ai_p_at(p, AI_TOK_LBRACE)) return ai_p_parse_block(p);
  if (ai_p_at(p, AI_KW_IF)) return parse_if_stmt(p);
  if (ai_p_at(p, AI_KW_MATCH)) return parse_match_stmt(p);
  if (ai_p_at(p, AI_KW_RETURN)) return parse_return_stmt(p);

  if (ai_p_at(p, AI_KW_UNSAFE)) {
    p->i += 1;
    return ai_p_parse_block(p);
  }
  if (ai_p_at(p, AI_KW_DEFER)) {
    p->i += 1;
    return ai_p_parse_statement(p);
  }
  if (ai_p_at(p, AI_KW_BREAK) || ai_p_at(p, AI_KW_CONTINUE)) {
    p->i += 1;
    ai_p_consume_if(p, AI_TOK_SEMI);
    return true;
  }
  if (ai_p_at(p, AI_KW_GOTO)) {
    p->i += 1;
    if (!ai_p_consume(p, AI_TOK_IDENTIFIER)) return false;
    ai_p_consume_if(p, AI_TOK_SEMI);
    return true;
  }

  // Allow var decls and typed var sugar inside blocks (top-level forbidden).
  if (ai_p_at(p, AI_KW_LET) || ai_p_at(p, AI_KW_CONST)) {
    if (p->block_depth == 0) {
      ai_diag_error(p->diag, ai_p_tok(p)->span, "top-level variable declarations are not allowed");
      while (!ai_p_at(p, AI_TOK_SEMI) && !ai_p_at(p, AI_TOK_EOF)) {
        p->i += 1;
      }
      ai_p_consume_if(p, AI_TOK_SEMI);
      return true;
    }
    return ai_p_parse_var_decl(p);
  }
  if (ai_p_at(p, AI_TOK_IDENTIFIER) && ai_p_at_n(p, 1, AI_TOK_COLON)) {
    if (p->block_depth == 0) {
      ai_diag_error(p->diag, ai_p_tok(p)->span, "top-level variable declarations are not allowed");
      while (!ai_p_at(p, AI_TOK_SEMI) && !ai_p_at(p, AI_TOK_EOF)) {
        p->i += 1;
      }
      ai_p_consume_if(p, AI_TOK_SEMI);
      return true;
    }
    return ai_p_parse_typed_var_sugar(p);
  }

  if (!ai_p_parse_expression(p)) return false;
  ai_p_consume_if(p, AI_TOK_SEMI);
  return true;
}
