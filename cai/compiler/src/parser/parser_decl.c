#include "parser/parser_internal.h"

/*
 * Declaration parsing (top-level items and declaration forms inside blocks).
 *
 * Note: CAI grammar allows many constructs; `caic` currently implements a
 * pragmatic subset focused on syntax checking and collecting function signatures
 * for the prototype LLVM backend.
 */

static bool parse_function_decl_like(AiParser* p, bool is_async) {
  const AiToken* name_tok = ai_p_tok(p);
  if (!ai_p_consume(p, AI_TOK_IDENTIFIER)) return false;

  AiFuncSig* collecting = NULL;
  if (p->module) {
    AiIdent id;
    id.text = name_tok->text;
    id.len = name_tok->text_len;
    id.span = name_tok->span;
    collecting = ai_module_add_func(p->module, id);
  }

  // Optional type params: `<T, U, ...>`
  if (ai_p_at(p, AI_TOK_LT)) {
    if (!ai_p_parse_type_args(p)) return false;
  }

  if (!ai_p_consume(p, AI_TOK_LPAREN)) return false;
  if (!ai_p_parse_param_list(p, collecting)) return false;
  if (!ai_p_consume(p, AI_TOK_RPAREN)) return false;

  if (collecting) {
    // Signature-only return type extraction.
    collecting->ret = AI_TYPE_VOID;
    if (ai_p_consume_if(p, AI_TOK_ARROW)) {
      AiTypeKind ty;
      if (!ai_p_parse_type_kind_for_sig(p, &ty)) return false;
      collecting->ret = ty;
      if (ai_p_consume_if(p, AI_TOK_COMMA)) {
        ai_diag_error(p->diag, ai_p_tok(p)->span, "emit-llvm: multi-return not supported yet");
        return false;
      }
    } else if (ai_p_is_primitive_type(ai_p_tok(p)->kind)) {
      AiTypeKind ty;
      if (!ai_p_parse_type_kind_for_sig(p, &ty)) return false;
      collecting->ret = ty;
      if (ai_p_consume_if(p, AI_TOK_COMMA)) {
        ai_diag_error(p->diag, ai_p_tok(p)->span, "emit-llvm: multi-return not supported yet");
        return false;
      }
    }
  } else {
    if (!ai_p_parse_return_sig(p)) return false;
  }

  if (is_async) p->async_depth += 1;
  bool ok = ai_p_parse_block(p);
  if (is_async) p->async_depth -= 1;
  return ok;
}

static bool parse_function_decl(AiParser* p) {
  bool is_async = ai_p_consume_if(p, AI_KW_ASYNC);
  if (!ai_p_consume(p, AI_KW_FN)) return false;
  return parse_function_decl_like(p, is_async);
}

static bool parse_extern_fn_decl(AiParser* p) {
  if (!ai_p_consume(p, AI_KW_EXTERN)) return false;
  if (!ai_p_consume(p, AI_KW_FN)) return false;

  const AiToken* name_tok = ai_p_tok(p);
  if (!ai_p_consume(p, AI_TOK_IDENTIFIER)) return false;

  AiFuncSig* collecting = NULL;
  if (p->module) {
    AiIdent id;
    id.text = name_tok->text;
    id.len = name_tok->text_len;
    id.span = name_tok->span;
    collecting = ai_module_add_func(p->module, id);
  }

  if (!ai_p_consume(p, AI_TOK_LPAREN)) return false;
  if (!ai_p_parse_param_list(p, collecting)) return false;
  if (!ai_p_consume(p, AI_TOK_RPAREN)) return false;

  // Extern return sig: allow either `->` or direct type list.
  if (collecting) {
    collecting->ret = AI_TYPE_VOID;
    if (ai_p_consume_if(p, AI_TOK_ARROW)) {
      AiTypeKind ty;
      if (!ai_p_parse_type_kind_for_sig(p, &ty)) return false;
      collecting->ret = ty;
      if (ai_p_consume_if(p, AI_TOK_COMMA)) {
        ai_diag_error(p->diag, ai_p_tok(p)->span, "emit-llvm: multi-return not supported yet");
        return false;
      }
    } else if (!ai_p_at(p, AI_TOK_SEMI) && !ai_p_at(p, AI_TOK_EOF) && !ai_p_at(p, AI_KW_AS)) {
      AiTypeKind ty;
      if (!ai_p_parse_type_kind_for_sig(p, &ty)) return false;
      collecting->ret = ty;
      if (ai_p_consume_if(p, AI_TOK_COMMA)) {
        ai_diag_error(p->diag, ai_p_tok(p)->span, "emit-llvm: multi-return not supported yet");
        return false;
      }
    }
  } else {
    if (ai_p_consume_if(p, AI_TOK_ARROW)) {
      if (!ai_p_parse_type_list(p)) return false;
    } else if (!ai_p_at(p, AI_TOK_SEMI) && !ai_p_at(p, AI_TOK_EOF) && !ai_p_at(p, AI_KW_AS)) {
      if (!ai_p_parse_type_list(p)) return false;
    }
  }

  if (ai_p_consume_if(p, AI_KW_AS)) {
    if (!ai_p_consume(p, AI_TOK_IDENTIFIER)) return false;
  }

  ai_p_consume_if(p, AI_TOK_SEMI);
  return true;
}

static bool parse_extern_block(AiParser* p) {
  if (!ai_p_consume(p, AI_KW_EXTERN)) return false;
  ai_p_skip_seps(p);
  if (!ai_p_consume(p, AI_TOK_LBRACE)) return false;
  ai_p_skip_seps(p);

  while (!ai_p_at(p, AI_TOK_RBRACE) && !ai_p_at(p, AI_TOK_EOF)) {
    if (ai_p_consume_if(p, AI_TOK_SEMI)) continue;

    // attr* not implemented yet; skip leading '@...'
    while (ai_p_consume_if(p, AI_TOK_AT)) {
      if (!ai_p_consume(p, AI_TOK_IDENTIFIER)) return false;
      if (ai_p_consume_if(p, AI_TOK_LPAREN)) {
        int depth = 1;
        while (depth > 0 && !ai_p_at(p, AI_TOK_EOF)) {
          if (ai_p_consume_if(p, AI_TOK_LPAREN)) depth++;
          else if (ai_p_consume_if(p, AI_TOK_RPAREN)) depth--;
          else p->i += 1;
        }
      }
      ai_p_skip_seps(p);
    }

    if (!ai_p_consume(p, AI_KW_FN)) return false;

    const AiToken* name_tok = ai_p_tok(p);
    if (!ai_p_consume(p, AI_TOK_IDENTIFIER)) return false;

    AiFuncSig* collecting = NULL;
    if (p->module) {
      AiIdent id;
      id.text = name_tok->text;
      id.len = name_tok->text_len;
      id.span = name_tok->span;
      collecting = ai_module_add_func(p->module, id);
    }

    if (!ai_p_consume(p, AI_TOK_LPAREN)) return false;
    if (!ai_p_parse_param_list(p, collecting)) return false;
    if (!ai_p_consume(p, AI_TOK_RPAREN)) return false;

    if (collecting) {
      collecting->ret = AI_TYPE_VOID;
      if (ai_p_consume_if(p, AI_TOK_ARROW)) {
        AiTypeKind ty;
        if (!ai_p_parse_type_kind_for_sig(p, &ty)) return false;
        collecting->ret = ty;
        if (ai_p_consume_if(p, AI_TOK_COMMA)) {
          ai_diag_error(p->diag, ai_p_tok(p)->span, "emit-llvm: multi-return not supported yet");
          return false;
        }
      } else if (!ai_p_at(p, AI_TOK_SEMI) && !ai_p_at(p, AI_KW_AS) && !ai_p_at(p, AI_TOK_RBRACE)) {
        AiTypeKind ty;
        if (!ai_p_parse_type_kind_for_sig(p, &ty)) return false;
        collecting->ret = ty;
        if (ai_p_consume_if(p, AI_TOK_COMMA)) {
          ai_diag_error(p->diag, ai_p_tok(p)->span, "emit-llvm: multi-return not supported yet");
          return false;
        }
      }
    } else {
      if (ai_p_consume_if(p, AI_TOK_ARROW)) {
        if (!ai_p_parse_type_list(p)) return false;
      } else if (!ai_p_at(p, AI_TOK_SEMI) && !ai_p_at(p, AI_KW_AS) && !ai_p_at(p, AI_TOK_RBRACE)) {
        if (!ai_p_parse_type_list(p)) return false;
      }
    }

    if (ai_p_consume_if(p, AI_KW_AS)) {
      if (!ai_p_consume(p, AI_TOK_IDENTIFIER)) return false;
    }

    ai_p_consume_if(p, AI_TOK_SEMI);
    ai_p_skip_seps(p);
  }

  if (!ai_p_consume(p, AI_TOK_RBRACE)) return false;
  ai_p_consume_if(p, AI_TOK_SEMI);
  return true;
}

static bool parse_struct_decl(AiParser* p) {
  if (!ai_p_consume(p, AI_KW_STRUCT)) return false;
  if (!ai_p_consume(p, AI_TOK_IDENTIFIER)) return false;
  ai_p_skip_seps(p);
  if (!ai_p_consume(p, AI_TOK_LBRACE)) return false;
  ai_p_skip_seps(p);

  while (!ai_p_at(p, AI_TOK_RBRACE) && !ai_p_at(p, AI_TOK_EOF)) {
    if (ai_p_consume_if(p, AI_TOK_SEMI)) continue;
    ai_p_consume_if(p, AI_KW_PRIVATE);

    bool is_async = ai_p_consume_if(p, AI_KW_ASYNC);
    if (ai_p_consume_if(p, AI_KW_FN)) {
      if (!parse_function_decl_like(p, is_async)) return false;
      ai_p_skip_seps(p);
      continue;
    }
    if (is_async) {
      ai_diag_error(p->diag, ai_p_tok(p)->span, "expected fn after async");
      return false;
    }

    if (ai_p_consume_if(p, AI_KW_INIT) || ai_p_consume_if(p, AI_KW_DEINIT)) {
      if (!ai_p_consume(p, AI_TOK_LPAREN)) return false;
      if (!ai_p_consume(p, AI_TOK_RPAREN)) return false;
      (void)ai_p_parse_return_sig(p);
      if (!ai_p_parse_block(p)) return false;
      ai_p_skip_seps(p);
      continue;
    }

    if (ai_p_consume_if(p, AI_TOK_ELLIPSIS)) {
      if (!ai_p_parse_type(p)) return false;
      if (ai_p_consume_if(p, AI_KW_AS)) {
        if (!ai_p_consume(p, AI_TOK_IDENTIFIER)) return false;
      }
      ai_p_skip_seps(p);
      continue;
    }

    // Field: [const] name : type (= expr)?
    ai_p_consume_if(p, AI_KW_CONST);
    if (!ai_p_consume(p, AI_TOK_IDENTIFIER)) return false;
    if (!ai_p_consume(p, AI_TOK_COLON)) return false;
    if (!ai_p_parse_type(p)) return false;
    if (ai_p_consume_if(p, AI_TOK_ASSIGN)) {
      if (!ai_p_parse_expression(p)) return false;
    }
    ai_p_skip_seps(p);
  }

  return ai_p_consume(p, AI_TOK_RBRACE);
}

static bool parse_enum_decl(AiParser* p) {
  if (!ai_p_consume(p, AI_KW_ENUM)) return false;
  if (!ai_p_consume(p, AI_TOK_IDENTIFIER)) return false;
  ai_p_skip_seps(p);
  if (!ai_p_consume(p, AI_TOK_LBRACE)) return false;
  ai_p_skip_seps(p);

  if (ai_p_at(p, AI_TOK_RBRACE)) {
    p->i += 1;
    return true;
  }

  for (;;) {
    if (!ai_p_consume(p, AI_TOK_IDENTIFIER)) return false;
    if (ai_p_consume_if(p, AI_TOK_LPAREN)) {
      if (!ai_p_at(p, AI_TOK_RPAREN)) {
        if (!ai_p_parse_type_list(p)) return false;
      }
      if (!ai_p_consume(p, AI_TOK_RPAREN)) return false;
    }
    if (ai_p_consume_if(p, AI_TOK_ASSIGN)) {
      if (!(ai_p_at(p, AI_TOK_INT_LIT) || ai_p_at(p, AI_TOK_LONG_LIT))) {
        ai_diag_error(p->diag, ai_p_tok(p)->span, "expected integer literal for enum discriminant");
        return false;
      }
      p->i += 1;
    }

    // Next variant or end: require at least one separator.
    if (ai_p_at(p, AI_TOK_RBRACE)) break;
    if (!ai_p_consume(p, AI_TOK_SEMI)) return false;
    ai_p_skip_seps(p);
    if (ai_p_at(p, AI_TOK_RBRACE)) break;
  }

  return ai_p_consume(p, AI_TOK_RBRACE);
}

static bool parse_object_decl(AiParser* p) {
  if (!ai_p_consume(p, AI_KW_OBJECT)) return false;
  if (!ai_p_consume(p, AI_TOK_IDENTIFIER)) return false;
  ai_p_skip_seps(p);
  if (!ai_p_consume(p, AI_TOK_LBRACE)) return false;
  ai_p_skip_seps(p);

  while (!ai_p_at(p, AI_TOK_RBRACE) && !ai_p_at(p, AI_TOK_EOF)) {
    if (ai_p_consume_if(p, AI_TOK_SEMI)) continue;
    ai_p_consume_if(p, AI_KW_PRIVATE);
    bool is_async = ai_p_consume_if(p, AI_KW_ASYNC);
    if (!ai_p_consume(p, AI_KW_FN)) return false;
    if (!parse_function_decl_like(p, is_async)) return false;
    ai_p_skip_seps(p);
  }

  return ai_p_consume(p, AI_TOK_RBRACE);
}

static bool parse_trait_decl(AiParser* p) {
  if (!ai_p_consume(p, AI_KW_TRAIT)) return false;
  if (!ai_p_consume(p, AI_TOK_IDENTIFIER)) return false;
  ai_p_skip_seps(p);
  if (!ai_p_consume(p, AI_TOK_LBRACE)) return false;
  ai_p_skip_seps(p);

  while (!ai_p_at(p, AI_TOK_RBRACE) && !ai_p_at(p, AI_TOK_EOF)) {
    if (ai_p_consume_if(p, AI_TOK_SEMI)) continue;
    ai_p_consume_if(p, AI_KW_PRIVATE);
    ai_p_consume_if(p, AI_KW_ASYNC);
    if (!ai_p_consume(p, AI_KW_FN)) return false;
    if (!ai_p_consume(p, AI_TOK_IDENTIFIER)) return false;
    if (!ai_p_consume(p, AI_TOK_LPAREN)) return false;

    if (!ai_p_at(p, AI_TOK_RPAREN)) {
      // Trait params require `name: type`.
      for (;;) {
        if (ai_p_at(p, AI_KW_CONST) || ai_p_at(p, AI_KW_MUT) || ai_p_at(p, AI_KW_MOVE)) {
          p->i += 1;
        }
        if (!ai_p_consume(p, AI_TOK_IDENTIFIER)) return false;
        if (!ai_p_consume(p, AI_TOK_COLON)) return false;
        if (!ai_p_parse_type(p)) return false;
        if (!ai_p_consume_if(p, AI_TOK_COMMA)) break;
      }
    }

    if (!ai_p_consume(p, AI_TOK_RPAREN)) return false;

    // Allow either normal return sig or extern-style return sig.
    if (ai_p_consume_if(p, AI_TOK_ARROW)) {
      if (!ai_p_parse_type_list(p)) return false;
    } else if (ai_p_is_primitive_type(ai_p_tok(p)->kind) || ai_p_at(p, AI_TOK_IDENTIFIER) ||
               ai_p_at(p, AI_TOK_AMP) || ai_p_at(p, AI_KW_REF)) {
      if (!ai_p_parse_type_list(p)) return false;
    }

    ai_p_consume_if(p, AI_TOK_SEMI);
    ai_p_skip_seps(p);
  }

  return ai_p_consume(p, AI_TOK_RBRACE);
}

static bool parse_impl_decl(AiParser* p) {
  if (!ai_p_consume(p, AI_KW_IMPL)) return false;
  if (!ai_p_consume(p, AI_TOK_IDENTIFIER)) return false;
  if (ai_p_consume_if(p, AI_KW_FOR)) {
    if (!ai_p_consume(p, AI_TOK_IDENTIFIER)) return false;
  }
  ai_p_skip_seps(p);
  if (!ai_p_consume(p, AI_TOK_LBRACE)) return false;
  ai_p_skip_seps(p);

  while (!ai_p_at(p, AI_TOK_RBRACE) && !ai_p_at(p, AI_TOK_EOF)) {
    if (ai_p_consume_if(p, AI_TOK_SEMI)) continue;
    ai_p_consume_if(p, AI_KW_PRIVATE);
    bool is_async = ai_p_consume_if(p, AI_KW_ASYNC);
    if (!ai_p_consume(p, AI_KW_FN)) return false;
    if (!parse_function_decl_like(p, is_async)) return false;
    ai_p_skip_seps(p);
  }

  return ai_p_consume(p, AI_TOK_RBRACE);
}

static bool parse_import_name(AiParser* p) {
  if (!ai_p_consume(p, AI_TOK_IDENTIFIER)) return false;
  if (ai_p_consume_if(p, AI_KW_AS)) {
    if (!ai_p_consume(p, AI_TOK_IDENTIFIER)) return false;
  }
  return true;
}

static bool parse_import_decl(AiParser* p) {
  if (!ai_p_consume(p, AI_KW_IMPORT)) return false;

  if (ai_p_at(p, AI_TOK_STRING_LIT)) {
    p->i += 1;
    if (ai_p_consume_if(p, AI_KW_AS)) {
      if (!ai_p_consume(p, AI_TOK_IDENTIFIER)) return false;
    }
    return true;
  }

  if (!parse_import_name(p)) return false;
  while (ai_p_consume_if(p, AI_TOK_COMMA)) {
    if (!parse_import_name(p)) return false;
  }
  if (!ai_p_consume(p, AI_KW_FROM)) return false;
  if (!ai_p_consume(p, AI_TOK_STRING_LIT)) return false;
  return true;
}

static bool parse_from_import_decl(AiParser* p) {
  if (!ai_p_consume(p, AI_KW_FROM)) return false;
  if (!ai_p_consume(p, AI_TOK_STRING_LIT)) return false;
  if (!ai_p_consume(p, AI_KW_IMPORT)) return false;
  if (!parse_import_name(p)) return false;
  while (ai_p_consume_if(p, AI_TOK_COMMA)) {
    if (!parse_import_name(p)) return false;
  }
  return true;
}

bool ai_p_parse_var_decl(AiParser* p) {
  bool is_const = ai_p_consume_if(p, AI_KW_CONST);
  if (!is_const) {
    if (!ai_p_consume(p, AI_KW_LET)) return false;
  }

  // Support:
  // - name[:T] (= expr)?
  // - name[:T], name2[:U] (= expr)?
  // Destructure is not implemented yet.
  if (!ai_p_consume(p, AI_TOK_IDENTIFIER)) return false;
  if (ai_p_consume_if(p, AI_TOK_COLON)) {
    if (!ai_p_parse_type(p)) return false;
  }
  if (ai_p_consume_if(p, AI_TOK_ASSIGN)) {
    if (!ai_p_parse_expression(p)) return false;
  }
  while (ai_p_consume_if(p, AI_TOK_COMMA)) {
    if (!ai_p_consume(p, AI_TOK_IDENTIFIER)) return false;
    if (ai_p_consume_if(p, AI_TOK_COLON)) {
      if (!ai_p_parse_type(p)) return false;
    }
    if (ai_p_consume_if(p, AI_TOK_ASSIGN)) {
      if (!ai_p_parse_expression(p)) return false;
    }
  }
  ai_p_consume_if(p, AI_TOK_SEMI);
  return true;
}

bool ai_p_parse_typed_var_sugar(AiParser* p) {
  if (!ai_p_consume(p, AI_TOK_IDENTIFIER)) return false;
  if (!ai_p_consume(p, AI_TOK_COLON)) return false;
  if (!ai_p_parse_type(p)) return false;
  if (ai_p_consume_if(p, AI_TOK_ASSIGN)) {
    if (!ai_p_parse_expression(p)) return false;
  }
  ai_p_consume_if(p, AI_TOK_SEMI);
  return true;
}

bool ai_p_parse_declaration(AiParser* p) {
  // Attributes are ignored for now.
  while (ai_p_consume_if(p, AI_TOK_AT)) {
    if (!ai_p_consume(p, AI_TOK_IDENTIFIER)) return false;
    if (ai_p_consume_if(p, AI_TOK_LPAREN)) {
      int depth = 1;
      while (depth > 0 && !ai_p_at(p, AI_TOK_EOF)) {
        if (ai_p_consume_if(p, AI_TOK_LPAREN)) depth++;
        else if (ai_p_consume_if(p, AI_TOK_RPAREN)) depth--;
        else p->i += 1;
      }
      ai_p_skip_seps(p);
    } else {
      ai_p_skip_seps(p);
    }
  }

  if (ai_p_consume_if(p, AI_KW_PRIVATE)) {
    return ai_p_parse_declaration(p);
  }
  if (ai_p_at(p, AI_KW_IMPORT)) return parse_import_decl(p);
  if (ai_p_at(p, AI_KW_FROM)) return parse_from_import_decl(p);
  if (ai_p_at(p, AI_KW_ASYNC) || ai_p_at(p, AI_KW_FN)) return parse_function_decl(p);
  if (ai_p_at(p, AI_KW_EXTERN) && ai_p_at_n(p, 1, AI_TOK_LBRACE)) return parse_extern_block(p);
  if (ai_p_at(p, AI_KW_EXTERN)) return parse_extern_fn_decl(p);
  if (ai_p_at(p, AI_KW_STRUCT)) return parse_struct_decl(p);
  if (ai_p_at(p, AI_KW_ENUM)) return parse_enum_decl(p);
  if (ai_p_at(p, AI_KW_OBJECT)) return parse_object_decl(p);
  if (ai_p_at(p, AI_KW_TRAIT)) return parse_trait_decl(p);
  if (ai_p_at(p, AI_KW_IMPL)) return parse_impl_decl(p);

  return false;
}
