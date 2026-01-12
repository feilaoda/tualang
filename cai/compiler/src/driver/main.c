#include "cai/diag.h"
#include "cai/lexer.h"
#include "cai/llvm_backend.h"
#include "cai/module.h"
#include "cai/parser.h"
#include "cai/util.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/*
 * `caic` CLI driver.
 *
 * Current commands:
 * - `check <file.ai>`: lex + parse, report syntax/early-stage errors
 * - `emit-llvm <file.ai> [-o out.ll]`: prototype: collect function signatures and emit LLVM IR
 */

static const char* kind_str(AiTokenKind k) {
  switch (k) {
    case AI_TOK_EOF: return "EOF";
    case AI_TOK_SEMI: return "SEMI";
    case AI_TOK_IDENTIFIER: return "IDENT";
    case AI_TOK_INT_LIT: return "INT_LIT";
    case AI_TOK_LONG_LIT: return "LONG_LIT";
    case AI_TOK_FLOAT_LIT: return "FLOAT_LIT";
    case AI_TOK_STRING_LIT: return "STRING_LIT";
    case AI_TOK_FATARROW: return "=>";
    case AI_TOK_LBRACE: return "{";
    case AI_TOK_RBRACE: return "}";
    case AI_TOK_UNDERSCORE: return "_";
    case AI_KW_MATCH: return "match";
    case AI_KW_RETURN: return "return";
    default: return "tok";
  }
}

static void usage(void) {
  fprintf(stderr, "usage:\n");
  fprintf(stderr, "  caic check <file.ai>\n");
  fprintf(stderr, "  caic emit-llvm <file.ai> [-o out.ll]\n");
}

int main(int argc, char** argv) {
  const char* cmd = (argc >= 2) ? argv[1] : NULL;
  if (!cmd) {
    usage();
    return 2;
  }

  bool want_check = strcmp(cmd, "check") == 0;
  bool want_emit_llvm = strcmp(cmd, "emit-llvm") == 0;
  if (!want_check && !want_emit_llvm) {
    usage();
    return 2;
  }

  const char* path = (argc >= 3) ? argv[2] : NULL;
  if (!path) {
    usage();
    return 2;
  }

  const char* out_path = NULL;
  if (want_emit_llvm) {
    if (argc == 5 && strcmp(argv[3], "-o") == 0) {
      out_path = argv[4];
    } else if (argc != 3) {
      usage();
      return 2;
    }
  } else {
    if (argc != 3) {
      usage();
      return 2;
    }
  }
  AiBuf src = {0};
  if (!ai_read_file(path, &src)) {
    fprintf(stderr, "error: failed to read file: %s\n", path);
    return 1;
  }

  AiDiag diag;
  ai_diag_init(&diag, path);

  AiLexer lex = {0};
  lex.filename = path;
  lex.src = src.data;
  lex.len = src.len;
  lex.i = 0;
  lex.line = 1;
  lex.column = 1;
  lex.diag = &diag;

  AiTokens tokens = {0};
  (void)ai_lex(&lex, &tokens);

  if (getenv("CAIC_DUMP_TOKENS") != NULL) {
    for (size_t i = 0; i < tokens.len; i++) {
      const AiToken* t = &tokens.data[i];
      fprintf(stderr, "%d:%d  %-8s  %.*s\n", t->span.line, t->span.column, kind_str(t->kind),
              (int)t->text_len, t->text);
    }
  }

  AiParser parser = {0};
  parser.filename = path;
  parser.tokens = &tokens;
  parser.i = 0;
  parser.diag = &diag;
  parser.stop_before_lbrace_suffix = false;
  parser.block_depth = 0;
  parser.async_depth = 0;

  AiModule module;
  ai_module_init(&module);
  parser.module = want_emit_llvm ? &module : NULL;

  (void)ai_parse_compilation_unit(&parser);

  if (want_emit_llvm && diag.error_count == 0) {
    if (ai_emit_llvm_ir(&module, out_path) != 0) {
      diag.error_count += 1;
    }
  }

  ai_module_free(&module);

  ai_tokens_free(&tokens);
  ai_buf_free(&src);

  return diag.error_count == 0 ? 0 : 1;
}
