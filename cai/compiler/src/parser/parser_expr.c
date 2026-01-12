#include "parser/parser_internal.h"

/*
 * Expression parsing (precedence climbing).
 *
 * Current parser is intentionally syntax-focused (for `check` and prototype
 * `emit-llvm`). Type checking and MIR lowering will add richer structures later.
 */

static bool parse_primary(AiParser* p) {
  if (ai_p_at(p, AI_TOK_INT_LIT) || ai_p_at(p, AI_TOK_LONG_LIT) || ai_p_at(p, AI_TOK_FLOAT_LIT) ||
      ai_p_at(p, AI_TOK_STRING_LIT)) {
    p->i += 1;
    return true;
  }
  if (ai_p_consume_if(p, AI_KW_NULL) || ai_p_consume_if(p, AI_KW_TRUE) ||
      ai_p_consume_if(p, AI_KW_FALSE)) {
    return true;
  }
  if (ai_p_consume_if(p, AI_KW_THIS)) return true;
  if (ai_p_consume_if(p, AI_KW_PRINT) || ai_p_consume_if(p, AI_KW_PRINTLN)) return true;
  if (ai_p_consume_if(p, AI_KW_EMBED)) return ai_p_consume(p, AI_TOK_STRING_LIT);

  if (ai_p_consume_if(p, AI_TOK_LPAREN)) {
    bool saved = p->stop_before_lbrace_suffix;
    p->stop_before_lbrace_suffix = false;
    if (!ai_p_parse_expression(p)) return false;
    p->stop_before_lbrace_suffix = saved;
    return ai_p_consume(p, AI_TOK_RPAREN);
  }

  if (ai_p_consume_if(p, AI_TOK_LBRACK)) {
    ai_p_skip_seps(p);
    if (!ai_p_at(p, AI_TOK_RBRACK)) {
      if (!ai_p_parse_expression(p)) return false;
      while (ai_p_consume_if(p, AI_TOK_COMMA)) {
        ai_p_skip_seps(p);
        if (ai_p_at(p, AI_TOK_RBRACK)) break;
        if (!ai_p_parse_expression(p)) return false;
      }
    }
    ai_p_skip_seps(p);
    return ai_p_consume(p, AI_TOK_RBRACK);
  }

  if (ai_p_consume_if(p, AI_TOK_LBRACE)) {
    // Map literal: `{ key: value, ... }`
    ai_p_skip_seps(p);
    if (!ai_p_at(p, AI_TOK_RBRACE)) {
      if (!ai_p_parse_expression(p)) return false;
      if (!ai_p_consume(p, AI_TOK_COLON)) return false;
      if (!ai_p_parse_expression(p)) return false;
      while (ai_p_consume_if(p, AI_TOK_COMMA)) {
        ai_p_skip_seps(p);
        if (ai_p_at(p, AI_TOK_RBRACE)) break;
        if (!ai_p_parse_expression(p)) return false;
        if (!ai_p_consume(p, AI_TOK_COLON)) return false;
        if (!ai_p_parse_expression(p)) return false;
      }
    }
    ai_p_skip_seps(p);
    return ai_p_consume(p, AI_TOK_RBRACE);
  }

  if (ai_p_consume_if(p, AI_TOK_IDENTIFIER)) {
    while (ai_p_consume_if(p, AI_TOK_DOT)) {
      if (!ai_p_consume(p, AI_TOK_IDENTIFIER)) return false;
    }
    return true;
  }

  ai_diag_error(p->diag, ai_p_tok(p)->span, "expected expression");
  return false;
}

static bool parse_postfix(AiParser* p) {
  if (!parse_primary(p)) return false;
  for (;;) {
    if (ai_p_consume_if(p, AI_TOK_DOT)) {
      if (!ai_p_consume(p, AI_TOK_IDENTIFIER)) return false;
      continue;
    }

    if (ai_p_consume_if(p, AI_TOK_LPAREN)) {
      if (!ai_p_at(p, AI_TOK_RPAREN)) {
        if (!ai_p_parse_expression(p)) return false;
        while (ai_p_consume_if(p, AI_TOK_COMMA)) {
          if (!ai_p_parse_expression(p)) return false;
        }
      }
      if (!ai_p_consume(p, AI_TOK_RPAREN)) return false;
      continue;
    }

    if (ai_p_consume_if(p, AI_TOK_LBRACK)) {
      if (!ai_p_parse_expression(p)) return false;
      if (!ai_p_consume(p, AI_TOK_RBRACK)) return false;
      continue;
    }

    if (ai_p_at(p, AI_TOK_LBRACE)) {
      // In places like `if expr { ... }`, do not treat `{ ... }` as a suffix.
      if (p->stop_before_lbrace_suffix) {
        break;
      }
      ai_p_consume_if(p, AI_TOK_LBRACE);

      // Struct init suffix: `{ a: expr, ... }`
      ai_p_skip_seps(p);
      if (!ai_p_at(p, AI_TOK_RBRACE)) {
        if (!ai_p_consume(p, AI_TOK_IDENTIFIER)) return false;
        if (!ai_p_consume(p, AI_TOK_COLON)) return false;
        if (!ai_p_parse_expression(p)) return false;
        while (ai_p_consume_if(p, AI_TOK_COMMA)) {
          ai_p_skip_seps(p);
          if (ai_p_at(p, AI_TOK_RBRACE)) break;
          if (!ai_p_consume(p, AI_TOK_IDENTIFIER)) return false;
          if (!ai_p_consume(p, AI_TOK_COLON)) return false;
          if (!ai_p_parse_expression(p)) return false;
        }
      }
      ai_p_skip_seps(p);
      if (!ai_p_consume(p, AI_TOK_RBRACE)) return false;
      continue;
    }

    // Guard suffix: `callExpr ? { ... }` (syntax only here).
    if (ai_p_consume_if(p, AI_TOK_QUESTION)) {
      if (!ai_p_parse_block(p)) return false;
      continue;
    }

    if (ai_p_consume_if(p, AI_TOK_INC) || ai_p_consume_if(p, AI_TOK_DEC)) {
      continue;
    }

    if (ai_p_consume_if(p, AI_KW_AS)) {
      if (!ai_p_parse_type(p)) return false;
      continue;
    }

    break;
  }
  return true;
}

static int precedence(AiTokenKind k) {
  switch (k) {
    case AI_TOK_STAR:
    case AI_TOK_SLASH: return 60;
    case AI_TOK_PLUS:
    case AI_TOK_MINUS: return 50;
    case AI_TOK_LT:
    case AI_TOK_GT:
    case AI_TOK_LE:
    case AI_TOK_GE: return 40;
    case AI_TOK_EQ:
    case AI_TOK_NEQ: return 35;
    case AI_TOK_AMP: return 30;
    case AI_TOK_BXOR: return 29;
    case AI_TOK_BOR: return 28;
    case AI_TOK_AND: return 20;
    case AI_TOK_OR: return 19;
    case AI_TOK_COALESCE: return 10;
    default: return -1;
  }
}

static bool parse_unary(AiParser* p) {
  if (ai_p_at(p, AI_TOK_INC) || ai_p_at(p, AI_TOK_DEC) || ai_p_at(p, AI_TOK_MINUS) ||
      ai_p_at(p, AI_TOK_NOT) || ai_p_at(p, AI_TOK_BNOT) || ai_p_at(p, AI_TOK_AMP) ||
      ai_p_at(p, AI_KW_AWAIT)) {
    if (ai_p_at(p, AI_KW_AWAIT) && p->async_depth == 0) {
      ai_diag_error(p->diag, ai_p_tok(p)->span, "`await` is only allowed inside async functions");
      return false;
    }
    p->i += 1;
    return parse_unary(p);
  }
  return parse_postfix(p);
}

static bool parse_binary_rhs(AiParser* p, int min_prec) {
  if (!parse_unary(p)) return false;
  for (;;) {
    int prec = precedence(ai_p_tok(p)->kind);
    if (prec < min_prec) break;
    AiTokenKind op = ai_p_tok(p)->kind;
    p->i += 1;
    int next_min = prec + 1;
    // Right-assoc for coalesce.
    if (op == AI_TOK_COALESCE) next_min = prec;
    if (!parse_binary_rhs(p, next_min)) return false;
  }
  return true;
}

bool ai_p_parse_expression(AiParser* p) {
  // Assignment (right associative).
  if (!parse_binary_rhs(p, 0)) return false;
  if (ai_p_consume_if(p, AI_TOK_ASSIGN)) {
    return ai_p_parse_expression(p);
  }
  return true;
}
