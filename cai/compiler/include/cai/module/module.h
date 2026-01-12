#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "cai/diag/diag.h"

/*
 * Module-level structures used by early compiler stages.
 *
 * Today this is mainly used as a lightweight container for function signatures
 * (`emit-llvm` prototype). It is NOT a full semantic module system yet.
 */

typedef enum AiTypeKind {
  AI_TYPE_VOID = 0,
  AI_TYPE_BOOL,
  AI_TYPE_I8,
  AI_TYPE_I16,
  AI_TYPE_I32,
  AI_TYPE_I64,
  AI_TYPE_U8,
  AI_TYPE_U16,
  AI_TYPE_U32,
  AI_TYPE_U64,
  AI_TYPE_F32,
  AI_TYPE_F64,
} AiTypeKind;

typedef enum AiParamMode {
  AI_PARAM_CONST = 0,
  AI_PARAM_MUT,
  AI_PARAM_MOVE,
} AiParamMode;

typedef struct AiIdent {
  const char* text;
  size_t len;
  AiSpan span;
} AiIdent;

typedef struct AiParamSig {
  AiParamMode mode;
  AiIdent name;
  AiTypeKind type;
} AiParamSig;

typedef struct AiFuncSig {
  AiIdent name;
  AiTypeKind ret;
  AiParamSig* params;
  size_t params_len;
  size_t params_cap;
} AiFuncSig;

typedef struct AiModule {
  AiFuncSig* funcs;
  size_t funcs_len;
  size_t funcs_cap;
} AiModule;

void ai_module_init(AiModule* m);
void ai_module_free(AiModule* m);

AiFuncSig* ai_module_add_func(AiModule* m, AiIdent name);
bool ai_func_add_param(AiFuncSig* f, AiParamSig p);

