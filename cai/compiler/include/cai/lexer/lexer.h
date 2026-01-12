#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "cai/diag/diag.h"

/*
 * Lexer for CAI source files.
 *
 * The lexer converts a UTF-8 source buffer into a flat token stream.
 * Newlines are significant: `\n` is tokenized as `AI_TOK_SEMI` (statement
 * separator), consistent with `cai/parser.g4`.
 */

typedef enum AiTokenKind {
  AI_TOK_EOF = 0,
  AI_TOK_SEMI,

  AI_TOK_IDENTIFIER,
  AI_TOK_INT_LIT,
  AI_TOK_LONG_LIT,
  AI_TOK_FLOAT_LIT,
  AI_TOK_STRING_LIT,

  // Punctuation / operators
  AI_TOK_AT,
  AI_TOK_ELLIPSIS,
  AI_TOK_DBLCOLON,
  AI_TOK_COALESCE,
  AI_TOK_ARROW,
  AI_TOK_FATARROW,
  AI_TOK_LE,
  AI_TOK_GE,
  AI_TOK_EQ,
  AI_TOK_NEQ,
  AI_TOK_AND,
  AI_TOK_OR,
  AI_TOK_INC,
  AI_TOK_DEC,

  AI_TOK_ASSIGN,
  AI_TOK_PLUS,
  AI_TOK_MINUS,
  AI_TOK_STAR,
  AI_TOK_SLASH,
  AI_TOK_LT,
  AI_TOK_GT,
  AI_TOK_NOT,
  AI_TOK_AMP,
  AI_TOK_BOR,
  AI_TOK_BXOR,
  AI_TOK_BNOT,
  AI_TOK_QUESTION,
  AI_TOK_DOT,
  AI_TOK_COLON,
  AI_TOK_COMMA,
  AI_TOK_UNDERSCORE,

  AI_TOK_LPAREN,
  AI_TOK_RPAREN,
  AI_TOK_LBRACE,
  AI_TOK_RBRACE,
  AI_TOK_LBRACK,
  AI_TOK_RBRACK,

  // Keywords
  AI_KW_LET,
  AI_KW_CONST,
  AI_KW_MUT,
  AI_KW_IF,
  AI_KW_MATCH,
  AI_KW_ELSE,
  AI_KW_FOR,
  AI_KW_WHILE,
  AI_KW_DO,
  AI_KW_UNSAFE,
  AI_KW_BREAK,
  AI_KW_CONTINUE,
  AI_KW_GOTO,
  AI_KW_IMPORT,
  AI_KW_FROM,
  AI_KW_AS,
  AI_KW_FN,
  AI_KW_EXTERN,
  AI_KW_PRIVATE,
  AI_KW_RETURN,
  AI_KW_STRUCT,
  AI_KW_OBJECT,
  AI_KW_ENUM,
  AI_KW_IMPL,
  AI_KW_TRAIT,
  AI_KW_INIT,
  AI_KW_DEINIT,
  AI_KW_THIS,
  AI_KW_MOVE,
  AI_KW_IN,
  AI_KW_PRINTLN,
  AI_KW_PRINT,
  AI_KW_NULL,
  AI_KW_TRUE,
  AI_KW_FALSE,
  AI_KW_ASYNC,
  AI_KW_AWAIT,
  AI_KW_DEFER,
  AI_KW_YIELD,
  AI_KW_EMBED,

  // Type keywords / builtins
  AI_KW_REF,
  AI_KW_ANY,
  AI_KW_VOID,
  AI_KW_INT,
  AI_KW_LONG,
  AI_KW_DOUBLE,
  AI_KW_FLOAT,
  AI_KW_STRING,
  AI_KW_BOOL,
  AI_KW_BYTE,
  AI_KW_U8,
  AI_KW_I8,
  AI_KW_U16,
  AI_KW_I16,
  AI_KW_U32,
  AI_KW_U64,
  AI_KW_USIZE,
  AI_KW_ISIZE,
  AI_KW_F8,
  AI_KW_F16,
  AI_KW_F32,
  AI_KW_F64,
  AI_KW_BF8,
  AI_KW_BF16,
} AiTokenKind;

typedef struct AiToken {
  AiTokenKind kind;
  AiSpan span;
  const char* text;
  size_t text_len;
} AiToken;

typedef struct AiTokens {
  AiToken* data;
  size_t len;
  size_t cap;
} AiTokens;

typedef struct AiLexer {
  const char* filename;
  const char* src;
  size_t len;
  size_t i;
  int line;
  int column;
  AiDiag* diag;
} AiLexer;

void ai_tokens_free(AiTokens* tokens);
bool ai_lex(AiLexer* lex, AiTokens* out_tokens);

