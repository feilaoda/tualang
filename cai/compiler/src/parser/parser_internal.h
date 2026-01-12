#pragma once

/*
 * Internal parser API for `caic`.
 *
 * This header is intentionally NOT installed under `include/` because it is an
 * implementation detail. All parser modules under `src/` include this file to
 * share common helpers and cross-module parse routines.
 */

#include "cai/parser.h"

// Token navigation.
const AiToken* ai_p_tok(const AiParser* p);
const AiToken* ai_p_tok_n(const AiParser* p, size_t n);
bool ai_p_at(const AiParser* p, AiTokenKind k);
bool ai_p_at_n(const AiParser* p, size_t n, AiTokenKind k);

// Error/reporting helpers.
const char* ai_p_tok_name(AiTokenKind k);
bool ai_p_consume(AiParser* p, AiTokenKind k);
bool ai_p_consume_if(AiParser* p, AiTokenKind k);
void ai_p_skip_seps(AiParser* p);

// Shared predicates.
bool ai_p_is_primitive_type(AiTokenKind k);

// Type parsing.
bool ai_p_parse_type(AiParser* p);
bool ai_p_parse_type_list(AiParser* p);
bool ai_p_parse_return_sig(AiParser* p);
bool ai_p_parse_type_args(AiParser* p);
bool ai_p_parse_type_kind_for_sig(AiParser* p, AiTypeKind* out);

// Function signature components.
bool ai_p_parse_param_list(AiParser* p, AiFuncSig* collecting);

// Expressions / statements / declarations.
bool ai_p_parse_expression(AiParser* p);
bool ai_p_parse_statement(AiParser* p);
bool ai_p_parse_block(AiParser* p);

bool ai_p_parse_var_decl(AiParser* p);
bool ai_p_parse_typed_var_sugar(AiParser* p);
bool ai_p_parse_declaration(AiParser* p);
