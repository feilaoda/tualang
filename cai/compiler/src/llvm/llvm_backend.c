#include "cai/llvm_backend.h"

#include <stdio.h>
#include <stdlib.h>

#include <llvm-c/Core.h>

static LLVMTypeRef llvm_type(AiTypeKind k) {
  switch (k) {
    case AI_TYPE_VOID: return LLVMVoidType();
    case AI_TYPE_BOOL: return LLVMInt1Type();
    case AI_TYPE_I8: return LLVMInt8Type();
    case AI_TYPE_I16: return LLVMInt16Type();
    case AI_TYPE_I32: return LLVMInt32Type();
    case AI_TYPE_I64: return LLVMInt64Type();
    case AI_TYPE_U8: return LLVMInt8Type();
    case AI_TYPE_U16: return LLVMInt16Type();
    case AI_TYPE_U32: return LLVMInt32Type();
    case AI_TYPE_U64: return LLVMInt64Type();
    case AI_TYPE_F32: return LLVMFloatType();
    case AI_TYPE_F64: return LLVMDoubleType();
    default: return NULL;
  }
}

static void write_str(FILE* f, const char* s) { fputs(s, f); }

int ai_emit_llvm_ir(const AiModule* m, const char* out_path) {
  LLVMModuleRef mod = LLVMModuleCreateWithName("cai");

  for (size_t i = 0; i < m->funcs_len; i++) {
    const AiFuncSig* fn = &m->funcs[i];
    LLVMTypeRef ret = llvm_type(fn->ret);
    if (!ret) {
      LLVMDisposeModule(mod);
      fprintf(stderr, "emit-llvm: unsupported return type\n");
      return 1;
    }

    LLVMTypeRef* params = NULL;
    if (fn->params_len > 0) {
      params = (LLVMTypeRef*)calloc(fn->params_len, sizeof(LLVMTypeRef));
      if (!params) {
        LLVMDisposeModule(mod);
        fprintf(stderr, "emit-llvm: out of memory\n");
        return 1;
      }
      for (size_t j = 0; j < fn->params_len; j++) {
        LLVMTypeRef pt = llvm_type(fn->params[j].type);
        if (!pt) {
          free(params);
          LLVMDisposeModule(mod);
          fprintf(stderr, "emit-llvm: unsupported param type\n");
          return 1;
        }
        params[j] = pt;
      }
    }

    LLVMTypeRef fnty = LLVMFunctionType(ret, params, (unsigned)fn->params_len, 0);
    free(params);

    char* name = (char*)calloc(fn->name.len + 1, 1);
    if (!name) {
      LLVMDisposeModule(mod);
      fprintf(stderr, "emit-llvm: out of memory\n");
      return 1;
    }
    for (size_t k = 0; k < fn->name.len; k++) {
      name[k] = fn->name.text[k];
    }
    name[fn->name.len] = '\0';

    LLVMAddFunction(mod, name, fnty);
    free(name);
  }

  char* ir = LLVMPrintModuleToString(mod);
  LLVMDisposeModule(mod);
  if (!ir) {
    fprintf(stderr, "emit-llvm: failed to print module\n");
    return 1;
  }

  FILE* out = stdout;
  if (out_path) {
    out = fopen(out_path, "wb");
    if (!out) {
      LLVMDisposeMessage(ir);
      fprintf(stderr, "emit-llvm: failed to open output: %s\n", out_path);
      return 1;
    }
  }

  write_str(out, ir);

  if (out_path) {
    fclose(out);
  }
  LLVMDisposeMessage(ir);
  return 0;
}
