
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <stdio.h>
#include <stdarg.h>

#include "compiler.h"
#include "parser.h"
#include "error.h"
#include "list.h"
#include "debug.h"
#include "llvm/llvm.h"

#define compilerDebug(...) debug(__VA_ARGS__)

static LLVMTypeRef typeToLLVMType(Compiler* compiler, Type* type, bool defaultToVoid);

typedef struct TailrecState {
    int enabled;
    LLVMValueRef func;
    LLVMBasicBlockRef loop;
    int paramCount;
    LLVMValueRef* paramSlots;      // alloca slots (or box pointer slots if boxed)
    LLVMTypeRef* paramTypes;       // value types (T)
    int* paramIsBoxed;             // 1 if slot stores T*
    LLVMTypeRef* paramBoxPtrTypes; // T* when boxed, else NULL
} TailrecState;

static int tokenEqualsCString(const Token* token, const char* s) {
    if (!token || !s) return 0;
    size_t len = strlen(s);
    return token->length == (int)len && memcmp(token->start, s, len) == 0;
}

void initCompiler(Compiler* compiler) {
    compiler->structs = listNew();
    compiler->enums = listNew();
    compiler->loopStack = listNew();

    compiler->currentFilePath = NULL;

    compiler->currentModulePrefix = NULL;
    compiler->currentModulePrefixLen = 0;
    compiler->currentAliases = NULL;
    compiler->currentObjectPrefix = NULL;
    compiler->currentObjectPrefixLen = 0;
    compiler->currentObjectMethodName = NULL;
    compiler->currentObjectMethodNameLen = 0;

    compiler->multiReturns = listNew();
    compiler->wantMultiValue = 0;

    compiler->boxAllLocals = 0;
    compiler->lambdaCount = 0;
    compiler->closureType = NULL;
    compiler->mapType = NULL;
    compiler->arrayType = NULL;
    compiler->tuaValueType = NULL;
    compiler->closureSigs = listNew();
    compiler->closureReturnSigs = listNew();
    compiler->lastLambdaFuncType = NULL;
    compiler->lastSetFilePath = NULL;
    compiler->lastSetLine = 0;
    compiler->lastSetCol = 0;
    compiler->expectedMapKeyType = NULL;
    compiler->expectedMapValueType = NULL;
    compiler->expectedMapKeyKind = TYPE_ANY;
    compiler->expectedMapValueKind = TYPE_ANY;
    compiler->expectedArrayElemType = NULL;
    compiler->expectedArrayElemKind = TYPE_ANY;
    compiler->expectedArrayFixedLen = -1;
    compiler->tailrec = NULL;
    compiler->llvmOptLevel = 0;
    compiler->outputPath = NULL;
    compiler->linkSearchPaths = listNew();
    compiler->linkLibs = listNew();
    compiler->linkArgs = listNew();
    compiler->dlopenPaths = listNew();
    compiler->externDecls = NULL;
    compiler->runArgc = 0;
    compiler->runArgv = NULL;
    compiler->uncheckedIndex = 0;
    compiler->stackFixedArrays = 0;
    compiler->emitLoc = 1;
    
    // Debug information
    compiler->hadError = false;
    compiler->panicMode = false;
}

void compilerErrorAt(Compiler* compiler, int line, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    if (compiler) compiler->hadError = true;

    const char* file = compiler ? compiler->currentFilePath : NULL;
    if (file && line > 0) {
        fprintf(stderr, "%s:%d: error: ", file, line);
    } else if (line > 0) {
        fprintf(stderr, "error:%d: ", line);
    } else {
        fprintf(stderr, "error: ");
    }
    vfprintf(stderr, fmt, args);
    va_end(args);
    if (fmt) {
        size_t n = strlen(fmt);
        if (n == 0 || fmt[n - 1] != '\n') fprintf(stderr, "\n");
    } else {
        fprintf(stderr, "\n");
    }
}

void compilerErrorAtEx(Compiler* compiler, const char* file, int line, int col, const char* fmt, ...) {
    if (compiler) compiler->hadError = true;
    if (file && line > 0 && col > 0) {
        fprintf(stderr, "%s:%d:%d: error: ", file, line, col);
    } else if (file && line > 0) {
        fprintf(stderr, "%s:%d: error: ", file, line);
    } else if (line > 0 && col > 0) {
        fprintf(stderr, "error:%d:%d: ", line, col);
    } else if (line > 0) {
        fprintf(stderr, "error:%d: ", line);
    } else {
        fprintf(stderr, "error: ");
    }
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    if (fmt) {
        size_t n = strlen(fmt);
        if (n == 0 || fmt[n - 1] != '\n') fprintf(stderr, "\n");
    } else {
        fprintf(stderr, "\n");
    }
}

void compilerErrorAtToken(Compiler* compiler, const Token* token, const char* fmt, ...) {
    const char* file = compiler ? compiler->currentFilePath : NULL;
    int line = token ? token->line : 0;
    int col = token ? token->col : 0;
    if (compiler) compiler->hadError = true;
    if (file && line > 0 && col > 0) {
        fprintf(stderr, "%s:%d:%d: error: ", file, line, col);
    } else if (file && line > 0) {
        fprintf(stderr, "%s:%d: error: ", file, line);
    } else if (line > 0 && col > 0) {
        fprintf(stderr, "error:%d:%d: ", line, col);
    } else if (line > 0) {
        fprintf(stderr, "error:%d: ", line);
    } else {
        fprintf(stderr, "error: ");
    }
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    if (fmt) {
        size_t n = strlen(fmt);
        if (n == 0 || fmt[n - 1] != '\n') fprintf(stderr, "\n");
    } else {
        fprintf(stderr, "\n");
    }
}

StructInfo* compilerFindStruct(Compiler* compiler, const char* name, int length) {
    if (!compiler || !compiler->structs) return NULL;
    for (int i = 0; i < compiler->structs->length; i++) {
        StructInfo* info = listGet(compiler->structs, i);
        if (!info) continue;
        if (info->nameLength != length) continue;
        if (memcmp(info->name, name, (size_t)length) == 0) return info;
    }
    return NULL;
}

EnumInfo* compilerFindEnum(Compiler* compiler, const char* name, int length) {
    if (!compiler || !compiler->enums) return NULL;
    for (int i = 0; i < compiler->enums->length; i++) {
        EnumInfo* info = listGet(compiler->enums, i);
        if (!info) continue;
        if (info->nameLength != length) continue;
        if (memcmp(info->name, name, (size_t)length) == 0) return info;
    }
    return NULL;
}

SymbolAlias* compilerFindAlias(Compiler* compiler, const char* local, int localLen) {
    if (!compiler || !compiler->currentAliases) return NULL;
    for (int i = 0; i < compiler->currentAliases->length; i++) {
        SymbolAlias* a = listGet(compiler->currentAliases, i);
        if (!a) continue;
        if (a->localLen != localLen) continue;
        if (memcmp(a->local, local, (size_t)localLen) == 0) return a;
    }
    return NULL;
}

char* compilerQualifyToken(Compiler* compiler, const Token* name, int* outLen) {
    if (!compiler || !compiler->currentModulePrefix || compiler->currentModulePrefixLen == 0) return NULL;
    const int sepLen = 2;
    int len = compiler->currentModulePrefixLen + sepLen + name->length;
    char* s = malloc((size_t)len + 1);
    memcpy(s, compiler->currentModulePrefix, (size_t)compiler->currentModulePrefixLen);
    memcpy(s + compiler->currentModulePrefixLen, "__", (size_t)sepLen);
    memcpy(s + compiler->currentModulePrefixLen + sepLen, name->start, (size_t)name->length);
    s[len] = '\0';
    if (outLen) *outLen = len;
    return s;
}

int compilerMultiReturnCount(Compiler* compiler, const char* name, int nameLen) {
    if (!compiler || !compiler->multiReturns || !name) return 0;
    for (int i = 0; i < compiler->multiReturns->length; i++) {
        MultiReturnInfo* info = listGet(compiler->multiReturns, i);
        if (!info) continue;
        if (info->nameLen != nameLen) continue;
        if (memcmp(info->name, name, (size_t)nameLen) == 0) return info->count;
    }
    return 0;
}

void compilerRegisterMultiReturn(Compiler* compiler, const char* name, int nameLen, int count) {
    if (!compiler || !compiler->multiReturns || !name) return;
    if (count <= 1) return;
    for (int i = 0; i < compiler->multiReturns->length; i++) {
        MultiReturnInfo* info = listGet(compiler->multiReturns, i);
        if (!info) continue;
        if (info->nameLen != nameLen) continue;
        if (memcmp(info->name, name, (size_t)nameLen) == 0) {
            info->count = count;
            return;
        }
    }
    MultiReturnInfo* info = malloc(sizeof(MultiReturnInfo));
    info->name = malloc((size_t)nameLen + 1);
    memcpy(info->name, name, (size_t)nameLen);
    info->name[nameLen] = '\0';
    info->nameLen = nameLen;
    info->count = count;
    listAppend(compiler->multiReturns, info);
}

LLVMTypeRef compilerGetClosureType(Compiler* compiler) {
    if (!compiler) return NULL;
    if (compiler->closureType) return compiler->closureType;
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef fields[2] = { i8ptr, i8ptr };
    compiler->closureType = LLVMStructTypeInContext(compiler->context, fields, 2, 0);
    return compiler->closureType;
}

LLVMTypeRef compilerGetMapType(Compiler* compiler) {
    if (!compiler) return NULL;
    if (compiler->mapType) return compiler->mapType;
    LLVMTypeRef t = LLVMGetTypeByName2(compiler->context, "tua_map");
    if (!t) t = LLVMStructCreateNamed(compiler->context, "tua_map");
    compiler->mapType = LLVMPointerType(t, 0);
    return compiler->mapType;
}

LLVMTypeRef compilerGetArrayType(Compiler* compiler) {
    if (!compiler) return NULL;
    if (compiler->arrayType) return compiler->arrayType;
    LLVMTypeRef t = LLVMGetTypeByName2(compiler->context, "tua_array");
    if (!t) t = LLVMStructCreateNamed(compiler->context, "tua_array");
    // Layout must match `struct tua_array` in runtime.
    if (LLVMIsOpaqueStruct(t)) {
        LLVMTypeRef i64 = LLVMInt64TypeInContext(compiler->context);
        LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
        LLVMTypeRef fields[5] = { i64, i64, i8ptr, i64, i64 };
        LLVMStructSetBody(t, fields, 5, 0);
    }
    compiler->arrayType = LLVMPointerType(t, 0);
    return compiler->arrayType;
}

static LLVMValueRef getOrCreateTuaMapFree(Compiler* compiler) {
    if (!compiler) return NULL;
    LLVMValueRef fn = LLVMGetNamedFunction(compiler->module, "tua_map_free");
    if (fn) return fn;
    LLVMTypeRef mapType = compilerGetMapType(compiler);
    LLVMTypeRef params[1] = { mapType };
    LLVMTypeRef fty = LLVMFunctionType(LLVMVoidTypeInContext(compiler->context), params, 1, 0);
    return LLVMAddFunction(compiler->module, "tua_map_free", fty);
}

static LLVMValueRef getOrCreateTuaArrayFree(Compiler* compiler) {
    if (!compiler) return NULL;
    LLVMValueRef fn = LLVMGetNamedFunction(compiler->module, "tua_array_free");
    if (fn) return fn;
    LLVMTypeRef arrType = compilerGetArrayType(compiler);
    LLVMTypeRef params[1] = { arrType };
    LLVMTypeRef fty = LLVMFunctionType(LLVMVoidTypeInContext(compiler->context), params, 1, 0);
    return LLVMAddFunction(compiler->module, "tua_array_free", fty);
}

static LLVMValueRef loadLocalVarValueForDrop(Compiler* compiler, VariableRef var, const char* name) {
    if (!compiler || !var.value || !var.type) return NULL;
    LLVMBuilderRef builder = compiler->builder;
    if (var.isBoxed) {
        if (!var.boxPtrType) return NULL;
        LLVMValueRef cell = LLVMBuildLoad2(builder, var.boxPtrType, var.value, "drop_cell");
        return LLVMBuildLoad2(builder, var.type, cell, name ? name : "drop_val");
    }
    return LLVMBuildLoad2(builder, var.type, var.value, name ? name : "drop_val");
}

static void storeLocalVarValueForDrop(Compiler* compiler, VariableRef var, LLVMValueRef value) {
    if (!compiler || !var.value || !var.type || !value) return;
    LLVMBuilderRef builder = compiler->builder;
    if (var.isBoxed) {
        if (!var.boxPtrType) return;
        LLVMValueRef cell = LLVMBuildLoad2(builder, var.boxPtrType, var.value, "drop_cell2");
        LLVMBuildStore(builder, value, cell);
    } else {
        LLVMBuildStore(builder, value, var.value);
    }
}

static void emitDropForVar(Compiler* compiler, VariableRef var) {
    if (!compiler) return;
    if (var.isArray && var.isStackArray) return; // stack-backed fixed arrays must not be freed
    if (!var.isMap && !var.isArray) return;
    if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(compiler->builder))) return;

    LLVMValueRef cur = loadLocalVarValueForDrop(compiler, var, "drop_cur");
    if (!cur) return;

    LLVMValueRef fn = var.isArray ? getOrCreateTuaArrayFree(compiler) : getOrCreateTuaMapFree(compiler);
    if (!fn) return;
    LLVMTypeRef fty = LLVMGlobalGetValueType(fn);
    LLVMValueRef args1[1] = { cur };
    LLVMBuildCall2(compiler->builder, fty, fn, args1, 1, "");

    LLVMValueRef nullv = LLVMConstNull(var.type);
    storeLocalVarValueForDrop(compiler, var, nullv);
}

static void emitDropForBlockVars(Compiler* compiler, Block* block) {
    if (!compiler || !block || !block->variables) return;
    for (ListNode* n = block->variables->head; n != NULL; n = n->next) {
        VariableRef* vr = (VariableRef*)n->data;
        if (!vr) continue;
        emitDropForVar(compiler, *vr);
    }
}

static void emitDropForCurrentFunctionScopes(Compiler* compiler) {
    if (!compiler || !compiler->current || !compiler->current->func) return;
    LLVMValueRef fn = compiler->current->func;
    for (Block* b = compiler->current; b != NULL && b->func == fn; b = b->parent) {
        emitDropForBlockVars(compiler, b);
    }
}

static void moveOutOnReturnIfNeeded(Compiler* compiler, Expr* e) {
    if (!compiler || !e) return;
    if (e->type != EXPR_VARIABLE) return;
    VariableRef v = findVariableExpr(compiler, e);
    if (!v.value || !v.type) return;
    if (v.isArray && v.isStackArray) return;
    if (!v.isMap && !v.isArray) return;
    LLVMValueRef nullv = LLVMConstNull(v.type);
    storeLocalVarValueForDrop(compiler, v, nullv);
}

LLVMTypeRef compilerGetTuaValueType(Compiler* compiler) {
    if (!compiler) return NULL;
    if (compiler->tuaValueType) return compiler->tuaValueType;
    LLVMTypeRef fields[2] = {
        LLVMInt32TypeInContext(compiler->context),
        LLVMInt64TypeInContext(compiler->context),
    };
    compiler->tuaValueType = LLVMStructTypeInContext(compiler->context, fields, 2, 0);
    return compiler->tuaValueType;
}

LLVMTypeRef compilerGetOptionType(Compiler* compiler, LLVMTypeRef inner) {
    if (!compiler || !inner) return NULL;
    LLVMTypeRef fields[2] = {
        LLVMInt1TypeInContext(compiler->context),
        inner,
    };
    return LLVMStructTypeInContext(compiler->context, fields, 2, 0);
}

void compilerRegisterClosureSig(Compiler* compiler, const char* name, int nameLen, LLVMTypeRef funcType) {
    if (!compiler || !compiler->closureSigs || !name || nameLen <= 0 || !funcType) return;
    for (int i = 0; i < compiler->closureSigs->length; i++) {
        ClosureSig* s = listGet(compiler->closureSigs, i);
        if (!s) continue;
        if (s->nameLen != nameLen) continue;
        if (memcmp(s->name, name, (size_t)nameLen) == 0) {
            s->funcType = funcType;
            return;
        }
    }
    ClosureSig* s = malloc(sizeof(ClosureSig));
    s->name = malloc((size_t)nameLen + 1);
    memcpy(s->name, name, (size_t)nameLen);
    s->name[nameLen] = '\0';
    s->nameLen = nameLen;
    s->funcType = funcType;
    listAppend(compiler->closureSigs, s);
}

LLVMTypeRef compilerFindClosureSig(Compiler* compiler, const char* name, int nameLen) {
    if (!compiler || !compiler->closureSigs || !name) return NULL;
    for (int i = 0; i < compiler->closureSigs->length; i++) {
        ClosureSig* s = listGet(compiler->closureSigs, i);
        if (!s) continue;
        if (s->nameLen != nameLen) continue;
        if (memcmp(s->name, name, (size_t)nameLen) == 0) return s->funcType;
    }
    return NULL;
}

void compilerRegisterClosureReturnSig(Compiler* compiler, const char* name, int nameLen, LLVMTypeRef funcType) {
    if (!compiler || !compiler->closureReturnSigs || !name || nameLen <= 0 || !funcType) return;
    for (int i = 0; i < compiler->closureReturnSigs->length; i++) {
        ClosureReturnSig* s = listGet(compiler->closureReturnSigs, i);
        if (!s) continue;
        if (s->nameLen != nameLen) continue;
        if (memcmp(s->name, name, (size_t)nameLen) == 0) {
            if (!s->funcTypes) s->funcTypes = listNew();
            listAppend(s->funcTypes, funcType);
            return;
        }
    }
    ClosureReturnSig* s = malloc(sizeof(ClosureReturnSig));
    s->name = malloc((size_t)nameLen + 1);
    memcpy(s->name, name, (size_t)nameLen);
    s->name[nameLen] = '\0';
    s->nameLen = nameLen;
    s->funcTypes = listNew();
    listAppend(s->funcTypes, funcType);
    listAppend(compiler->closureReturnSigs, s);
}

LLVMTypeRef compilerFindClosureReturnSig(Compiler* compiler, const char* name, int nameLen) {
    return compilerFindClosureReturnSigAt(compiler, name, nameLen, 0);
}

LLVMTypeRef compilerFindClosureReturnSigAt(Compiler* compiler, const char* name, int nameLen, int level) {
    if (!compiler || !compiler->closureReturnSigs || !name) return NULL;
    for (int i = 0; i < compiler->closureReturnSigs->length; i++) {
        ClosureReturnSig* s = listGet(compiler->closureReturnSigs, i);
        if (!s) continue;
        if (s->nameLen != nameLen) continue;
        if (memcmp(s->name, name, (size_t)nameLen) == 0) {
            if (!s->funcTypes) return NULL;
            if (level < 0 || level >= s->funcTypes->length) return NULL;
            return (LLVMTypeRef)listGet(s->funcTypes, level);
        }
    }
    return NULL;
}

LLVMTypeRef compilerClosureSigFromType(Compiler* compiler, Type* type) {
    if (!compiler || !type || type->kind != TYPE_FUNC) return NULL;

    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);

    int argCount = type->paramTypes ? type->paramTypes->length : 0;
    int totalParams = argCount + 1; // env ptr + args...
    LLVMTypeRef* params = malloc(sizeof(LLVMTypeRef) * (size_t)totalParams);
    params[0] = i8ptr;
    for (int i = 0; i < argCount; i++) {
        Type* t = listGet(type->paramTypes, i);
        params[i + 1] = typeToLLVMType(compiler, t, false);
    }

    LLVMTypeRef retType = LLVMVoidTypeInContext(compiler->context);
    int rc = type->returnTypes ? type->returnTypes->length : 0;
    if (rc <= 0) {
        retType = LLVMVoidTypeInContext(compiler->context);
    } else if (rc == 1) {
        Type* rt = listGet(type->returnTypes, 0);
        retType = typeToLLVMType(compiler, rt, true);
    } else {
        LLVMTypeRef* rts = malloc(sizeof(LLVMTypeRef) * (size_t)rc);
        for (int i = 0; i < rc; i++) {
            Type* rt = listGet(type->returnTypes, i);
            rts[i] = typeToLLVMType(compiler, rt, false);
        }
        retType = LLVMStructTypeInContext(compiler->context, rts, (unsigned)rc, 0);
        free(rts);
    }

    LLVMTypeRef fnType = LLVMFunctionType(retType, params, (unsigned)totalParams, 0);
    free(params);
    return fnType;
}

StructInfo* compilerResolveStructByToken(Compiler* compiler, const Token* name) {
    if (!compiler || !name) return NULL;
    SymbolAlias* a = compilerFindAlias(compiler, name->start, name->length);
    if (a && a->kind == ALIAS_STRUCT) {
        return compilerFindStruct(compiler, a->qualified, a->qualifiedLen);
    }
    if (compiler && compiler->currentModulePrefix) {
        int ql = 0;
        char* q = compilerQualifyToken(compiler, name, &ql);
        if (q) {
            StructInfo* info = compilerFindStruct(compiler, q, ql);
            free(q);
            if (info) return info;
        }
    }
    return compilerFindStruct(compiler, name->start, name->length);
}

EnumInfo* compilerResolveEnumByToken(Compiler* compiler, const Token* name) {
    if (!compiler || !name) return NULL;
    SymbolAlias* a = compilerFindAlias(compiler, name->start, name->length);
    if (a && a->kind == ALIAS_ENUM) {
        return compilerFindEnum(compiler, a->qualified, a->qualifiedLen);
    }
    if (compiler && compiler->currentModulePrefix) {
        int ql = 0;
        char* q = compilerQualifyToken(compiler, name, &ql);
        if (q) {
            EnumInfo* info = compilerFindEnum(compiler, q, ql);
            free(q);
            if (info) return info;
        }
    }
    return compilerFindEnum(compiler, name->start, name->length);
}

static LLVMTypeRef typeToLLVMType(Compiler* compiler, Type* type, bool defaultToVoid) {
    if (type == NULL) {
        return defaultToVoid ? LLVMVoidTypeInContext(compiler->context)
                             : LLVMInt32TypeInContext(compiler->context);
    }

    switch (type->kind) {
        case TYPE_I8:
        case TYPE_U8:
        case TYPE_BYTE:
        case TYPE_F8:
        case TYPE_BF8:
            return LLVMInt8TypeInContext(compiler->context);
        case TYPE_I16:
        case TYPE_U16:
            return LLVMInt16TypeInContext(compiler->context);
        case TYPE_INT:
        case TYPE_U32:
            return LLVMInt32TypeInContext(compiler->context);
        case TYPE_LONG:
        case TYPE_U64:
            return LLVMInt64TypeInContext(compiler->context);
        case TYPE_ISIZE:
        case TYPE_USIZE: {
            unsigned bits = (unsigned)(sizeof(void*) * 8);
            return LLVMIntTypeInContext(compiler->context, bits);
        }
        case TYPE_F16:
            return LLVMHalfTypeInContext(compiler->context);
        case TYPE_DOUBLE:
            return LLVMDoubleTypeInContext(compiler->context);
        case TYPE_FLOAT:
            return LLVMFloatTypeInContext(compiler->context);
        case TYPE_BF16:
            return LLVMBFloatTypeInContext(compiler->context);
        case TYPE_BOOL:
            return LLVMInt1TypeInContext(compiler->context);
        case TYPE_STRING:
            return LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
        case TYPE_PTR:
            return LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
        case TYPE_NAMED: {
            if (type->name.length == 3 && memcmp(type->name.start, "ptr", 3) == 0) {
                return LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
            }
            if (type->name.length == 3 && memcmp(type->name.start, "map", 3) == 0) {
                return compilerGetMapType(compiler);
            }
            if (type->name.length == 6 && memcmp(type->name.start, "Option", 6) == 0) {
                Type* inner = NULL;
                if (type->typeArgs && type->typeArgs->length == 1) {
                    inner = (Type*)type->typeArgs->head->data;
                }
                if (!inner) {
                    return compilerGetOptionType(compiler, compilerGetTuaValueType(compiler));
                }
                LLVMTypeRef innerTy = typeToLLVMType(compiler, inner, false);
                return compilerGetOptionType(compiler, innerTy);
            }
            StructInfo* info = compilerResolveStructByToken(compiler, &type->name);
            if (!info) {
                // Best-effort: create/lookup an opaque named struct type.
                char* tn = malloc((size_t)type->name.length + 1);
                memcpy(tn, type->name.start, (size_t)type->name.length);
                tn[type->name.length] = '\0';
                LLVMTypeRef t = LLVMGetTypeByName2(compiler->context, tn);
                if (!t) t = LLVMStructCreateNamed(compiler->context, tn);
                free(tn);
                return t;
            }
            return info->type; // value semantics
        }
        case TYPE_REF: {
            LLVMTypeRef inner = typeToLLVMType(compiler, type->inner, false);
            return LLVMPointerType(inner, 0);
        }
        case TYPE_ARRAY:
            return compilerGetArrayType(compiler);
        case TYPE_FUNC:
            return compilerGetClosureType(compiler);
        case TYPE_VOID:
            return LLVMVoidTypeInContext(compiler->context);
        default:
            return defaultToVoid ? LLVMVoidTypeInContext(compiler->context)
                                 : LLVMInt32TypeInContext(compiler->context);
    }
}

static void compilerRegisterClosureReturnSigChain(Compiler* compiler, const char* name, int nameLen, Type* type) {
    if (!compiler || !name || nameLen <= 0) return;
    Type* t = type;
    while (t && t->kind == TYPE_FUNC) {
        LLVMTypeRef sig = compilerClosureSigFromType(compiler, t);
        if (sig) compilerRegisterClosureReturnSig(compiler, name, nameLen, sig);

        // Advance only for single-return nested function types.
        if (!t->returnTypes || t->returnTypes->length != 1) break;
        Type* next = (Type*)listGet(t->returnTypes, 0);
        if (!next || next->kind != TYPE_FUNC) break;
        t = next;
    }
}

static LLVMValueRef castValueToType(Compiler* compiler, LLVMValueRef value, LLVMTypeRef targetType) {
    if (!value) return NULL;
    LLVMTypeRef srcType = LLVMTypeOf(value);
    if (srcType == targetType) return value;

    LLVMTypeKind srcKind = LLVMGetTypeKind(srcType);
    LLVMTypeKind dstKind = LLVMGetTypeKind(targetType);

    // Option<T> -> Option<U>
    if (srcKind == LLVMStructTypeKind && dstKind == LLVMStructTypeKind &&
        LLVMCountStructElementTypes(srcType) == 2 && LLVMCountStructElementTypes(targetType) == 2) {
        LLVMTypeRef s0 = LLVMStructGetTypeAtIndex(srcType, 0);
        LLVMTypeRef d0 = LLVMStructGetTypeAtIndex(targetType, 0);
        if (LLVMGetTypeKind(s0) == LLVMIntegerTypeKind && LLVMGetIntTypeWidth(s0) == 1 &&
            LLVMGetTypeKind(d0) == LLVMIntegerTypeKind && LLVMGetIntTypeWidth(d0) == 1) {
            LLVMValueRef ok = LLVMBuildExtractValue(compiler->builder, value, 0, "opt_ok");
            LLVMValueRef payload = LLVMBuildExtractValue(compiler->builder, value, 1, "opt_v");
            LLVMTypeRef dstInner = LLVMStructGetTypeAtIndex(targetType, 1);

            LLVMValueRef fn = compiler->current->func;
            LLVMBasicBlockRef someBB = LLVMAppendBasicBlock(fn, "optcast.some");
            LLVMBasicBlockRef noneBB = LLVMAppendBasicBlock(fn, "optcast.none");
            LLVMBasicBlockRef contBB = LLVMAppendBasicBlock(fn, "optcast.cont");
            LLVMBuildCondBr(compiler->builder, ok, someBB, noneBB);

            LLVMPositionBuilderAtEnd(compiler->builder, someBB);
            LLVMValueRef someV = castValueToType(compiler, payload, dstInner);
            LLVMBuildBr(compiler->builder, contBB);
            LLVMBasicBlockRef someEnd = LLVMGetInsertBlock(compiler->builder);

            LLVMPositionBuilderAtEnd(compiler->builder, noneBB);
            LLVMValueRef noneV = LLVMConstNull(dstInner);
            LLVMBuildBr(compiler->builder, contBB);
            LLVMBasicBlockRef noneEnd = LLVMGetInsertBlock(compiler->builder);

            LLVMPositionBuilderAtEnd(compiler->builder, contBB);
            LLVMValueRef phi = LLVMBuildPhi(compiler->builder, dstInner, "optcast_v");
            LLVMAddIncoming(phi, &someV, &someEnd, 1);
            LLVMAddIncoming(phi, &noneV, &noneEnd, 1);

            LLVMValueRef out = LLVMGetUndef(targetType);
            out = LLVMBuildInsertValue(compiler->builder, out, ok, 0, "o0");
            out = LLVMBuildInsertValue(compiler->builder, out, phi, 1, "o1");
            return out;
        }
    }

    LLVMTypeRef vt = compilerGetTuaValueType(compiler);
    if (srcType == vt) {
        LLVMValueRef fn = NULL;
        LLVMTypeRef fnType = NULL;
        if (dstKind == LLVMIntegerTypeKind) {
            unsigned bits = LLVMGetIntTypeWidth(targetType);
            if (bits == 1) {
                fn = LLVMGetNamedFunction(compiler->module, "tua_value_to_bool");
                if (!fn) {
                    LLVMTypeRef params[1] = { vt };
                    fnType = LLVMFunctionType(LLVMInt32TypeInContext(compiler->context), params, 1, 0);
                    fn = LLVMAddFunction(compiler->module, "tua_value_to_bool", fnType);
                }
                fnType = LLVMGlobalGetValueType(fn);
                LLVMValueRef b32 = LLVMBuildCall2(compiler->builder, fnType, fn, &value, 1, "b32");
                return LLVMBuildTrunc(compiler->builder, b32, targetType, "b");
            }
            if (bits <= 32) {
                fn = LLVMGetNamedFunction(compiler->module, "tua_value_to_int");
                if (!fn) {
                    LLVMTypeRef params[1] = { vt };
                    fnType = LLVMFunctionType(LLVMInt32TypeInContext(compiler->context), params, 1, 0);
                    fn = LLVMAddFunction(compiler->module, "tua_value_to_int", fnType);
                }
                fnType = LLVMGlobalGetValueType(fn);
                LLVMValueRef i32 = LLVMBuildCall2(compiler->builder, fnType, fn, &value, 1, "i32");
                if (bits < 32) return LLVMBuildTrunc(compiler->builder, i32, targetType, "itr");
                if (bits > 32) return LLVMBuildSExt(compiler->builder, i32, targetType, "isx");
                return i32;
            }
            if (bits == 64) {
                fn = LLVMGetNamedFunction(compiler->module, "tua_value_to_long");
                if (!fn) {
                    LLVMTypeRef params[1] = { vt };
                    fnType = LLVMFunctionType(LLVMInt64TypeInContext(compiler->context), params, 1, 0);
                    fn = LLVMAddFunction(compiler->module, "tua_value_to_long", fnType);
                }
                fnType = LLVMGlobalGetValueType(fn);
                return LLVMBuildCall2(compiler->builder, fnType, fn, &value, 1, "i64");
            }
        }

        if (dstKind == LLVMDoubleTypeKind) {
            fn = LLVMGetNamedFunction(compiler->module, "tua_value_to_double");
            if (!fn) {
                LLVMTypeRef params[1] = { vt };
                fnType = LLVMFunctionType(LLVMDoubleTypeInContext(compiler->context), params, 1, 0);
                fn = LLVMAddFunction(compiler->module, "tua_value_to_double", fnType);
            }
            fnType = LLVMGlobalGetValueType(fn);
            return LLVMBuildCall2(compiler->builder, fnType, fn, &value, 1, "d");
        }
        if (dstKind == LLVMFloatTypeKind) {
            // Decode as double then truncate to float (runtime stores floats as doubles in tua_value).
            fn = LLVMGetNamedFunction(compiler->module, "tua_value_to_double");
            if (!fn) {
                LLVMTypeRef params[1] = { vt };
                fnType = LLVMFunctionType(LLVMDoubleTypeInContext(compiler->context), params, 1, 0);
                fn = LLVMAddFunction(compiler->module, "tua_value_to_double", fnType);
            }
            fnType = LLVMGlobalGetValueType(fn);
            LLVMValueRef d = LLVMBuildCall2(compiler->builder, fnType, fn, &value, 1, "d");
            return LLVMBuildFPTrunc(compiler->builder, d, targetType, "f");
        }

        if (dstKind == LLVMPointerTypeKind) {
            LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
            if (targetType == i8ptr) {
                fn = LLVMGetNamedFunction(compiler->module, "tua_value_to_string");
                if (!fn) {
                    LLVMTypeRef params[1] = { vt };
                    fnType = LLVMFunctionType(i8ptr, params, 1, 0);
                    fn = LLVMAddFunction(compiler->module, "tua_value_to_string", fnType);
                }
                fnType = LLVMGlobalGetValueType(fn);
                return LLVMBuildCall2(compiler->builder, fnType, fn, &value, 1, "s");
            }
        }
    }

    if (srcKind == LLVMPointerTypeKind && dstKind == LLVMPointerTypeKind) {
        return LLVMBuildBitCast(compiler->builder, value, targetType, "ptrcast");
    }

    if (srcKind == LLVMIntegerTypeKind && dstKind == LLVMIntegerTypeKind) {
        unsigned srcBits = LLVMGetIntTypeWidth(srcType);
        unsigned dstBits = LLVMGetIntTypeWidth(targetType);
        if (srcBits < dstBits) return LLVMBuildSExt(compiler->builder, value, targetType, "sext");
        if (srcBits > dstBits) return LLVMBuildTrunc(compiler->builder, value, targetType, "trunc");
        return value;
    }

    if (srcKind == LLVMIntegerTypeKind && (dstKind == LLVMFloatTypeKind || dstKind == LLVMDoubleTypeKind)) {
        return LLVMBuildSIToFP(compiler->builder, value, targetType, "sitofp");
    }

    if ((srcKind == LLVMFloatTypeKind || srcKind == LLVMDoubleTypeKind) && dstKind == LLVMIntegerTypeKind) {
        return LLVMBuildFPToSI(compiler->builder, value, targetType, "fptosi");
    }

    if (srcKind == LLVMFloatTypeKind && dstKind == LLVMDoubleTypeKind) {
        return LLVMBuildFPExt(compiler->builder, value, targetType, "fpext");
    }
    if (srcKind == LLVMDoubleTypeKind && dstKind == LLVMFloatTypeKind) {
        return LLVMBuildFPTrunc(compiler->builder, value, targetType, "fptrunc");
    }

    return value;
}

static LLVMValueRef getOrCreateTuaSetLoc(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_set_loc");
    if (existing) return existing;
    LLVMTypeRef i32 = LLVMInt32TypeInContext(compiler->context);
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef params[3] = { i8ptr, i32, i32 };
    LLVMTypeRef fnType = LLVMFunctionType(LLVMVoidTypeInContext(compiler->context), params, 3, 0);
    return LLVMAddFunction(compiler->module, "tua_set_loc", fnType);
}

static void emitSetLocIfNeeded(Compiler* compiler, int line, int col) {
    if (!compiler) return;
    if (!compiler->emitLoc) return;
    if (line <= 0) return;
    const char* file = compiler->currentFilePath;
    if (compiler->lastSetFilePath == file && compiler->lastSetLine == line && compiler->lastSetCol == col) return;
    if (!compiler->builder) return;
    LLVMBasicBlockRef bb = LLVMGetInsertBlock(compiler->builder);
    if (!bb) return;
    if (LLVMGetBasicBlockTerminator(bb)) return;

    compiler->lastSetFilePath = file;
    compiler->lastSetLine = line;
    compiler->lastSetCol = col;

    LLVMValueRef fn = getOrCreateTuaSetLoc(compiler);
    LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
    LLVMTypeRef i32 = LLVMInt32TypeInContext(compiler->context);
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMValueRef fileV = file ? LLVMBuildGlobalStringPtr(compiler->builder, file, "tua_file") : LLVMConstNull(i8ptr);
    LLVMValueRef lineV = LLVMConstInt(i32, (unsigned)line, 0);
    LLVMValueRef colV = LLVMConstInt(i32, (unsigned)(col > 0 ? col : 0), 0);
    LLVMValueRef args[3] = { fileV, lineV, colV };
    LLVMBuildCall2(compiler->builder, fnType, fn, args, 3, "");
}

LLVMValueRef compileExpr(Compiler* compiler, Expr* expr) {
    if (expr == NULL) {
        error("compileExpr got NULL\n");
        return NULL;
    }
    emitSetLocIfNeeded(compiler, expr->token.line, expr->token.col);
    compilerDebug("Compiling expression type:%s\n", exprTypeToString(expr->type));
    switch (expr->type) {
        case EXPR_BINARY:
            return emitBinaryExpr(compiler, (BinaryExpr*)expr);
            break;
        case EXPR_UNARY:
            return emitUnaryExpr(compiler, (UnaryExpr*)expr);
            break;
        case EXPR_LITERAL:
            return emitLiteralExpr(compiler, (LiteralExpr*)expr);
            break;
        case EXPR_VARIABLE:
            return emitVariableExpr(compiler, (VariableExpr*)expr);
            break;
        case EXPR_ASSIGN:
            return emitAssignExpr(compiler, (AssignExpr*)expr);
            break;
        case EXPR_CALL:
            return emitCallExpr(compiler, (CallExpr*)expr);
            break;
        case EXPR_CAST:
            return emitCastExpr(compiler, (CastExpr*)expr);
            break;
        case EXPR_GROUPING:
            return compileExpr(compiler, ((GroupingExpr*)expr)->expression);
            break;
        case EXPR_GET:
            return emitGetExpr(compiler, (GetExpr*)expr);
            break;
        case EXPR_SET:
            return emitSetExpr(compiler, (SetExpr*)expr);
            break;
        case EXPR_POSTFIX:
            //i++
            return emitPostfixExpr(compiler, (PostfixExpr*)expr);
            break;
        case EXPR_PREFIX:
            //++i
            return emitPrefixExpr(compiler, (PrefixExpr*)expr);
            break;
        case EXPR_LAMBDA:
            return emitLambdaExpr(compiler, (LambdaExpr*)expr);
            break;
        case EXPR_MAP_LITERAL:
            return emitMapLiteralExpr(compiler, (MapLiteralExpr*)expr);
            break;
        case EXPR_ARRAY_LITERAL:
            return emitArrayLiteralExpr(compiler, (ArrayLiteralExpr*)expr);
            break;
        case EXPR_BRACE_LITERAL:
            return emitBraceLiteralExpr(compiler, (BraceLiteralExpr*)expr);
            break;
        case EXPR_STRUCT_INIT:
            return emitStructInitExpr(compiler, (StructInitExpr*)expr);
            break;
        case EXPR_INDEX:
            return emitIndexExpr(compiler, (IndexExpr*)expr);
            break;
        case EXPR_INDEX_SET:
            return emitIndexSetExpr(compiler, (IndexSetExpr*)expr);
            break;

    }
    compilerDebug("Compiled expression end %d\n", expr->type);
    return NULL;
}

LLVMValueRef compileExprMulti(Compiler* compiler, Expr* expr) {
    if (!compiler || !expr) return NULL;

    // Multi-values only matter for direct call expressions.
    // Everything else keeps the default "first value" rule for nested calls.
    Expr* target = expr;
    if (target->type == EXPR_GROUPING) {
        target = ((GroupingExpr*)target)->expression;
        if (!target) return NULL;
    }
    if (target->type != EXPR_CALL) {
        return compileExpr(compiler, expr);
    }

    int saved = compiler->wantMultiValue;
    compiler->wantMultiValue = 1;
    LLVMValueRef v = compileExpr(compiler, expr);
    compiler->wantMultiValue = saved;
    return v;
}

static int exprHasLambdaLiteral(Expr* e) {
    if (!e) return 0;
    if (e->type == EXPR_LAMBDA) return 1;
    switch (e->type) {
        case EXPR_BINARY:
            return exprHasLambdaLiteral(((BinaryExpr*)e)->left) || exprHasLambdaLiteral(((BinaryExpr*)e)->right);
        case EXPR_UNARY:
            return exprHasLambdaLiteral(((UnaryExpr*)e)->right);
        case EXPR_GROUPING:
            return exprHasLambdaLiteral(((GroupingExpr*)e)->expression);
        case EXPR_CALL: {
            CallExpr* c = (CallExpr*)e;
            if (exprHasLambdaLiteral(c->callee)) return 1;
            for (ListNode* n = c->arguments ? c->arguments->head : NULL; n != NULL; n = n->next) {
                if (exprHasLambdaLiteral((Expr*)n->data)) return 1;
            }
            return 0;
        }
        case EXPR_ASSIGN:
            return exprHasLambdaLiteral(((AssignExpr*)e)->value);
        case EXPR_GET:
            return exprHasLambdaLiteral(((GetExpr*)e)->object);
        case EXPR_SET: {
            SetExpr* s = (SetExpr*)e;
            return exprHasLambdaLiteral(s->object) || exprHasLambdaLiteral(s->value);
        }
        case EXPR_MAP_LITERAL: {
            MapLiteralExpr* m = (MapLiteralExpr*)e;
            for (ListNode* n = m->entries ? m->entries->head : NULL; n != NULL; n = n->next) {
                MapEntry* me = (MapEntry*)n->data;
                if (me && exprHasLambdaLiteral(me->value)) return 1;
            }
            return 0;
        }
        case EXPR_ARRAY_LITERAL: {
            ArrayLiteralExpr* a = (ArrayLiteralExpr*)e;
            for (ListNode* n = a->elements ? a->elements->head : NULL; n != NULL; n = n->next) {
                if (exprHasLambdaLiteral((Expr*)n->data)) return 1;
            }
            return 0;
        }
        case EXPR_INDEX: {
            IndexExpr* i = (IndexExpr*)e;
            return exprHasLambdaLiteral(i->object) || exprHasLambdaLiteral(i->index);
        }
        case EXPR_INDEX_SET: {
            IndexSetExpr* s = (IndexSetExpr*)e;
            return exprHasLambdaLiteral(s->object) || exprHasLambdaLiteral(s->index) || exprHasLambdaLiteral(s->value);
        }
        case EXPR_POSTFIX:
            return exprHasLambdaLiteral(((PostfixExpr*)e)->operand);
        case EXPR_PREFIX:
            return exprHasLambdaLiteral(((PrefixExpr*)e)->operand);
        default:
            return 0;
    }
}

static int stmtHasLambdaLiteral(Stmt* s) {
    if (!s) return 0;
    if (s->type == STMT_PRIVATE) return stmtHasLambdaLiteral(((PrivateStmt*)s)->inner);
    switch (s->type) {
        case STMT_VAR:
            return exprHasLambdaLiteral(((VarStmt*)s)->initializer);
        case STMT_DESTRUCTURE:
            return exprHasLambdaLiteral(((DestructureStmt*)s)->value);
        case STMT_EXPR:
            return exprHasLambdaLiteral(((ExprStmt*)s)->expression);
        case STMT_RETURN: {
            ReturnStmt* r = (ReturnStmt*)s;
            if (r->values) {
                for (ListNode* n = r->values->head; n != NULL; n = n->next) {
                    if (exprHasLambdaLiteral((Expr*)n->data)) return 1;
                }
                return 0;
            }
            return exprHasLambdaLiteral(r->value);
        }
        case STMT_BLOCK: {
            BlockStmt* b = (BlockStmt*)s;
            for (ListNode* n = b->statements ? b->statements->head : NULL; n != NULL; n = n->next) {
                if (stmtHasLambdaLiteral((Stmt*)n->data)) return 1;
            }
            return 0;
        }
        case STMT_IF: {
            IfStmt* i = (IfStmt*)s;
            return exprHasLambdaLiteral(i->condition) || stmtHasLambdaLiteral(i->thenBranch) || stmtHasLambdaLiteral(i->elseBranch);
        }
        case STMT_FOR: {
            ForStmt* f = (ForStmt*)s;
            return stmtHasLambdaLiteral(f->initializer) || exprHasLambdaLiteral(f->condition) || exprHasLambdaLiteral(f->increment) || stmtHasLambdaLiteral(f->body);
        }
        case STMT_FOR_IN: {
            ForInStmt* fi = (ForInStmt*)s;
            return exprHasLambdaLiteral(fi->range) || stmtHasLambdaLiteral(fi->body);
        }
        case STMT_WHILE: {
            WhileStmt* w = (WhileStmt*)s;
            return exprHasLambdaLiteral(w->condition) || stmtHasLambdaLiteral(w->body);
        }
        case STMT_DO_WHILE: {
            DoWhileStmt* dw = (DoWhileStmt*)s;
            return exprHasLambdaLiteral(dw->condition) || stmtHasLambdaLiteral(dw->body);
        }
        default:
            return 0;
    }
}

static int tailrecIsSelfCall(Compiler* compiler, const Token* calleeName) {
    if (!compiler || !calleeName) return 0;
    if (!compiler->current || !compiler->current->func) return 0;

    const char* cur = LLVMGetValueName(compiler->current->func);
    if (cur && tokenEqualsCString(calleeName, cur)) return 1;

    // Module-qualified self call: `foo(...)` inside module where function is compiled as `mod__foo`.
    if (compiler->currentModulePrefix) {
        int ql = 0;
        char* q = compilerQualifyToken(compiler, calleeName, &ql);
        if (q) {
            int ok = cur && ql == (int)strlen(cur) && memcmp(q, cur, (size_t)ql) == 0;
            free(q);
            if (ok) return 1;
        }
    }

    return 0;
}

static int tailrecTryRewriteReturn(Compiler* compiler, ReturnStmt* stmt) {
    if (!compiler || !stmt) return 0;
    TailrecState* tr = (TailrecState*)compiler->tailrec;
    if (!tr || !tr->enabled || !tr->loop || !tr->func) return 0;
    if (!compiler->current || compiler->current->func != tr->func) return 0;

    // Only `return f(args...)` (single return value).
    Expr* retExpr = NULL;
    if (stmt->values && stmt->values->length > 0) {
        if (stmt->values->length != 1) return 0;
        retExpr = (Expr*)stmt->values->head->data;
    } else {
        retExpr = stmt->value;
    }
    if (!retExpr || retExpr->type != EXPR_CALL) return 0;

    CallExpr* call = (CallExpr*)retExpr;
    if (!call->callee || call->callee->type != EXPR_VARIABLE) return 0;
    VariableExpr* callee = (VariableExpr*)call->callee;
    if (!tailrecIsSelfCall(compiler, &callee->name)) return 0;

    unsigned got = call->arguments ? (unsigned)call->arguments->length : 0;
    if ((int)got != tr->paramCount) return 0;

    LLVMBuilderRef builder = compiler->builder;

    // Evaluate all arguments first (preserve argument evaluation semantics).
    LLVMValueRef* argVals = NULL;
    if (tr->paramCount > 0) {
        argVals = malloc(sizeof(LLVMValueRef) * (size_t)tr->paramCount);
        ListNode* n = call->arguments ? call->arguments->head : NULL;
        for (int i = 0; i < tr->paramCount; i++) {
            if (!n) {
                free(argVals);
                return 0;
            }
            LLVMValueRef v = compileExpr(compiler, (Expr*)n->data);
            v = castValueToType(compiler, v, tr->paramTypes[i]);
            argVals[i] = v;
            n = n->next;
        }
    }

    // Assign arguments into parameter storage slots.
    for (int i = 0; i < tr->paramCount; i++) {
        if (tr->paramIsBoxed && tr->paramIsBoxed[i]) {
            LLVMTypeRef cellTy = tr->paramBoxPtrTypes ? tr->paramBoxPtrTypes[i] : NULL;
            if (!cellTy) {
                if (argVals) free(argVals);
                return 0;
            }
            LLVMValueRef cell = LLVMBuildLoad2(builder, cellTy, tr->paramSlots[i], "tr_cell");
            LLVMBuildStore(builder, argVals[i], cell);
        } else {
            LLVMBuildStore(builder, argVals[i], tr->paramSlots[i]);
        }
    }
    if (argVals) free(argVals);

    // Jump back to loop header instead of calling recursively.
    LLVMBuildBr(builder, tr->loop);
    return 1;
}

void compileStmt(Compiler* compiler, Stmt* stmt) {
    compilerDebug("Compiling statement %s\n", stmtTypeToString(stmt->type));
#ifdef DEBUG
    printStmt(stmt, 0);
#endif

    switch (stmt->type) {
        case STMT_IF:
            compileIfStmt(compiler, (IfStmt*)stmt);
            break;
        case STMT_FOR:
            compileForStmt(compiler, (ForStmt*)stmt);
            break;
        case STMT_FOR_IN:
            compileForInStmt(compiler, (ForInStmt*)stmt);
            break;
        case STMT_WHILE:
            compileWhileStmt(compiler, (WhileStmt*)stmt);
            break;
        case STMT_DO_WHILE:
            compileDoWhileStmt(compiler, (DoWhileStmt*)stmt);
            break;
        case STMT_BREAK:
            compileBreakStmt(compiler, (BreakStmt*)stmt);
            break;
        case STMT_CONTINUE:
            compileContinueStmt(compiler, (ContinueStmt*)stmt);
            break;
        case STMT_LABEL:
            compileLabelStmt(compiler, (LabelStmt*)stmt);
            break;
        case STMT_GOTO:
            compileGotoStmt(compiler, (GotoStmt*)stmt);
            break;
        case STMT_IMPORT:
        case STMT_FROM_IMPORT:
            // handled by module loader
            break;
        case STMT_PRIVATE:
            compileStmt(compiler, ((PrivateStmt*)stmt)->inner);
            break;
        case STMT_BLOCK:
            compileBlockStmt(compiler, (BlockStmt*)stmt);
            break;
        case STMT_RETURN:
            compileReturnStmt(compiler, (ReturnStmt*)stmt);
            break;
        case STMT_EXPR:
            compileExprStmt(compiler, (ExprStmt*)stmt);
            break;
        case STMT_VAR:
            compileVarStmt(compiler, (VarStmt*)stmt);
            break;
        case STMT_DESTRUCTURE:
            compileDestructureStmt(compiler, (DestructureStmt*)stmt);
            break;
        case STMT_FUNC:
            compileFuncStmt(compiler, (FuncStmt*)stmt);
            break;
        case STMT_STRUCT:
            compileStructStmt(compiler, (StructStmt*)stmt);
            break;
        case STMT_OBJECT:
            compileObjectStmt(compiler, (ObjectStmt*)stmt);
            break;
        case STMT_ENUM:
            compileEnumStmt(compiler, (EnumStmt*)stmt);
            break;
        case STMT_IMPL:
            compileImplStmt(compiler, (ImplStmt*)stmt);
            break;
    }
}

void compileIfStmt(Compiler* compiler, IfStmt* stmt) {
    emitIfStmt(compiler, stmt);
}

void compileForStmt(Compiler* compiler, ForStmt* stmt) {
    emitForStmt(compiler, stmt);
    
}

void compileWhileStmt(Compiler* compiler, WhileStmt* stmt) {
    emitWhileStmt(compiler, stmt);
}

void compileDoWhileStmt(Compiler* compiler, DoWhileStmt* stmt) {
    emitDoWhileStmt(compiler, stmt);
}

void compileBreakStmt(Compiler* compiler, BreakStmt* stmt) {
    (void)stmt;
    emitBreakStmt(compiler);
}

void compileContinueStmt(Compiler* compiler, ContinueStmt* stmt) {
    (void)stmt;
    emitContinueStmt(compiler);
}

void compileLabelStmt(Compiler* compiler, LabelStmt* stmt) {
    emitLabelStmt(compiler, stmt);
}

void compileGotoStmt(Compiler* compiler, GotoStmt* stmt) {
    emitGotoStmt(compiler, stmt);
}

void compileForInStmt(Compiler* compiler, ForInStmt* stmt) {
    emitForInStmt(compiler, stmt);
}

void compileBlockStmt(Compiler* compiler, BlockStmt* stmt){
    compilerDebug("Compiling block statement\n");

    // Enter a new lexical scope for name resolution (LLVM JIT path).
    Block* saved = compiler->current;
    Block* scoped = malloc(sizeof(Block));
    scoped->parent = saved;
    scoped->func = saved ? saved->func : NULL;
    scoped->variables = listNew();
    // Labels should be function-scoped (Lua-style goto/labels).
    scoped->labels = saved ? saved->labels : listNew();
    compiler->current = scoped;

    ListNode* node = stmt->statements->head;
    while (node != NULL) {
        if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(compiler->builder))) break;
        compileStmt(compiler, (Stmt*)node->data);
        node = node->next;
    }

    // Drop locals on normal block exit (best-effort; early returns handled in compileReturnStmt).
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(compiler->builder))) {
        emitDropForBlockVars(compiler, scoped);
    }

    compiler->current = saved;
    compilerDebug("Compiled block statement end\n");
}
void compileReturnStmt(Compiler* compiler, ReturnStmt* stmt){
    compilerDebug("Compiling return statement\n");
    LLVMBuilderRef builder = compiler->builder;
    if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(builder))) {
        return;
    }

    LLVMTypeRef funcType = LLVMGlobalGetValueType(compiler->current->func);
    LLVMTypeRef returnType = LLVMGetReturnType(funcType);

    if (tailrecTryRewriteReturn(compiler, stmt)) {
        return;
    }

    if (LLVMGetTypeKind(returnType) == LLVMVoidTypeKind) {
        emitDropForCurrentFunctionScopes(compiler);
        LLVMBuildRetVoid(builder);
        return;
    }

    const char* fnName = LLVMGetValueName(compiler->current->func);
    int fnNameLen = fnName ? (int)strlen(fnName) : 0;
    int multiCount = compilerMultiReturnCount(compiler, fnName, fnNameLen);

    if (multiCount > 1 && LLVMGetTypeKind(returnType) == LLVMStructTypeKind) {
        // Special-case `return f()` forwarding.
        if (stmt->values && stmt->values->length == 1) {
            Expr* only = (Expr*)stmt->values->head->data;
            LLVMValueRef mv = compileExprMulti(compiler, only);
            if (mv && LLVMTypeOf(mv) == returnType) {
                emitDropForCurrentFunctionScopes(compiler);
                LLVMBuildRet(builder, mv);
                return;
            }
        }

        unsigned elementCount = LLVMCountStructElementTypes(returnType);
        LLVMValueRef out = LLVMGetUndef(returnType);
        unsigned provided = stmt->values ? (unsigned)stmt->values->length : (stmt->value ? 1u : 0u);
        ListNode* node = stmt->values ? stmt->values->head : NULL;

        for (unsigned i = 0; i < elementCount; i++) {
            LLVMValueRef v = NULL;
            if (node && i < provided) {
                Expr* srcExpr = (Expr*)node->data;
                v = compileExpr(compiler, srcExpr);
                moveOutOnReturnIfNeeded(compiler, srcExpr);
                node = node->next;
                LLVMTypeRef want = LLVMStructGetTypeAtIndex(returnType, i);
                v = castValueToType(compiler, v, want);
            }
            if (!v) {
                LLVMTypeRef t = LLVMStructGetTypeAtIndex(returnType, i);
                v = LLVMConstNull(t);
            }
            out = LLVMBuildInsertValue(builder, out, v, i, "mvr");
        }
        emitDropForCurrentFunctionScopes(compiler);
        LLVMBuildRet(builder, out);
        return;
    }

    LLVMValueRef returnValue = NULL;
    if (stmt->values && stmt->values->length > 0) {
        if (stmt->values->length > 1) {
            error("Function returns single value, but return has multiple expressions\n");
        }
        Expr* srcExpr = (Expr*)stmt->values->head->data;
        returnValue = compileExpr(compiler, srcExpr);
        moveOutOnReturnIfNeeded(compiler, srcExpr);
    } else if (stmt->value != NULL) {
        returnValue = compileExpr(compiler, stmt->value);
        moveOutOnReturnIfNeeded(compiler, stmt->value);
    }
    if (returnValue == NULL) {
        returnValue = LLVMConstNull(returnType);
    }
    returnValue = castValueToType(compiler, returnValue, returnType);
    emitDropForCurrentFunctionScopes(compiler);
    LLVMBuildRet(builder, returnValue);
}
void compileExprStmt(Compiler* compiler, ExprStmt* stmt){
    compilerDebug("Compiling Expr statement\n");
    compileExpr(compiler, stmt->expression);
}

static char* mangleTwo(const Token* left, const Token* right, const char* sep, int* outLen) {
    int sepLen = (int)strlen(sep);
    int len = left->length + sepLen + right->length;
    char* s = malloc((size_t)len + 1);
    memcpy(s, left->start, (size_t)left->length);
    memcpy(s + left->length, sep, (size_t)sepLen);
    memcpy(s + left->length + sepLen, right->start, (size_t)right->length);
    s[len] = '\0';
    if (outLen) *outLen = len;
    return s;
}


static const char* typeToLLVM(Type* type) {
    if (type == NULL) return "i8*";  // Any type
    
    switch (type->kind) {
        case TYPE_I8:
        case TYPE_U8:
        case TYPE_BYTE:
        case TYPE_F8:
        case TYPE_BF8:
            return "i8";
        case TYPE_I16:
        case TYPE_U16:
            return "i16";
        case TYPE_INT:    return "i32";
        case TYPE_U32:    return "i32";
        case TYPE_LONG:   return "i64";
        case TYPE_U64:    return "i64";
        case TYPE_ISIZE:
        case TYPE_USIZE:
            return (sizeof(void*) == 8) ? "i64" : "i32";
        case TYPE_F16:    return "half";
        case TYPE_DOUBLE: return "double";
        case TYPE_FLOAT:  return "float";
        case TYPE_BF16:   return "bfloat";
        case TYPE_STRING: return "i8*";
        case TYPE_BOOL:   return "i1";
        default:         return "UNKNOWN";
    }
}

// static void emitIR(Compiler* compiler, const char* format, ...) {
//     va_list args;
//     va_start(args, format);
    
//     // Get buffer size needed
//     va_list args_copy;
//     va_copy(args_copy, args);
//     int size = vsnprintf(NULL, 0, format, args_copy);
//     va_end(args_copy);
    
//     // Allocate buffer
//     char* buffer = malloc(size + 1);
//     vsnprintf(buffer, size + 1, format, args);
//     va_end(args);
    
//     // Add to IR output list
//     IRLine* line = malloc(sizeof(IRLine));
//     line->text = buffer;
//     line->indent = compiler->scopeDepth * 2;  // 2 spaces per scope level
//     listAppend(compiler->ir, line);
// }

void compileVarStmt(Compiler* compiler, VarStmt* stmt) {
    compilerDebug("compileVarStmt: %.*s\n", stmt->name.length, stmt->name.start);
    emitVarStmt(compiler, stmt);
}

void compileImplStmt(Compiler* compiler, ImplStmt* stmt) {
    if (!compiler || !stmt) return;
    if (!stmt->methods) return;

    StructInfo* info = compilerResolveStructByToken(compiler, &stmt->name);
    if (!info) {
        error("Unknown struct for impl: %.*s\n", stmt->name.length, stmt->name.start);
        return;
    }

    Token structTok = stmt->name;
    structTok.start = info->name;
    structTok.length = info->nameLength;

    for (ListNode* node = stmt->methods->head; node != NULL; node = node->next) {
        FuncStmt* method = (FuncStmt*)node->data;
        if (!method) continue;

        int mangledLen = 0;
        char* mangled = mangleTwo(&structTok, &method->name, "__", &mangledLen);

        // If a function with this mangled name already exists, treat as duplicate method definition.
        LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, mangled);
        if (existing) {
            error("Duplicate method definition: %.*s.%.*s\n",
                  stmt->name.length, stmt->name.start,
                  method->name.length, method->name.start);
            free(mangled);
            continue;
        }

        Token mangledTok = method->name;
        mangledTok.start = mangled;
        mangledTok.length = mangledLen;

        // Build params: this + original params
        List* params = listNew();
        Token thisNameTok = (Token){TOKEN_IDENTIFIER, "this", 4, method->name.line, method->name.col, 0};

        Type* thisInner = malloc(sizeof(Type));
        thisInner->kind = TYPE_NAMED;
        thisInner->name = structTok;
        thisInner->inner = NULL;
        thisInner->paramTypes = NULL;
        thisInner->returnTypes = NULL;

        Type* thisType = malloc(sizeof(Type));
        thisType->kind = TYPE_REF;
        thisType->name = (Token){0};
        thisType->inner = thisInner;
        thisType->paramTypes = NULL;
        thisType->returnTypes = NULL;

        Parameter* thisParam = malloc(sizeof(Parameter));
        thisParam->name = thisNameTok;
        thisParam->type = thisType;
        listAppend(params, thisParam);

        if (method->params) {
            for (ListNode* p = method->params->head; p != NULL; p = p->next) {
                listAppend(params, p->data);
            }
        }

        // Compile as a normal function statement with mangled name.
        FuncStmt tmp = *method;
        tmp.name = mangledTok;
        tmp.params = params;
        compileFuncStmt(compiler, &tmp);

        free(mangled);
    }
}

static const char* llvmStructNameOrNull(LLVMTypeRef t) {
    if (!t) return NULL;
    if (LLVMGetTypeKind(t) != LLVMStructTypeKind) return NULL;
    return LLVMGetStructName(t);
}

void compileDestructureStmt(Compiler* compiler, DestructureStmt* stmt) {
    if (!compiler || !stmt) return;
    if (!stmt->names || stmt->names->length <= 0) return;
    if (!stmt->value) {
        error("Destructuring requires a RHS expression\n");
        return;
    }

    LLVMValueRef rhs = compileExprMulti(compiler, stmt->value);
    if (!rhs) {
        error("Failed to compile RHS of destructuring\n");
        return;
    }

    LLVMTypeRef rhsType = LLVMTypeOf(rhs);
    if (LLVMGetTypeKind(rhsType) != LLVMStructTypeKind) {
        error("Destructuring RHS must return multiple values\n");
        return;
    }

    unsigned rhsCount = LLVMCountStructElementTypes(rhsType);
    if ((int)rhsCount != stmt->names->length) {
        error("Destructuring arity mismatch: want %d values, got %u\n", stmt->names->length, rhsCount);
    }
    unsigned useCount = rhsCount;
    if ((int)useCount > stmt->names->length) useCount = (unsigned)stmt->names->length;

    if (stmt->isDeclaration) {
        for (unsigned i = 0; i < useCount; i++) {
            Token* nameTok = (Token*)listGet(stmt->names, (int)i);
            Type* declaredType = stmt->types ? (Type*)listGet(stmt->types, (int)i) : NULL;
            if (!nameTok) continue;

            LLVMValueRef v = LLVMBuildExtractValue(compiler->builder, rhs, i, "mv");
            LLVMTypeRef targetType = declaredType ? typeToLLVMType(compiler, declaredType, false)
                                                  : LLVMStructGetTypeAtIndex(rhsType, i);
            v = castValueToType(compiler, v, targetType);

            char* varName = malloc((size_t)nameTok->length + 1);
            memcpy(varName, nameTok->start, (size_t)nameTok->length);
            varName[nameTok->length] = '\0';

            int isBoxed = compiler->boxAllLocals;
            LLVMTypeRef boxPtrType = isBoxed ? LLVMPointerType(targetType, 0) : NULL;
            LLVMValueRef slot = NULL;
            if (isBoxed) {
                slot = LLVMBuildAlloca(compiler->builder, boxPtrType, varName);
                LLVMValueRef mallocFn = LLVMGetNamedFunction(compiler->module, "malloc");
                if (!mallocFn) {
                    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
                    LLVMTypeRef i64 = LLVMInt64TypeInContext(compiler->context);
                    LLVMTypeRef mty = LLVMFunctionType(i8ptr, &i64, 1, 0);
                    mallocFn = LLVMAddFunction(compiler->module, "malloc", mty);
                }
                LLVMValueRef sizeV = LLVMSizeOf(targetType);
                LLVMValueRef raw = LLVMBuildCall2(compiler->builder, LLVMGlobalGetValueType(mallocFn), mallocFn, &sizeV, 1, "malloc");
                LLVMValueRef cell = LLVMBuildBitCast(compiler->builder, raw, boxPtrType, "cell");
                if (v) LLVMBuildStore(compiler->builder, v, cell);
                LLVMBuildStore(compiler->builder, cell, slot);
            } else {
                slot = LLVMBuildAlloca(compiler->builder, targetType, varName);
                if (v) LLVMBuildStore(compiler->builder, v, slot);
            }

            VariableRef* variable = malloc(sizeof(VariableRef));
            variable->name = varName;
            variable->length = nameTok->length;
            variable->value = slot;
            variable->type = targetType;

            const char* typeName = llvmStructNameOrNull(targetType);
            if (typeName) {
                variable->typeName = typeName;
                variable->typeNameLength = (int)strlen(typeName);
            } else {
                variable->typeName = NULL;
                variable->typeNameLength = 0;
            }

            variable->isConst = stmt->isConst ? 1 : 0;
            variable->isGlobal = 0;
            variable->isBoxed = isBoxed;
            variable->boxPtrType = isBoxed ? boxPtrType : NULL;
            variable->isTypedMap = 0;
            variable->mapKeyType = NULL;
            variable->mapValueType = NULL;
            variable->isArray = (targetType == compilerGetArrayType(compiler));
            variable->arrayElemType = NULL;
            variable->arrayFixedLen = -1;
            variable->isStackArray = 0;
            variable->stackArrayData = NULL;
            variable->isMap = (targetType == compilerGetMapType(compiler));
            listAppend(compiler->current->variables, variable);
        }
    } else {
        for (unsigned i = 0; i < useCount; i++) {
            Token* nameTok = (Token*)listGet(stmt->names, (int)i);
            if (!nameTok) continue;

            VariableExpr ve;
            memset(&ve, 0, sizeof(ve));
            ve.base.type = EXPR_VARIABLE;
            ve.name = *nameTok;

            VariableRef var = findVariableExpr(compiler, (Expr*)&ve);
            if (!var.value) {
                error("Undefined variable in destructuring assignment: %.*s\n", nameTok->length, nameTok->start);
                continue;
            }
            if (var.isConst) {
                error("Cannot assign to const in destructuring assignment: %.*s\n", nameTok->length, nameTok->start);
                continue;
            }

            LLVMValueRef v = LLVMBuildExtractValue(compiler->builder, rhs, i, "mv");
            v = castValueToType(compiler, v, var.type);
            if (v) {
                if (var.isBoxed) {
                    LLVMValueRef cellPtr = LLVMBuildLoad2(compiler->builder, var.boxPtrType, var.value, "cellptr");
                    LLVMBuildStore(compiler->builder, v, cellPtr);
                } else {
                    LLVMBuildStore(compiler->builder, v, var.value);
                }
            }
        }
    }
}

void compileFuncStmt(Compiler* compiler, FuncStmt* stmt) {
    compilerDebug("Compiling function statement %.*s\n", stmt->name.length, stmt->name.start);

    char* funcName = malloc((size_t)stmt->name.length + 1);
    memcpy(funcName, stmt->name.start, (size_t)stmt->name.length);
    funcName[stmt->name.length] = '\0';

    // If this function contains any lambda literal, box all locals/params to make
    // upvalue-by-reference safe without a separate "close upvalues" phase.
    int savedBox = compiler->boxAllLocals;
    int containsLambda = 0;
    for (ListNode* n = stmt->body ? stmt->body->head : NULL; n != NULL; n = n->next) {
        if (stmtHasLambdaLiteral((Stmt*)n->data)) { containsLambda = 1; break; }
    }
    compiler->boxAllLocals = containsLambda ? 1 : savedBox;

    int paramCount = stmt->params ? stmt->params->length : 0;
    LLVMTypeRef* paramTypes = NULL;
    if (paramCount > 0) {
        paramTypes = malloc(sizeof(LLVMTypeRef) * (size_t)paramCount);
        for (int i = 0; i < paramCount; i++) {
            Parameter* p = listGet(stmt->params, i);
            paramTypes[i] = typeToLLVMType(compiler, p->type, false);
        }
    }

    LLVMTypeRef retType = LLVMVoidTypeInContext(compiler->context);
    if (stmt->returnTypes && stmt->returnTypes->length > 1) {
        int rc = stmt->returnTypes->length;
        LLVMTypeRef* rts = malloc(sizeof(LLVMTypeRef) * (size_t)rc);
        for (int i = 0; i < rc; i++) {
            Type* t = listGet(stmt->returnTypes, i);
            rts[i] = typeToLLVMType(compiler, t, false);
        }
        retType = LLVMStructTypeInContext(compiler->context, rts, (unsigned)rc, 0);
        compilerRegisterMultiReturn(compiler, funcName, stmt->name.length, rc);
        free(rts);
    } else {
        retType = typeToLLVMType(compiler, stmt->returnType, true);
    }
    LLVMTypeRef funcType = LLVMFunctionType(retType, paramTypes, (unsigned)paramCount, 0);
    LLVMValueRef func = LLVMAddFunction(compiler->module, funcName, funcType);

    // If this function returns a closure value (possibly nested), record the expected closure call signature(s)
    // so expressions like `makeAdder(1)(2)` and deeper chains like `bar(1)(2)(3)` can be compiled.
    if (stmt->returnType && stmt->returnType->kind == TYPE_FUNC) {
        compilerRegisterClosureReturnSigChain(compiler, funcName, stmt->name.length, stmt->returnType);
    }

    // `extern fn` declaration: declare prototype only (no body).
    if (stmt->body == NULL) {
        // `extern fn symbol(...) T as localName`
        // Sugar: declare the external symbol (as-is), then generate a module-local wrapper
        // `<modulePrefix>__localName` that forwards to `symbol`.
        if (stmt->externAlias.length > 0) {
            int wlen = 0;
            char* wname = NULL;
            if (compiler && compiler->currentModulePrefix) {
                const int sepLen = 2;
                wlen = compiler->currentModulePrefixLen + sepLen + stmt->externAlias.length;
                wname = malloc((size_t)wlen + 1);
                memcpy(wname, compiler->currentModulePrefix, (size_t)compiler->currentModulePrefixLen);
                memcpy(wname + compiler->currentModulePrefixLen, "__", (size_t)sepLen);
                memcpy(wname + compiler->currentModulePrefixLen + sepLen, stmt->externAlias.start, (size_t)stmt->externAlias.length);
                wname[wlen] = '\0';
            } else {
                wlen = stmt->externAlias.length;
                wname = malloc((size_t)wlen + 1);
                memcpy(wname, stmt->externAlias.start, (size_t)stmt->externAlias.length);
                wname[wlen] = '\0';
            }

            // Reject conflicts: the alias wrapper is emitted as a normal (internal) function,
            // so the generated name must be unique within the module.
            LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, wname);
            if (existing) {
                compilerErrorAtToken(
                    compiler,
                    &stmt->externAlias,
                    "extern alias '%.*s' conflicts with an existing symbol",
                    stmt->externAlias.length,
                    stmt->externAlias.start
                );
            } else {
                LLVMValueRef wrapper = LLVMAddFunction(compiler->module, wname, funcType);
                LLVMSetLinkage(wrapper, LLVMInternalLinkage);

                // Mirror multi-return and closure-return metadata onto the wrapper
                // so calling the alias behaves like calling the extern directly.
                if (stmt->returnTypes && stmt->returnTypes->length > 1) {
                    compilerRegisterMultiReturn(compiler, wname, wlen, stmt->returnTypes->length);
                }
                if (stmt->returnType && stmt->returnType->kind == TYPE_FUNC) {
                    compilerRegisterClosureReturnSigChain(compiler, wname, wlen, stmt->returnType);
                }

                LLVMBasicBlockRef savedBlock = LLVMGetInsertBlock(compiler->builder);
                Block* savedCurrent = compiler->current;

                LLVMBasicBlockRef entry = LLVMAppendBasicBlock(wrapper, "entry");
                LLVMPositionBuilderAtEnd(compiler->builder, entry);

                // Forward all arguments to the external function prototype.
                LLVMValueRef* args = NULL;
                if (paramCount > 0) {
                    args = malloc(sizeof(LLVMValueRef) * (size_t)paramCount);
                    for (int i = 0; i < paramCount; i++) {
                        args[i] = LLVMGetParam(wrapper, (unsigned)i);
                    }
                }

                LLVMValueRef call = LLVMBuildCall2(compiler->builder, funcType, func, args, (unsigned)paramCount, "");
                if (args) free(args);

                if (LLVMGetTypeKind(retType) == LLVMVoidTypeKind) {
                    LLVMBuildRetVoid(compiler->builder);
                } else {
                    LLVMBuildRet(compiler->builder, call);
                }

                if (savedBlock) LLVMPositionBuilderAtEnd(compiler->builder, savedBlock);
                compiler->current = savedCurrent;
            }

            free(wname);
        }

        if (paramTypes) free(paramTypes);
        free(funcName);
        return;
    }

    // Save current insertion point (main)
    LLVMBasicBlockRef savedBlock = LLVMGetInsertBlock(compiler->builder);
    Block* savedCurrent = compiler->current;
    const char* savedLastSetFilePath = compiler->lastSetFilePath;
    int savedLastSetLine = compiler->lastSetLine;
    int savedLastSetCol = compiler->lastSetCol;
    TailrecState* savedTailrec = (TailrecState*)compiler->tailrec;

    // Create function entry
    LLVMBasicBlockRef entry = LLVMAppendBasicBlock(func, "entry");
    LLVMPositionBuilderAtEnd(compiler->builder, entry);
    compiler->lastSetFilePath = compiler->currentFilePath;
    compiler->lastSetLine = 0;
    compiler->lastSetCol = 0;

    Block* funcBlock = malloc(sizeof(Block));
    funcBlock->parent = savedCurrent; // allow lookup of globals (no closures yet)
    funcBlock->func = func;
    funcBlock->variables = listNew();
    funcBlock->labels = listNew();
    compiler->current = funcBlock;

    // Tail recursion elimination: enable only when this function is not using boxing for closures.
    // Boxing makes tail-call frame reuse observable for captured variables.
    TailrecState tr = {0};
    tr.enabled = compiler->boxAllLocals ? 0 : 1;
    tr.func = func;
    tr.paramCount = paramCount;
    if (paramCount > 0) {
        tr.paramSlots = malloc(sizeof(LLVMValueRef) * (size_t)paramCount);
        tr.paramTypes = malloc(sizeof(LLVMTypeRef) * (size_t)paramCount);
        tr.paramIsBoxed = malloc(sizeof(int) * (size_t)paramCount);
        tr.paramBoxPtrTypes = malloc(sizeof(LLVMTypeRef) * (size_t)paramCount);
        for (int i = 0; i < paramCount; i++) {
            tr.paramSlots[i] = NULL;
            tr.paramTypes[i] = NULL;
            tr.paramIsBoxed[i] = 0;
            tr.paramBoxPtrTypes[i] = NULL;
        }
    }
    compiler->tailrec = &tr;

    // Bind parameters into local allocas
    for (int i = 0; i < paramCount; i++) {
        Parameter* p = listGet(stmt->params, i);
        LLVMValueRef arg = LLVMGetParam(func, (unsigned)i);

        char* paramName = malloc((size_t)p->name.length + 1);
        memcpy(paramName, p->name.start, (size_t)p->name.length);
        paramName[p->name.length] = '\0';

        LLVMValueRef slot = NULL;
        int isBoxed = compiler->boxAllLocals;
        LLVMTypeRef valueType = paramTypes[i];
        LLVMTypeRef boxPtrType = isBoxed ? LLVMPointerType(valueType, 0) : NULL;
        if (isBoxed) {
            slot = LLVMBuildAlloca(compiler->builder, boxPtrType, paramName);
            LLVMValueRef mallocFn = LLVMGetNamedFunction(compiler->module, "malloc");
            if (!mallocFn) {
                LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
                LLVMTypeRef i64 = LLVMInt64TypeInContext(compiler->context);
                LLVMTypeRef mty = LLVMFunctionType(i8ptr, &i64, 1, 0);
                mallocFn = LLVMAddFunction(compiler->module, "malloc", mty);
            }
            LLVMValueRef sizeV = LLVMSizeOf(valueType);
            LLVMValueRef raw = LLVMBuildCall2(compiler->builder, LLVMGlobalGetValueType(mallocFn), mallocFn, &sizeV, 1, "malloc");
            LLVMValueRef cell = LLVMBuildBitCast(compiler->builder, raw, boxPtrType, "cell");
            LLVMBuildStore(compiler->builder, arg, cell);
            LLVMBuildStore(compiler->builder, cell, slot);
        } else {
            slot = LLVMBuildAlloca(compiler->builder, valueType, paramName);
            LLVMBuildStore(compiler->builder, arg, slot);
        }

        if (tr.paramSlots && tr.paramTypes && tr.paramIsBoxed && tr.paramBoxPtrTypes) {
            tr.paramSlots[i] = slot;
            tr.paramTypes[i] = valueType;
            tr.paramIsBoxed[i] = isBoxed ? 1 : 0;
            tr.paramBoxPtrTypes[i] = isBoxed ? boxPtrType : NULL;
        }

        VariableRef* variable = malloc(sizeof(VariableRef));
        variable->name = paramName;
        variable->length = p->name.length;
        variable->value = slot;
        variable->type = valueType;
        if (p->type && p->type->kind == TYPE_NAMED) {
            StructInfo* info = compilerResolveStructByToken(compiler, &p->type->name);
            if (info) {
                variable->typeName = info->name;
                variable->typeNameLength = info->nameLength;
            } else {
                variable->typeName = p->type->name.start;
                variable->typeNameLength = p->type->name.length;
            }
        } else if (p->type && p->type->kind == TYPE_REF && p->type->inner && p->type->inner->kind == TYPE_NAMED) {
            StructInfo* info = compilerResolveStructByToken(compiler, &p->type->inner->name);
            if (info) {
                variable->typeName = info->name;
                variable->typeNameLength = info->nameLength;
            } else {
                variable->typeName = p->type->inner->name.start;
                variable->typeNameLength = p->type->inner->name.length;
            }
        } else {
            variable->typeName = NULL;
            variable->typeNameLength = 0;
        }
        variable->isConst = 0;
        variable->isGlobal = 0;
        variable->isBoxed = isBoxed;
        variable->boxPtrType = isBoxed ? boxPtrType : NULL;
        variable->isMap = 0;
        variable->isTypedMap = 0;
        variable->mapKeyType = NULL;
        variable->mapValueType = NULL;
        variable->isArray = 0;
        variable->arrayElemType = NULL;
        variable->arrayFixedLen = -1;
        variable->isStackArray = 0;
        variable->stackArrayData = NULL;

        if (p->type && p->type->kind == TYPE_NAMED &&
            p->type->name.length == 3 && memcmp(p->type->name.start, "map", 3) == 0 &&
            p->type->typeArgs && p->type->typeArgs->length == 2) {
            variable->isMap = 1;
            Type* kAst = (Type*)p->type->typeArgs->head->data;
            Type* vAst = (Type*)p->type->typeArgs->head->next->data;
            int okKey = kAst && (kAst->kind == TYPE_STRING || kAst->kind == TYPE_INT || kAst->kind == TYPE_LONG);
            int okVal = vAst && (vAst->kind == TYPE_STRING || vAst->kind == TYPE_INT || vAst->kind == TYPE_LONG ||
                                 vAst->kind == TYPE_FLOAT || vAst->kind == TYPE_DOUBLE || vAst->kind == TYPE_BOOL);
            if (!okKey) {
                error("map<K,V> key type must be string/int/long for now\n");
            } else if (!okVal) {
                error("map<K,V> value type must be int/long/float/double/bool/string for now\n");
            } else {
                variable->isTypedMap = 1;
                variable->mapKeyType = typeToLLVMType(compiler, kAst, false);
                variable->mapValueType = typeToLLVMType(compiler, vAst, false);
            }
        }

        if (p->type && p->type->kind == TYPE_NAMED &&
            p->type->name.length == 3 && memcmp(p->type->name.start, "map", 3) == 0 &&
            (!p->type->typeArgs || p->type->typeArgs->length == 0)) {
            variable->isMap = 1;
        }

        if (p->type && p->type->kind == TYPE_ARRAY && valueType == compilerGetArrayType(compiler)) {
            variable->isArray = 1;
            variable->arrayElemType = p->type->inner ? typeToLLVMType(compiler, p->type->inner, false)
                                                     : LLVMInt32TypeInContext(compiler->context);
            variable->arrayFixedLen = p->type->arrayLen;
        }
        listAppend(funcBlock->variables, variable);

        // If parameter is annotated as a function type, record the closure call signature
        // so `f(x)` can be compiled inside this function body.
        if (p->type && p->type->kind == TYPE_FUNC) {
            LLVMTypeRef sig = compilerClosureSigFromType(compiler, p->type);
            if (sig) compilerRegisterClosureSig(compiler, variable->name, variable->length, sig);
        }
    }

    // Place the body in a loop header block so `return f(args...)` can branch back.
    if (tr.enabled) {
        tr.loop = LLVMAppendBasicBlock(func, "tailrecurse");
        LLVMBuildBr(compiler->builder, tr.loop);
        LLVMPositionBuilderAtEnd(compiler->builder, tr.loop);
    }

    // Compile function body
    for (ListNode* node = stmt->body ? stmt->body->head : NULL; node != NULL; node = node->next) {
        if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(compiler->builder))) break;
        compileStmt(compiler, (Stmt*)node->data);
    }

    // Implicit return
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(compiler->builder))) {
        emitDropForBlockVars(compiler, funcBlock);
        if (LLVMGetTypeKind(retType) == LLVMVoidTypeKind) {
            LLVMBuildRetVoid(compiler->builder);
        } else {
            LLVMBuildRet(compiler->builder, LLVMConstNull(retType));
        }
    }

    // Restore insertion point and compiler block
    compiler->current = savedCurrent;
    LLVMPositionBuilderAtEnd(compiler->builder, savedBlock);
    compiler->boxAllLocals = savedBox;
    compiler->lastSetFilePath = savedLastSetFilePath;
    compiler->lastSetLine = savedLastSetLine;
    compiler->lastSetCol = savedLastSetCol;
    compiler->tailrec = savedTailrec;

    if (tr.paramSlots) free(tr.paramSlots);
    if (tr.paramTypes) free(tr.paramTypes);
    if (tr.paramIsBoxed) free(tr.paramIsBoxed);
    if (tr.paramBoxPtrTypes) free(tr.paramBoxPtrTypes);

    if (paramTypes) free(paramTypes);
    free(funcName);
}

void compileStructStmt(Compiler* compiler, StructStmt* stmt) {
    if (!compiler || !stmt) return;

    // Register struct type
    char* structName = malloc((size_t)stmt->name.length + 1);
    memcpy(structName, stmt->name.start, (size_t)stmt->name.length);
    structName[stmt->name.length] = '\0';

    LLVMTypeRef structType = LLVMGetTypeByName2(compiler->context, structName);
    if (!structType) {
        structType = LLVMStructCreateNamed(compiler->context, structName);
    }

    // Set body (fields)
    int fieldCount = stmt->fields ? stmt->fields->length : 0;
    if (fieldCount > 0) {
        LLVMTypeRef* fieldTypes = malloc(sizeof(LLVMTypeRef) * (size_t)fieldCount);
        for (int i = 0; i < fieldCount; i++) {
            FieldDeclaration* f = listGet(stmt->fields, i);
            fieldTypes[i] = typeToLLVMType(compiler, f ? f->type : NULL, false);
        }
        LLVMStructSetBody(structType, fieldTypes, (unsigned)fieldCount, 0);
        free(fieldTypes);
    } else {
        LLVMStructSetBody(structType, NULL, 0, 0);
    }

    if (!compilerFindStruct(compiler, stmt->name.start, stmt->name.length)) {
        StructInfo* info = malloc(sizeof(StructInfo));
        info->name = structName;
        info->nameLength = stmt->name.length;
        info->type = structType;
        info->decl = stmt;
        listAppend(compiler->structs, info);
    } else {
        free(structName);
    }

    // Compile methods as `Struct__method(this: Struct*, ...)`
    if (stmt->methods) {
        for (ListNode* node = stmt->methods->head; node != NULL; node = node->next) {
            FuncStmt* method = (FuncStmt*)node->data;
            if (!method) continue;

            int mangledLen = 0;
            char* mangled = mangleTwo(&stmt->name, &method->name, "__", &mangledLen);
            Token mangledTok = method->name;
            mangledTok.start = mangled;
            mangledTok.length = mangledLen;

            // Build params: this + original params
            List* params = listNew();
            Token thisNameTok = (Token){TOKEN_IDENTIFIER, "this", 4, method->name.line, method->name.col, 0};
            Type* thisInner = malloc(sizeof(Type));
            thisInner->kind = TYPE_NAMED;
            thisInner->name = stmt->name;
            thisInner->inner = NULL;
            thisInner->paramTypes = NULL;
            thisInner->returnTypes = NULL;

            Type* thisType = malloc(sizeof(Type));
            thisType->kind = TYPE_REF;
            thisType->name = (Token){0};
            thisType->inner = thisInner;
            thisType->paramTypes = NULL;
            thisType->returnTypes = NULL;
            Parameter* thisParam = malloc(sizeof(Parameter));
            thisParam->name = thisNameTok;
            thisParam->type = thisType;
            listAppend(params, thisParam);
            if (method->params) {
                for (ListNode* p = method->params->head; p != NULL; p = p->next) {
                    listAppend(params, p->data);
                }
            }

            FuncStmt tmp = *method;
            tmp.name = mangledTok;
            tmp.params = params;
            compileFuncStmt(compiler, &tmp);

            free(mangled);
        }
    }
}

void compileEnumStmt(Compiler* compiler, EnumStmt* stmt) {
    if (!compiler || !stmt) return;

    char* enumName = malloc((size_t)stmt->name.length + 1);
    memcpy(enumName, stmt->name.start, (size_t)stmt->name.length);
    enumName[stmt->name.length] = '\0';

    bool sawInt = false;
    bool sawString = false;
    for (ListNode* node = stmt->variants ? stmt->variants->head : NULL; node != NULL; node = node->next) {
        EnumVariantDecl* v = (EnumVariantDecl*)node->data;
        if (!v) continue;
        if (v->valueKind == ENUM_VALUE_INT) sawInt = true;
        if (v->valueKind == ENUM_VALUE_STRING) sawString = true;
    }
    if (sawInt && sawString) {
        error("Enum cannot mix int and string tags: %.*s\n", stmt->name.length, stmt->name.start);
        // keep going best-effort, default to int
        sawString = false;
    }
    bool isStringTag = sawString;

    if (!compilerFindEnum(compiler, stmt->name.start, stmt->name.length)) {
        EnumInfo* info = malloc(sizeof(EnumInfo));
        info->name = enumName;
        info->nameLength = stmt->name.length;
        info->decl = stmt;
        info->isStringTag = isStringTag ? 1 : 0;
        listAppend(compiler->enums, info);
    } else {
        free(enumName);
    }

    // Generate enum helper:
    // - int-tag enum:    Enum__toString(value:int) string
    // - string-tag enum: Enum__toString(value:string) string (identity)
    int helperLen = stmt->name.length + 2 + (int)strlen("toString");
    char* helperName = malloc((size_t)helperLen + 1);
    memcpy(helperName, stmt->name.start, (size_t)stmt->name.length);
    memcpy(helperName + stmt->name.length, "__", 2);
    memcpy(helperName + stmt->name.length + 2, "toString", (size_t)strlen("toString"));
    helperName[helperLen] = '\0';

    if (!LLVMGetNamedFunction(compiler->module, helperName)) {
        LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
        LLVMTypeRef i32 = LLVMInt32TypeInContext(compiler->context);
        LLVMTypeRef paramType = isStringTag ? i8ptr : i32;
        LLVMTypeRef fnType = LLVMFunctionType(i8ptr, &paramType, 1, 0);
        LLVMValueRef fn = LLVMAddFunction(compiler->module, helperName, fnType);

        LLVMBasicBlockRef savedBlock = LLVMGetInsertBlock(compiler->builder);

        LLVMBasicBlockRef entry = LLVMAppendBasicBlock(fn, "entry");
        LLVMPositionBuilderAtEnd(compiler->builder, entry);

        LLVMValueRef valueArg = LLVMGetParam(fn, 0);

        if (isStringTag) {
            LLVMBuildRet(compiler->builder, valueArg);
            LLVMPositionBuilderAtEnd(compiler->builder, savedBlock);
            free(helperName);
            return;
        }

        LLVMBasicBlockRef defBlock = LLVMAppendBasicBlock(fn, "default");
        unsigned variantCount = stmt->variants ? (unsigned)stmt->variants->length : 0;
        LLVMValueRef sw = LLVMBuildSwitch(compiler->builder, valueArg, defBlock, variantCount);

        int current = -1;
        for (unsigned i = 0; i < variantCount; i++) {
            EnumVariantDecl* v = listGet(stmt->variants, (int)i);
            if (!v) continue;

            if (v->valueKind == ENUM_VALUE_INT) {
                char* tmp = malloc((size_t)v->value.length + 1);
                memcpy(tmp, v->value.start, (size_t)v->value.length);
                tmp[v->value.length] = '\0';
                current = (int)strtol(tmp, NULL, 10);
                free(tmp);
            } else {
                current++;
            }

            LLVMBasicBlockRef caseBlock = LLVMAppendBasicBlock(fn, "case");
            LLVMAddCase(sw, LLVMConstInt(i32, (uint64_t)(uint32_t)current, 0), caseBlock);

            LLVMPositionBuilderAtEnd(compiler->builder, caseBlock);
            char* variantName = malloc((size_t)v->name.length + 1);
            memcpy(variantName, v->name.start, (size_t)v->name.length);
            variantName[v->name.length] = '\0';
            LLVMValueRef str = LLVMBuildGlobalStringPtr(compiler->builder, variantName, "enum_variant");
            LLVMBuildRet(compiler->builder, str);
            free(variantName);
        }

        LLVMPositionBuilderAtEnd(compiler->builder, defBlock);
        LLVMValueRef unknown = LLVMBuildGlobalStringPtr(compiler->builder, "Unknown", "enum_unknown");
        LLVMBuildRet(compiler->builder, unknown);

        LLVMPositionBuilderAtEnd(compiler->builder, savedBlock);
    }

    free(helperName);
}

void compileObjectStmt(Compiler* compiler, ObjectStmt* stmt) {
    // Compile object methods as top-level functions with mangled names: Object__method
    if (!stmt || !stmt->methods) return;

    const char* savedObj = compiler ? compiler->currentObjectPrefix : NULL;
    int savedObjLen = compiler ? compiler->currentObjectPrefixLen : 0;
    const char* savedMethod = compiler ? compiler->currentObjectMethodName : NULL;
    int savedMethodLen = compiler ? compiler->currentObjectMethodNameLen : 0;

    for (ListNode* node = stmt->methods->head; node != NULL; node = node->next) {
        FuncStmt* method = (FuncStmt*)node->data;
        if (!method) continue;

        // Allow unqualified calls inside `object` methods to resolve to sibling methods:
        // inside `object O { fn a(){ b() } fn b(){...} }`, `b()` resolves to `O.b()`.
        if (compiler) {
            compiler->currentObjectPrefix = stmt->name.start;
            compiler->currentObjectPrefixLen = stmt->name.length;
            compiler->currentObjectMethodName = method->name.start;
            compiler->currentObjectMethodNameLen = method->name.length;
        }

        int mangledLen = 0;
        char* mangled = mangleTwo(&stmt->name, &method->name, "__", &mangledLen);
        Token mangledTok = method->name;
        mangledTok.start = mangled;
        mangledTok.length = mangledLen;

        FuncStmt tmp = *method;
        tmp.name = mangledTok;
        compileFuncStmt(compiler, &tmp);

        free(mangled);

        if (compiler) {
            compiler->currentObjectPrefix = savedObj;
            compiler->currentObjectPrefixLen = savedObjLen;
            compiler->currentObjectMethodName = savedMethod;
            compiler->currentObjectMethodNameLen = savedMethodLen;
        }
    }
}
