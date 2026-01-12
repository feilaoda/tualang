#include "parser/parser_internal.h"

/*
 * Parser common utilities.
 *
 * Keep this file small and dependency-free: only token navigation, separator
 * handling, and basic predicates used by multiple parse modules.
 */

const AiToken* ai_p_tok(const AiParser* p) { return &p->tokens->data[p->i]; }
const AiToken* ai_p_tok_n(const AiParser* p, size_t n) { return &p->tokens->data[p->i + n]; }
bool ai_p_at(const AiParser* p, AiTokenKind k) { return ai_p_tok(p)->kind == k; }
bool ai_p_at_n(const AiParser* p, size_t n, AiTokenKind k) { return ai_p_tok_n(p, n)->kind == k; }

const char* ai_p_tok_name(AiTokenKind k) {
  switch (k) {
    case AI_TOK_EOF: return "EOF";
    case AI_TOK_SEMI: return "SEMI";
    case AI_TOK_IDENTIFIER: return "Identifier";
    case AI_TOK_INT_LIT: return "INT_LIT";
    case AI_TOK_LONG_LIT: return "LONG_LIT";
    case AI_TOK_FLOAT_LIT: return "FLOAT_LIT";
    case AI_TOK_STRING_LIT: return "STRING_LIT";
    case AI_TOK_LPAREN: return "(";
    case AI_TOK_RPAREN: return ")";
    case AI_TOK_LBRACE: return "{";
    case AI_TOK_RBRACE: return "}";
    case AI_TOK_LBRACK: return "[";
    case AI_TOK_RBRACK: return "]";
    case AI_TOK_COMMA: return ",";
    case AI_TOK_COLON: return ":";
    case AI_TOK_ASSIGN: return "=";
    case AI_TOK_ARROW: return "->";
    case AI_KW_FN: return "fn";
    case AI_KW_RETURN: return "return";
    default: return "token";
  }
}

bool ai_p_consume(AiParser* p, AiTokenKind k) {
  if (!ai_p_at(p, k)) {
    ai_diag_error(p->diag, ai_p_tok(p)->span, "expected %s", ai_p_tok_name(k));
    return false;
  }
  p->i += 1;
  return true;
}

bool ai_p_consume_if(AiParser* p, AiTokenKind k) {
  if (ai_p_at(p, k)) {
    p->i += 1;
    return true;
  }
  return false;
}

void ai_p_skip_seps(AiParser* p) {
  while (ai_p_consume_if(p, AI_TOK_SEMI)) {
  }
}

bool ai_p_is_primitive_type(AiTokenKind k) {
  switch (k) {
    case AI_KW_INT:
    case AI_KW_LONG:
    case AI_KW_I8:
    case AI_KW_I16:
    case AI_KW_ISIZE:
    case AI_KW_U8:
    case AI_KW_U16:
    case AI_KW_U32:
    case AI_KW_U64:
    case AI_KW_USIZE:
    case AI_KW_BYTE:
    case AI_KW_FLOAT:
    case AI_KW_DOUBLE:
    case AI_KW_F8:
    case AI_KW_F16:
    case AI_KW_F32:
    case AI_KW_F64:
    case AI_KW_BF8:
    case AI_KW_BF16:
    case AI_KW_BOOL:
    case AI_KW_STRING:
    case AI_KW_VOID:
    case AI_KW_ANY: return true;
    default: return false;
  }
}
