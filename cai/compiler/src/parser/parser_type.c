#include "parser/parser_internal.h"

/*
 * Type and signature parsing.
 *
 * This module contains:
 * - full type parsing (for `check`)
 * - a restricted type parser for `emit-llvm` signature collection
 */

static bool type_kind_from_token(AiTokenKind k, AiTypeKind* out) {
  switch (k) {
    case AI_KW_VOID: *out = AI_TYPE_VOID; return true;
    case AI_KW_BOOL: *out = AI_TYPE_BOOL; return true;
    case AI_KW_BYTE:
    case AI_KW_I8: *out = AI_TYPE_I8; return true;
    case AI_KW_I16: *out = AI_TYPE_I16; return true;
    case AI_KW_INT: *out = AI_TYPE_I32; return true;
    case AI_KW_LONG: *out = AI_TYPE_I64; return true;
    case AI_KW_U8: *out = AI_TYPE_U8; return true;
    case AI_KW_U16: *out = AI_TYPE_U16; return true;
    case AI_KW_U32: *out = AI_TYPE_U32; return true;
    case AI_KW_U64: *out = AI_TYPE_U64; return true;
    case AI_KW_F32:
    case AI_KW_FLOAT: *out = AI_TYPE_F32; return true;
    case AI_KW_F64:
    case AI_KW_DOUBLE: *out = AI_TYPE_F64; return true;
    default: return false;
  }
}

bool ai_p_parse_type_kind_for_sig(AiParser* p, AiTypeKind* out) {
  AiTokenKind k = ai_p_tok(p)->kind;
  if (type_kind_from_token(k, out)) {
    p->i += 1;
    // Signature mode is intentionally strict for now.
    if (ai_p_at(p, AI_TOK_LBRACK) || ai_p_at(p, AI_TOK_LT)) {
      ai_diag_error(p->diag, ai_p_tok(p)->span, "emit-llvm: unsupported type form in signature");
      return false;
    }
    return true;
  }
  ai_diag_error(p->diag, ai_p_tok(p)->span, "emit-llvm: unsupported type in signature");
  return false;
}

bool ai_p_parse_type_args(AiParser* p) {
  if (!ai_p_consume_if(p, AI_TOK_LT)) {
    return true;
  }
  ai_p_skip_seps(p);
  if (!ai_p_parse_type(p)) return false;
  while (true) {
    ai_p_skip_seps(p);
    if (ai_p_consume_if(p, AI_TOK_COMMA)) {
      ai_p_skip_seps(p);
      if (!ai_p_parse_type(p)) return false;
      continue;
    }
    break;
  }
  ai_p_skip_seps(p);
  return ai_p_consume(p, AI_TOK_GT);
}

static bool parse_named_type(AiParser* p) {
  if (!ai_p_consume(p, AI_TOK_IDENTIFIER)) return false;
  while (ai_p_consume_if(p, AI_TOK_DOT)) {
    if (!ai_p_consume(p, AI_TOK_IDENTIFIER)) return false;
  }
  return ai_p_parse_type_args(p);
}

static bool parse_type_primary(AiParser* p) {
  if (ai_p_consume_if(p, AI_TOK_AMP)) {
    return ai_p_parse_type(p);
  }
  if (ai_p_consume_if(p, AI_KW_REF)) {
    if (!ai_p_consume(p, AI_TOK_LT)) return false;
    if (!ai_p_parse_type(p)) return false;
    return ai_p_consume(p, AI_TOK_GT);
  }
  if (ai_p_is_primitive_type(ai_p_tok(p)->kind)) {
    p->i += 1;
    return true;
  }
  if (ai_p_at(p, AI_TOK_IDENTIFIER)) {
    return parse_named_type(p);
  }
  ai_diag_error(p->diag, ai_p_tok(p)->span, "expected type");
  return false;
}

bool ai_p_parse_type(AiParser* p) {
  if (!parse_type_primary(p)) return false;
  while (ai_p_consume_if(p, AI_TOK_LBRACK)) {
    if (ai_p_consume_if(p, AI_TOK_RBRACK)) {
      continue;
    }
    if (!ai_p_consume(p, AI_TOK_INT_LIT)) return false;
    if (!ai_p_consume(p, AI_TOK_RBRACK)) return false;
  }
  return true;
}

bool ai_p_parse_type_list(AiParser* p) {
  if (!ai_p_parse_type(p)) return false;
  while (ai_p_consume_if(p, AI_TOK_COMMA)) {
    if (!ai_p_parse_type(p)) return false;
  }
  return true;
}

bool ai_p_parse_return_sig(AiParser* p) {
  if (ai_p_consume_if(p, AI_TOK_ARROW)) {
    return ai_p_parse_type_list(p);
  }

  // Primitive return sugar (without `->`).
  if (ai_p_is_primitive_type(ai_p_tok(p)->kind)) {
    p->i += 1;
    while (ai_p_consume_if(p, AI_TOK_COMMA)) {
      if (!ai_p_parse_type(p)) return false;
    }
    return true;
  }
  return true;
}

static bool parse_param(AiParser* p, AiFuncSig* collecting) {
  AiParamMode mode = AI_PARAM_CONST;
  if (ai_p_consume_if(p, AI_KW_CONST)) mode = AI_PARAM_CONST;
  else if (ai_p_consume_if(p, AI_KW_MUT)) mode = AI_PARAM_MUT;
  else if (ai_p_consume_if(p, AI_KW_MOVE)) mode = AI_PARAM_MOVE;

  const AiToken* name_tok = ai_p_tok(p);
  if (!ai_p_consume(p, AI_TOK_IDENTIFIER)) return false;

  if (!ai_p_consume_if(p, AI_TOK_COLON)) {
    if (collecting) {
      ai_diag_error(p->diag, name_tok->span, "emit-llvm: parameter '%.*s' requires explicit type",
                    (int)name_tok->text_len, name_tok->text);
      return false;
    }
    return true;
  }

  if (collecting) {
    AiTypeKind ty;
    if (!ai_p_parse_type_kind_for_sig(p, &ty)) return false;
    AiParamSig ps;
    ps.mode = mode;
    ps.name.text = name_tok->text;
    ps.name.len = name_tok->text_len;
    ps.name.span = name_tok->span;
    ps.type = ty;
    if (!ai_func_add_param(collecting, ps)) {
      ai_diag_error(p->diag, name_tok->span, "out of memory");
      return false;
    }
    return true;
  }

  return ai_p_parse_type(p);
}

bool ai_p_parse_param_list(AiParser* p, AiFuncSig* collecting) {
  if (ai_p_at(p, AI_TOK_RPAREN)) return true;
  if (!parse_param(p, collecting)) return false;
  while (ai_p_consume_if(p, AI_TOK_COMMA)) {
    if (!parse_param(p, collecting)) return false;
  }
  return true;
}
