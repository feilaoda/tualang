#include "cai/lexer.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/*
 * Lexer implementation.
 *
 * This lexer follows two key rules:
 * - newlines (`\n`) are tokenized as `AI_TOK_SEMI` (statement separator)
 * - keywords are case-sensitive (e.g. `Ref` is a type keyword, `ref` is an identifier)
 */

static void tokens_push(AiTokens* t, AiToken tok) {
  if (t->len == t->cap) {
    size_t new_cap = t->cap ? t->cap * 2 : 256;
    t->data = (AiToken*)realloc(t->data, new_cap * sizeof(AiToken));
    t->cap = new_cap;
  }
  t->data[t->len++] = tok;
}

void ai_tokens_free(AiTokens* tokens) {
  free(tokens->data);
  tokens->data = NULL;
  tokens->len = 0;
  tokens->cap = 0;
}

static inline bool at_eof(const AiLexer* lex) { return lex->i >= lex->len; }

static inline char peek(const AiLexer* lex) { return at_eof(lex) ? '\0' : lex->src[lex->i]; }

static inline char peek_n(const AiLexer* lex, size_t n) {
  size_t idx = lex->i + n;
  return (idx >= lex->len) ? '\0' : lex->src[idx];
}

static void bump(AiLexer* lex) {
  if (at_eof(lex)) {
    return;
  }
  char c = lex->src[lex->i++];
  if (c == '\n') {
    lex->line += 1;
    lex->column = 1;
  } else {
    lex->column += 1;
  }
}

static AiSpan span_make(size_t start_off, size_t end_off, int start_line, int start_col) {
  AiSpan s;
  s.offset = start_off;
  s.length = end_off - start_off;
  s.line = start_line;
  s.column = start_col;
  return s;
}

static bool match(AiLexer* lex, const char* s) {
  size_t n = strlen(s);
  if (lex->i + n > lex->len) {
    return false;
  }
  if (memcmp(lex->src + lex->i, s, n) != 0) {
    return false;
  }
  for (size_t k = 0; k < n; k++) {
    bump(lex);
  }
  return true;
}

static bool is_ident_start(char c) { return (c == '_') || isalpha((unsigned char)c); }
static bool is_ident_cont(char c) { return (c == '_') || isalnum((unsigned char)c); }

static AiTokenKind keyword_kind(const char* s, size_t n) {
  typedef struct {
    const char* word;
    AiTokenKind kind;
  } Entry;

  // Aliases: var->let, func->fn, boolean->bool.
  static const Entry table[] = {
      {"let", AI_KW_LET},       {"var", AI_KW_LET},     {"const", AI_KW_CONST},
      {"mut", AI_KW_MUT},       {"if", AI_KW_IF},       {"match", AI_KW_MATCH},
      {"else", AI_KW_ELSE},     {"for", AI_KW_FOR},     {"while", AI_KW_WHILE},
      {"do", AI_KW_DO},         {"unsafe", AI_KW_UNSAFE}, {"break", AI_KW_BREAK},
      {"continue", AI_KW_CONTINUE}, {"goto", AI_KW_GOTO}, {"return", AI_KW_RETURN},
      {"import", AI_KW_IMPORT}, {"from", AI_KW_FROM},   {"as", AI_KW_AS},
      {"fn", AI_KW_FN},         {"func", AI_KW_FN},     {"extern", AI_KW_EXTERN},
      {"private", AI_KW_PRIVATE}, {"struct", AI_KW_STRUCT}, {"object", AI_KW_OBJECT},
      {"enum", AI_KW_ENUM},     {"impl", AI_KW_IMPL},   {"trait", AI_KW_TRAIT},
      {"init", AI_KW_INIT},     {"deinit", AI_KW_DEINIT}, {"this", AI_KW_THIS},
      {"move", AI_KW_MOVE},     {"in", AI_KW_IN},       {"println", AI_KW_PRINTLN},
      {"print", AI_KW_PRINT},   {"null", AI_KW_NULL},   {"true", AI_KW_TRUE},
      {"false", AI_KW_FALSE},   {"async", AI_KW_ASYNC}, {"await", AI_KW_AWAIT},
      {"defer", AI_KW_DEFER},   {"yield", AI_KW_YIELD}, {"embed", AI_KW_EMBED},

      // Type keywords / builtins.
      {"Ref", AI_KW_REF},       {"any", AI_KW_ANY},
      {"void", AI_KW_VOID},     {"int", AI_KW_INT},     {"long", AI_KW_LONG},
      {"double", AI_KW_DOUBLE}, {"float", AI_KW_FLOAT}, {"string", AI_KW_STRING},
      {"bool", AI_KW_BOOL},     {"boolean", AI_KW_BOOL}, {"byte", AI_KW_BYTE},
      {"u8", AI_KW_U8},         {"i8", AI_KW_I8},       {"u16", AI_KW_U16},
      {"i16", AI_KW_I16},       {"u32", AI_KW_U32},     {"u64", AI_KW_U64},
      {"usize", AI_KW_USIZE},   {"isize", AI_KW_ISIZE}, {"f8", AI_KW_F8},
      {"f16", AI_KW_F16},       {"f32", AI_KW_F32},     {"f64", AI_KW_F64},
      {"bf8", AI_KW_BF8},       {"bf16", AI_KW_BF16},
  };

  for (size_t i = 0; i < (sizeof(table) / sizeof(table[0])); i++) {
    const char* w = table[i].word;
    size_t wn = strlen(w);
    if (wn == n && memcmp(s, w, n) == 0) {
      return table[i].kind;
    }
  }
  return AI_TOK_IDENTIFIER;
}

static void skip_ws_and_comments(AiLexer* lex) {
  for (;;) {
    char c = peek(lex);
    if (c == ' ' || c == '\t' || c == '\f' || c == '\v' || c == '\r') {
      bump(lex);
      continue;
    }

    if (c == '/' && peek_n(lex, 1) == '/') {
      while (!at_eof(lex) && peek(lex) != '\n') {
        bump(lex);
      }
      continue;
    }

    if (c == '/' && peek_n(lex, 1) == '*') {
      bump(lex);
      bump(lex);
      while (!at_eof(lex)) {
        if (peek(lex) == '*' && peek_n(lex, 1) == '/') {
          bump(lex);
          bump(lex);
          break;
        }
        bump(lex);
      }
      continue;
    }

    break;
  }
}

static void emit_simple(AiLexer* lex, AiTokens* out, AiTokenKind kind, size_t start_off,
                        int start_line, int start_col, const char* start_text) {
  AiToken tok;
  tok.kind = kind;
  tok.span = span_make(start_off, lex->i, start_line, start_col);
  tok.text = start_text;
  tok.text_len = tok.span.length;
  tokens_push(out, tok);
}

static void emit_ident_or_kw(AiLexer* lex, AiTokens* out, size_t start_off, int start_line,
                             int start_col, const char* start_text) {
  while (is_ident_cont(peek(lex))) {
    bump(lex);
  }
  size_t end = lex->i;
  AiToken tok;
  size_t n = end - start_off;
  tok.kind = (n == 1 && start_text[0] == '_') ? AI_TOK_UNDERSCORE : keyword_kind(start_text, n);
  tok.span = span_make(start_off, end, start_line, start_col);
  tok.text = start_text;
  tok.text_len = tok.span.length;
  tokens_push(out, tok);
}

static void emit_number(AiLexer* lex, AiTokens* out, size_t start_off, int start_line, int start_col,
                        const char* start_text) {
  bool is_float = false;
  while (isdigit((unsigned char)peek(lex))) {
    bump(lex);
  }

  if (peek(lex) == '.' && isdigit((unsigned char)peek_n(lex, 1))) {
    is_float = true;
    bump(lex);
    while (isdigit((unsigned char)peek(lex))) {
      bump(lex);
    }
  }

  if (peek(lex) == 'e' || peek(lex) == 'E') {
    is_float = true;
    bump(lex);
    if (peek(lex) == '+' || peek(lex) == '-') {
      bump(lex);
    }
    while (isdigit((unsigned char)peek(lex))) {
      bump(lex);
    }
  }

  AiTokenKind kind = AI_TOK_INT_LIT;
  if (!is_float && (peek(lex) == 'l' || peek(lex) == 'L')) {
    bump(lex);
    kind = AI_TOK_LONG_LIT;
  } else if (is_float) {
    kind = AI_TOK_FLOAT_LIT;
  }

  emit_simple(lex, out, kind, start_off, start_line, start_col, start_text);
}

static void emit_string(AiLexer* lex, AiTokens* out, size_t start_off, int start_line, int start_col,
                        const char* start_text) {
  // Consume opening quote.
  bump(lex);
  while (!at_eof(lex)) {
    char c = peek(lex);
    if (c == '"') {
      bump(lex);
      break;
    }
    if (c == '\\') {
      bump(lex);
      if (!at_eof(lex)) {
        bump(lex);
      }
      continue;
    }
    if (c == '\n') {
      AiSpan sp = span_make(start_off, lex->i, start_line, start_col);
      ai_diag_error(lex->diag, sp, "unterminated string literal");
      break;
    }
    bump(lex);
  }
  emit_simple(lex, out, AI_TOK_STRING_LIT, start_off, start_line, start_col, start_text);
}

bool ai_lex(AiLexer* lex, AiTokens* out_tokens) {
  out_tokens->data = NULL;
  out_tokens->len = 0;
  out_tokens->cap = 0;

  while (!at_eof(lex)) {
    skip_ws_and_comments(lex);
    if (at_eof(lex)) {
      break;
    }

    size_t start_off = lex->i;
    int start_line = lex->line;
    int start_col = lex->column;
    const char* start_text = lex->src + start_off;

    char c = peek(lex);

    // Newlines and explicit semicolons are SEMI.
    if (c == '\n' || c == ';') {
      if (c == '\n') {
        while (peek(lex) == '\n') {
          bump(lex);
        }
      } else {
        bump(lex);
      }
      emit_simple(lex, out_tokens, AI_TOK_SEMI, start_off, start_line, start_col, start_text);
      continue;
    }

    if (is_ident_start(c)) {
      bump(lex);
      emit_ident_or_kw(lex, out_tokens, start_off, start_line, start_col, start_text);
      continue;
    }

    if (isdigit((unsigned char)c)) {
      bump(lex);
      emit_number(lex, out_tokens, start_off, start_line, start_col, start_text);
      continue;
    }

    if (c == '"') {
      emit_string(lex, out_tokens, start_off, start_line, start_col, start_text);
      continue;
    }

    // Longest-match operators.
    if (match(lex, "...")) {
      emit_simple(lex, out_tokens, AI_TOK_ELLIPSIS, start_off, start_line, start_col, start_text);
      continue;
    }
    if (match(lex, "::")) {
      emit_simple(lex, out_tokens, AI_TOK_DBLCOLON, start_off, start_line, start_col, start_text);
      continue;
    }
    if (match(lex, "??")) {
      emit_simple(lex, out_tokens, AI_TOK_COALESCE, start_off, start_line, start_col, start_text);
      continue;
    }
    if (match(lex, "->")) {
      emit_simple(lex, out_tokens, AI_TOK_ARROW, start_off, start_line, start_col, start_text);
      continue;
    }
    if (match(lex, "=>")) {
      emit_simple(lex, out_tokens, AI_TOK_FATARROW, start_off, start_line, start_col, start_text);
      continue;
    }
    if (match(lex, "<=")) {
      emit_simple(lex, out_tokens, AI_TOK_LE, start_off, start_line, start_col, start_text);
      continue;
    }
    if (match(lex, ">=")) {
      emit_simple(lex, out_tokens, AI_TOK_GE, start_off, start_line, start_col, start_text);
      continue;
    }
    if (match(lex, "==")) {
      emit_simple(lex, out_tokens, AI_TOK_EQ, start_off, start_line, start_col, start_text);
      continue;
    }
    if (match(lex, "!=")) {
      emit_simple(lex, out_tokens, AI_TOK_NEQ, start_off, start_line, start_col, start_text);
      continue;
    }
    if (match(lex, "&&")) {
      emit_simple(lex, out_tokens, AI_TOK_AND, start_off, start_line, start_col, start_text);
      continue;
    }
    if (match(lex, "||")) {
      emit_simple(lex, out_tokens, AI_TOK_OR, start_off, start_line, start_col, start_text);
      continue;
    }
    if (match(lex, "++")) {
      emit_simple(lex, out_tokens, AI_TOK_INC, start_off, start_line, start_col, start_text);
      continue;
    }
    if (match(lex, "--")) {
      emit_simple(lex, out_tokens, AI_TOK_DEC, start_off, start_line, start_col, start_text);
      continue;
    }

    // Single-char tokens.
    bump(lex);
    AiTokenKind kind = AI_TOK_EOF;
    switch (c) {
      case '@': kind = AI_TOK_AT; break;
      case '=': kind = AI_TOK_ASSIGN; break;
      case '+': kind = AI_TOK_PLUS; break;
      case '-': kind = AI_TOK_MINUS; break;
      case '*': kind = AI_TOK_STAR; break;
      case '/': kind = AI_TOK_SLASH; break;
      case '<': kind = AI_TOK_LT; break;
      case '>': kind = AI_TOK_GT; break;
      case '!': kind = AI_TOK_NOT; break;
      case '&': kind = AI_TOK_AMP; break;
      case '|': kind = AI_TOK_BOR; break;
      case '^': kind = AI_TOK_BXOR; break;
      case '~': kind = AI_TOK_BNOT; break;
      case '?': kind = AI_TOK_QUESTION; break;
      case '.': kind = AI_TOK_DOT; break;
      case ':': kind = AI_TOK_COLON; break;
      case ',': kind = AI_TOK_COMMA; break;
      case '_': kind = AI_TOK_UNDERSCORE; break;
      case '(': kind = AI_TOK_LPAREN; break;
      case ')': kind = AI_TOK_RPAREN; break;
      case '{': kind = AI_TOK_LBRACE; break;
      case '}': kind = AI_TOK_RBRACE; break;
      case '[': kind = AI_TOK_LBRACK; break;
      case ']': kind = AI_TOK_RBRACK; break;
      default: kind = AI_TOK_EOF; break;
    }

    if (kind == AI_TOK_EOF) {
      AiSpan sp = span_make(start_off, lex->i, start_line, start_col);
      ai_diag_error(lex->diag, sp, "unexpected character '%c'", c);
      continue;
    }
    emit_simple(lex, out_tokens, kind, start_off, start_line, start_col, start_text);
  }

  AiToken eof;
  eof.kind = AI_TOK_EOF;
  eof.span.offset = lex->len;
  eof.span.length = 0;
  eof.span.line = lex->line;
  eof.span.column = lex->column;
  eof.text = lex->src + lex->len;
  eof.text_len = 0;
  tokens_push(out_tokens, eof);

  return lex->diag->error_count == 0;
}
