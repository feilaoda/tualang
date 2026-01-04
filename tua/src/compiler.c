
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

typedef struct {
    char* buf;
    int len;
    int cap;
} StrBuf;

static void sbInit(StrBuf* sb, int cap);
static void sbAppendN(StrBuf* sb, const char* s, int n);
static void sbAppendC(StrBuf* sb, char c);
static void sbAppendSanitizedToken(StrBuf* sb, const Token* tok);

static int tokenEqualsCString(const Token* token, const char* s) {
    if (!token || !s) return 0;
    size_t len = strlen(s);
    return token->length == (int)len && memcmp(token->start, s, len) == 0;
}

static int astTypeIsNamedStructValue(Compiler* compiler, Type* t) {
    if (!t) return 0;
    t = compilerResolveGenericType(compiler, t);
    if (!t || t->kind != TYPE_NAMED) return 0;
    // Exclude built-in named types that are pointer-like or special-cased.
    if (t->name.length == 3 && memcmp(t->name.start, "map", 3) == 0) return 0;
    if (t->name.length == 5 && memcmp(t->name.start, "bytes", 5) == 0) return 0;
    if (t->name.length == 6 && memcmp(t->name.start, "Option", 6) == 0) return 0;
    if (t->name.length == 3 && memcmp(t->name.start, "ptr", 3) == 0) return 0;
    if (compilerResolveTraitByToken(compiler, &t->name)) return 0;
    return 1;
}

void initCompiler(Compiler* compiler) {
    compiler->structs = listNew();
    compiler->traits = listNew();
    compiler->enums = listNew();
    compiler->genericFuncTemplates = listNew();
    compiler->genericSubsts = NULL;
    compiler->traitImplPairs = listNew();
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
    compiler->boxedLocals = NULL;
    compiler->lambdaCount = 0;
    compiler->closureType = NULL;
    compiler->mapType = NULL;
    compiler->arrayType = NULL;
    compiler->bytesType = NULL;
    compiler->sliceTypes = listNew();
    compiler->sliceTypeCounter = 0;
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
    compiler->genericInstStack = listNew();
    
    // Debug information
    compiler->hadError = false;
    compiler->panicMode = false;
}

typedef struct {
    LLVMTypeRef elem;
    LLVMTypeRef slice;
} SliceTypeEntry;

static int tokenSetContains(List* set, const char* name, int len) {
    if (!set || !name || len <= 0) return 0;
    for (ListNode* n = set->head; n != NULL; n = n->next) {
        Token* t = (Token*)n->data;
        if (!t) continue;
        if (t->length != len) continue;
        if (memcmp(t->start, name, (size_t)len) == 0) return 1;
    }
    return 0;
}

static void tokenSetAdd(List* set, const char* name, int len) {
    if (!set || !name || len <= 0) return;
    if (tokenSetContains(set, name, len)) return;
    Token* t = (Token*)malloc(sizeof(Token));
    memset(t, 0, sizeof(*t));
    t->type = TOKEN_IDENTIFIER;
    t->start = name;
    t->length = len;
    listAppend(set, t);
}

static void tokenSetFree(List* set) {
    if (!set) return;
    for (ListNode* n = set->head; n != NULL; n = n->next) {
        free(n->data);
    }
    listFree(set);
}

int compilerShouldBoxLocal(Compiler* compiler, const char* name, int nameLen) {
    if (!compiler) return 0;
    if (compiler->boxAllLocals) return 1;
    if (!compiler->boxedLocals) return 0;
    return tokenSetContains(compiler->boxedLocals, name, nameLen);
}

static void compilerPrintGenericInstStack(Compiler* compiler) {
    if (!compiler || !compiler->genericInstStack || compiler->genericInstStack->length <= 0) return;
    // Print from newest to oldest.
    for (int i = compiler->genericInstStack->length - 1; i >= 0; i--) {
        GenericInstFrame* f = (GenericInstFrame*)listGet(compiler->genericInstStack, i);
        if (!f || !f->pretty) continue;
        if (f->file && f->line > 0 && f->col > 0) {
            fprintf(stderr, "note: while instantiating %s at %s:%d:%d\n", f->pretty, f->file, f->line, f->col);
        } else if (f->file && f->line > 0) {
            fprintf(stderr, "note: while instantiating %s at %s:%d\n", f->pretty, f->file, f->line);
        } else if (f->line > 0 && f->col > 0) {
            fprintf(stderr, "note: while instantiating %s at %d:%d\n", f->pretty, f->line, f->col);
        } else if (f->line > 0) {
            fprintf(stderr, "note: while instantiating %s at %d\n", f->pretty, f->line);
        } else {
            fprintf(stderr, "note: while instantiating %s\n", f->pretty);
        }
    }
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
    compilerPrintGenericInstStack(compiler);
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
    compilerPrintGenericInstStack(compiler);
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
    compilerPrintGenericInstStack(compiler);
}

static void sbAppendTypeDisplay(StrBuf* sb, Type* t) {
    if (!sb) return;
    if (!t) {
        sbAppendN(sb, "any", 3);
        return;
    }
    switch (t->kind) {
        case TYPE_BOOL: sbAppendN(sb, "bool", 4); return;
        case TYPE_STRING: sbAppendN(sb, "string", 6); return;
        case TYPE_I8: sbAppendN(sb, "i8", 2); return;
        case TYPE_I16: sbAppendN(sb, "i16", 3); return;
        case TYPE_INT: sbAppendN(sb, "int", 3); return;
        case TYPE_LONG: sbAppendN(sb, "long", 4); return;
        case TYPE_ISIZE: sbAppendN(sb, "isize", 5); return;
        case TYPE_U8: sbAppendN(sb, "u8", 2); return;
        case TYPE_U16: sbAppendN(sb, "u16", 3); return;
        case TYPE_U32: sbAppendN(sb, "u32", 3); return;
        case TYPE_U64: sbAppendN(sb, "u64", 3); return;
        case TYPE_USIZE: sbAppendN(sb, "usize", 5); return;
        case TYPE_BYTE: sbAppendN(sb, "byte", 4); return;
        case TYPE_F16: sbAppendN(sb, "f16", 3); return;
        case TYPE_FLOAT: sbAppendN(sb, "f32", 3); return;
        case TYPE_DOUBLE: sbAppendN(sb, "f64", 3); return;
        case TYPE_PTR: sbAppendN(sb, "ptr", 3); return;
        case TYPE_REF:
            sbAppendN(sb, "Ref<", 4);
            sbAppendTypeDisplay(sb, t->inner);
            sbAppendC(sb, '>');
            return;
        case TYPE_ARRAY:
            sbAppendTypeDisplay(sb, t->inner);
            sbAppendC(sb, '[');
            if (t->arrayLen >= 0) {
                char tmp[32];
                snprintf(tmp, sizeof(tmp), "%lld", (long long)t->arrayLen);
                sbAppendN(sb, tmp, (int)strlen(tmp));
            }
            sbAppendC(sb, ']');
            return;
        case TYPE_FUNC:
            sbAppendN(sb, "fn", 2);
            return;
        case TYPE_NAMED:
            sbAppendSanitizedToken(sb, &t->name);
            if (t->typeArgs && t->typeArgs->length > 0) {
                sbAppendC(sb, '<');
                for (ListNode* n = t->typeArgs->head; n != NULL; n = n->next) {
                    if (n != t->typeArgs->head) sbAppendN(sb, ", ", 2);
                    sbAppendTypeDisplay(sb, (Type*)n->data);
                }
                sbAppendC(sb, '>');
            }
            return;
        default:
            sbAppendN(sb, "any", 3);
            return;
    }
}

static char* formatGenericInstPretty(const char* baseName, int baseNameLen, List* canonArgs) {
    if (!baseName || baseNameLen <= 0) return NULL;
    StrBuf sb;
    sbInit(&sb, baseNameLen + 64);
    sbAppendN(&sb, baseName, baseNameLen);
    sbAppendC(&sb, '<');
    int ac = canonArgs ? canonArgs->length : 0;
    for (int i = 0; i < ac; i++) {
        if (i > 0) sbAppendN(&sb, ", ", 2);
        sbAppendTypeDisplay(&sb, (Type*)listGet(canonArgs, i));
    }
    sbAppendC(&sb, '>');
    return sb.buf;
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

TraitInfo* compilerFindTrait(Compiler* compiler, const char* name, int length) {
    if (!compiler || !compiler->traits) return NULL;
    for (int i = 0; i < compiler->traits->length; i++) {
        TraitInfo* info = listGet(compiler->traits, i);
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

LLVMTypeRef compilerGetBytesType(Compiler* compiler) {
    if (!compiler) return NULL;
    if (compiler->bytesType) return compiler->bytesType;
    LLVMTypeRef t = LLVMGetTypeByName2(compiler->context, "tua_bytes");
    if (!t) t = LLVMStructCreateNamed(compiler->context, "tua_bytes");
    compiler->bytesType = LLVMPointerType(t, 0);
    return compiler->bytesType;
}

LLVMTypeRef compilerGetSliceType(Compiler* compiler, LLVMTypeRef elemType) {
    if (!compiler || !elemType) return NULL;
    if (!compiler->sliceTypes) compiler->sliceTypes = listNew();
    for (int i = 0; i < compiler->sliceTypes->length; i++) {
        SliceTypeEntry* e = (SliceTypeEntry*)listGet(compiler->sliceTypes, i);
        if (e && e->elem == elemType) return e->slice;
    }

    LLVMContextRef ctx = compiler->context;
    LLVMTypeRef i64 = LLVMInt64TypeInContext(ctx);
    LLVMTypeRef elemPtr = LLVMPointerType(elemType, 0);

    char name[64];
    snprintf(name, sizeof(name), "tua_slice$%d", compiler->sliceTypeCounter++);
    LLVMTypeRef st = LLVMStructCreateNamed(ctx, name);
    LLVMTypeRef fields[2] = { elemPtr, i64 };
    LLVMStructSetBody(st, fields, 2, 0);

    LLVMTypeRef sliceTy = st;
    SliceTypeEntry* ent = (SliceTypeEntry*)malloc(sizeof(SliceTypeEntry));
    ent->elem = elemType;
    ent->slice = sliceTy;
    listAppend(compiler->sliceTypes, ent);
    return sliceTy;
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

static LLVMValueRef getOrCreateTuaBytesFree(Compiler* compiler) {
    if (!compiler) return NULL;
    LLVMValueRef fn = LLVMGetNamedFunction(compiler->module, "tua_bytes_free");
    if (fn) return fn;
    LLVMTypeRef bytesType = compilerGetBytesType(compiler);
    LLVMTypeRef params[1] = { bytesType };
    LLVMTypeRef fty = LLVMFunctionType(LLVMVoidTypeInContext(compiler->context), params, 1, 0);
    return LLVMAddFunction(compiler->module, "tua_bytes_free", fty);
}

static LLVMValueRef getOrCreateFree(Compiler* compiler) {
    if (!compiler) return NULL;
    LLVMValueRef fn = LLVMGetNamedFunction(compiler->module, "free");
    if (fn) return fn;
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef params[1] = { i8ptr };
    LLVMTypeRef fty = LLVMFunctionType(LLVMVoidTypeInContext(compiler->context), params, 1, 0);
    return LLVMAddFunction(compiler->module, "free", fty);
}

static LLVMTypeRef tuaBoxDropFnType(Compiler* compiler) {
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef params[1] = { i8ptr };
    return LLVMFunctionType(LLVMVoidTypeInContext(compiler->context), params, 1, 0);
}

static LLVMValueRef getOrCreateTuaBoxAlloc(Compiler* compiler) {
    if (!compiler) return NULL;
    LLVMValueRef fn = LLVMGetNamedFunction(compiler->module, "tua_box_alloc");
    if (fn) return fn;
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef i64 = LLVMInt64TypeInContext(compiler->context);
    LLVMTypeRef dropFnTy = tuaBoxDropFnType(compiler);
    LLVMTypeRef dropFnPtrTy = LLVMPointerType(dropFnTy, 0);
    LLVMTypeRef params[2] = { i64, dropFnPtrTy };
    LLVMTypeRef fty = LLVMFunctionType(i8ptr, params, 2, 0);
    return LLVMAddFunction(compiler->module, "tua_box_alloc", fty);
}

static LLVMValueRef getOrCreateTuaBoxInc(Compiler* compiler) {
    if (!compiler) return NULL;
    LLVMValueRef fn = LLVMGetNamedFunction(compiler->module, "tua_box_inc");
    if (fn) return fn;
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef params[1] = { i8ptr };
    LLVMTypeRef fty = LLVMFunctionType(LLVMVoidTypeInContext(compiler->context), params, 1, 0);
    return LLVMAddFunction(compiler->module, "tua_box_inc", fty);
}

static LLVMValueRef getOrCreateTuaBoxDec(Compiler* compiler) {
    if (!compiler) return NULL;
    LLVMValueRef fn = LLVMGetNamedFunction(compiler->module, "tua_box_dec");
    if (fn) return fn;
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef params[1] = { i8ptr };
    LLVMTypeRef fty = LLVMFunctionType(LLVMVoidTypeInContext(compiler->context), params, 1, 0);
    return LLVMAddFunction(compiler->module, "tua_box_dec", fty);
}

LLVMValueRef compilerGetOrCreateStructDrop(Compiler* compiler, StructInfo* info);
LLVMValueRef compilerGetOrCreateBoxDropFn(
    Compiler* compiler,
    LLVMTypeRef valueType,
    Type* astType,
    int isTraitObj,
    const char* traitName,
    int traitNameLen
);

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
    if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(compiler->builder))) return;

    // Boxed locals/params (closure safety): always release the box storage if owned by this binding.
    if (var.isBoxed && var.boxOwns) {
        if (!var.boxPtrType) return;
        LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
        LLVMValueRef cell = LLVMBuildLoad2(compiler->builder, var.boxPtrType, var.value, "boxp");
        LLVMValueRef decFn = getOrCreateTuaBoxDec(compiler);
        if (decFn) {
            LLVMTypeRef fty = LLVMGlobalGetValueType(decFn);
            LLVMValueRef arg0 = LLVMBuildBitCast(compiler->builder, cell, i8ptr, "boxp_i8");
            LLVMBuildCall2(compiler->builder, fty, decFn, &arg0, 1, "");
        }
        LLVMBuildStore(compiler->builder, LLVMConstNull(var.boxPtrType), var.value);
        return;
    }

    if (var.isBorrowed) return;
    LLVMTypeRef cloTy = compilerGetClosureType(compiler);
    int isClosure = (var.type == cloTy);
    if (!var.isMap && !var.isArray && !var.isBytes && !var.isTraitObj && !isClosure) return;

    LLVMValueRef cur = loadLocalVarValueForDrop(compiler, var, "drop_cur");
    if (!cur) return;

    if (var.isTraitObj) {
        if (!var.traitName || var.traitNameLength <= 0) return;
        TraitInfo* trait = compilerFindTrait(compiler, var.traitName, var.traitNameLength);
        if (!trait) return;
        LLVMTypeRef vtTy = compilerGetTraitVtableType(compiler, trait);
        if (!vtTy) return;
        LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);

        LLVMValueRef vtp = LLVMBuildExtractValue(compiler->builder, cur, 1, "to_vt");
        LLVMValueRef isNull = LLVMBuildIsNull(compiler->builder, vtp, "to_vt_isnull");

        LLVMValueRef fn = LLVMGetBasicBlockParent(LLVMGetInsertBlock(compiler->builder));
        LLVMBasicBlockRef dropBB = LLVMAppendBasicBlock(fn, "to_drop");
        LLVMBasicBlockRef contBB = LLVMAppendBasicBlock(fn, "to_drop_cont");
        LLVMBuildCondBr(compiler->builder, isNull, contBB, dropBB);

        LLVMPositionBuilderAtEnd(compiler->builder, dropBB);
        LLVMValueRef data = LLVMBuildExtractValue(compiler->builder, cur, 0, "to_data");
        LLVMValueRef vtptr = LLVMBuildBitCast(compiler->builder, vtp, LLVMPointerType(vtTy, 0), "vtptr");
        LLVMValueRef dropSlot = LLVMBuildStructGEP2(compiler->builder, vtTy, vtptr, 0, "vt_drop_p");
        LLVMValueRef dropRaw = LLVMBuildLoad2(compiler->builder, i8ptr, dropSlot, "vt_drop");
        LLVMTypeRef dropFnTy = LLVMFunctionType(LLVMVoidTypeInContext(compiler->context), &i8ptr, 1, 0);
        LLVMValueRef dropFn = LLVMBuildBitCast(compiler->builder, dropRaw, LLVMPointerType(dropFnTy, 0), "dropfn");
        LLVMBuildCall2(compiler->builder, dropFnTy, dropFn, &data, 1, "");
        LLVMBuildBr(compiler->builder, contBB);

        LLVMPositionBuilderAtEnd(compiler->builder, contBB);
    } else {
        if (isClosure) {
            LLVMValueRef env = LLVMBuildExtractValue(compiler->builder, cur, 1, "env");
            LLVMValueRef decFn = getOrCreateTuaBoxDec(compiler);
            if (decFn) {
                LLVMTypeRef fty = LLVMGlobalGetValueType(decFn);
                LLVMBuildCall2(compiler->builder, fty, decFn, &env, 1, "");
            }
        } else {
            LLVMValueRef fn =
                var.isArray ? getOrCreateTuaArrayFree(compiler) :
                (var.isBytes ? getOrCreateTuaBytesFree(compiler) : getOrCreateTuaMapFree(compiler));
            if (!fn) return;
            LLVMTypeRef fty = LLVMGlobalGetValueType(fn);
            LLVMValueRef args1[1] = { cur };
            LLVMBuildCall2(compiler->builder, fty, fn, args1, 1, "");
        }
    }

    LLVMValueRef nullv = LLVMConstNull(var.type);
    storeLocalVarValueForDrop(compiler, var, nullv);
}

void compilerEmitDropForBlockVars(Compiler* compiler, Block* block) {
    if (!compiler || !block || !block->variables) return;
    for (ListNode* n = block->variables->head; n != NULL; n = n->next) {
        VariableRef* vr = (VariableRef*)n->data;
        if (!vr) continue;
        emitDropForVar(compiler, *vr);
    }
}

void compilerEmitDropForCurrentFunctionScopes(Compiler* compiler) {
    if (!compiler || !compiler->current || !compiler->current->func) return;
    LLVMValueRef fn = compiler->current->func;
    for (Block* b = compiler->current; b != NULL && b->func == fn; b = b->parent) {
        compilerEmitDropForBlockVars(compiler, b);
    }
}

static void moveOutOnReturnIfNeeded(Compiler* compiler, Expr* e) {
    if (!compiler || !e) return;
    if (e->type != EXPR_VARIABLE) return;
    VariableRef v = findVariableExpr(compiler, e);
    if (!v.value || !v.type) return;
    if (v.isArray && v.isStackArray) return;
    LLVMTypeRef cloTy = compilerGetClosureType(compiler);
    if (!v.isMap && !v.isArray && !v.isBytes && !v.isTraitObj && v.type != cloTy) return;
    LLVMValueRef nullv = LLVMConstNull(v.type);
    storeLocalVarValueForDrop(compiler, v, nullv);
}

LLVMValueRef compilerGetOrCreateStructDrop(Compiler* compiler, StructInfo* info) {
    if (!compiler || !info || !info->name || info->nameLength <= 0 || !info->decl) return NULL;
    // Name: `<Struct>__drop`
    int dropNameLen = info->nameLength + 6; // "__drop"
    char* dropName = malloc((size_t)dropNameLen + 1);
    memcpy(dropName, info->name, (size_t)info->nameLength);
    memcpy(dropName + info->nameLength, "__drop", 6);
    dropName[dropNameLen] = '\0';

    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, dropName);
    if (existing) {
        free(dropName);
        return existing;
    }

    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef selfTy = LLVMPointerType(info->type, 0);
    LLVMTypeRef params[1] = { selfTy };
    LLVMTypeRef fnTy = LLVMFunctionType(LLVMVoidTypeInContext(compiler->context), params, 1, 0);
    LLVMValueRef fn = LLVMAddFunction(compiler->module, dropName, fnTy);
    LLVMSetLinkage(fn, LLVMInternalLinkage);

    // Save insertion point.
    LLVMBasicBlockRef savedBlock = LLVMGetInsertBlock(compiler->builder);
    Block* savedCurrent = compiler->current;

    LLVMBasicBlockRef entry = LLVMAppendBasicBlock(fn, "entry");
    LLVMPositionBuilderAtEnd(compiler->builder, entry);

    LLVMValueRef self = LLVMGetParam(fn, 0);

    // Drop fields best-effort (deep drop of struct/map/array/trait objects).
    for (int i = 0; info->decl->fields && i < info->decl->fields->length; i++) {
        FieldDeclaration* f = (FieldDeclaration*)listGet(info->decl->fields, i);
        if (!f || !f->type) continue;

        // Field pointer
        LLVMValueRef fieldPtr = LLVMBuildStructGEP2(compiler->builder, info->type, self, (unsigned)i, "fptr");

        // map: free handle
        if (f->type->kind == TYPE_NAMED && f->type->name.length == 3 && memcmp(f->type->name.start, "map", 3) == 0) {
            LLVMTypeRef mapTy = compilerGetMapType(compiler);
            LLVMValueRef cur = LLVMBuildLoad2(compiler->builder, mapTy, fieldPtr, "mcur");
            LLVMValueRef freeFn = getOrCreateTuaMapFree(compiler);
            if (freeFn) {
                LLVMTypeRef fty = LLVMGlobalGetValueType(freeFn);
                LLVMValueRef args1[1] = { cur };
                LLVMBuildCall2(compiler->builder, fty, freeFn, args1, 1, "");
            }
            LLVMBuildStore(compiler->builder, LLVMConstNull(mapTy), fieldPtr);
            continue;
        }

        // bytes: free handle
        if (f->type->kind == TYPE_NAMED && f->type->name.length == 5 && memcmp(f->type->name.start, "bytes", 5) == 0) {
            LLVMTypeRef bytesTy = compilerGetBytesType(compiler);
            LLVMValueRef cur = LLVMBuildLoad2(compiler->builder, bytesTy, fieldPtr, "bcur");
            LLVMValueRef freeFn = getOrCreateTuaBytesFree(compiler);
            if (freeFn) {
                LLVMTypeRef fty = LLVMGlobalGetValueType(freeFn);
                LLVMValueRef args1[1] = { cur };
                LLVMBuildCall2(compiler->builder, fty, freeFn, args1, 1, "");
            }
            LLVMBuildStore(compiler->builder, LLVMConstNull(bytesTy), fieldPtr);
            continue;
        }

        // arrays: free handle
        if (f->type->kind == TYPE_ARRAY) {
            LLVMTypeRef arrTy = compilerGetArrayType(compiler);
            LLVMValueRef cur = LLVMBuildLoad2(compiler->builder, arrTy, fieldPtr, "acur");
            LLVMValueRef freeFn = getOrCreateTuaArrayFree(compiler);
            if (freeFn) {
                LLVMTypeRef fty = LLVMGlobalGetValueType(freeFn);
                LLVMValueRef args1[1] = { cur };
                LLVMBuildCall2(compiler->builder, fty, freeFn, args1, 1, "");
            }
            LLVMBuildStore(compiler->builder, LLVMConstNull(arrTy), fieldPtr);
            continue;
        }

        // closure value: release env box
        if (f->type->kind == TYPE_FUNC) {
            LLVMTypeRef cloTy = compilerGetClosureType(compiler);
            LLVMValueRef cur = LLVMBuildLoad2(compiler->builder, cloTy, fieldPtr, "ccur");
            LLVMValueRef env = LLVMBuildExtractValue(compiler->builder, cur, 1, "c_env");
            LLVMValueRef decFn = getOrCreateTuaBoxDec(compiler);
            if (decFn) {
                LLVMTypeRef fty = LLVMGlobalGetValueType(decFn);
                LLVMBuildCall2(compiler->builder, fty, decFn, &env, 1, "");
            }
            LLVMBuildStore(compiler->builder, LLVMConstNull(cloTy), fieldPtr);
            continue;
        }

        // trait object: call vtable.drop(data)
        if (f->type->kind == TYPE_NAMED) {
            TraitInfo* ti = compilerResolveTraitByToken(compiler, &f->type->name);
            if (ti) {
                LLVMTypeRef objTy = compilerGetTraitObjType(compiler, ti);
                LLVMTypeRef vtTy = compilerGetTraitVtableType(compiler, ti);
                LLVMValueRef cur = LLVMBuildLoad2(compiler->builder, objTy, fieldPtr, "tcur");
                LLVMValueRef data = LLVMBuildExtractValue(compiler->builder, cur, 0, "t_data");
                LLVMValueRef vtp = LLVMBuildExtractValue(compiler->builder, cur, 1, "t_vt");
                LLVMValueRef vtptr = LLVMBuildBitCast(compiler->builder, vtp, LLVMPointerType(vtTy, 0), "t_vtptr");
                LLVMValueRef dropSlot = LLVMBuildStructGEP2(compiler->builder, vtTy, vtptr, 0, "t_drop_p");
                LLVMValueRef dropRaw = LLVMBuildLoad2(compiler->builder, i8ptr, dropSlot, "t_drop");
                LLVMTypeRef dropFnTy = LLVMFunctionType(LLVMVoidTypeInContext(compiler->context), &i8ptr, 1, 0);
                LLVMValueRef dropFn = LLVMBuildBitCast(compiler->builder, dropRaw, LLVMPointerType(dropFnTy, 0), "t_dropfn");
                LLVMBuildCall2(compiler->builder, dropFnTy, dropFn, &data, 1, "");
                LLVMBuildStore(compiler->builder, LLVMConstNull(objTy), fieldPtr);
                continue;
            }
        }

        // nested struct: recurse
        if (f->type->kind == TYPE_NAMED) {
            StructInfo* inner = compilerResolveStructByToken(compiler, &f->type->name);
            if (inner) {
                LLVMValueRef dropInner = compilerGetOrCreateStructDrop(compiler, inner);
                if (dropInner) {
                    LLVMTypeRef dropInnerTy = LLVMGlobalGetValueType(dropInner);
                    LLVMValueRef args1[1] = { fieldPtr };
                    LLVMBuildCall2(compiler->builder, dropInnerTy, dropInner, args1, 1, "");
                }
                continue;
            }
        }
    }

    LLVMBuildRetVoid(compiler->builder);

    // Restore insertion point.
    if (savedBlock) LLVMPositionBuilderAtEnd(compiler->builder, savedBlock);
    compiler->current = savedCurrent;

    free(dropName);
    return fn;
}

LLVMValueRef compilerGetOrCreateBoxDropFn(
    Compiler* compiler,
    LLVMTypeRef valueType,
    Type* astType,
    int isTraitObj,
    const char* traitName,
    int traitNameLen
) {
    if (!compiler || !valueType) return NULL;

    LLVMTypeRef mapTy = compilerGetMapType(compiler);
    LLVMTypeRef arrTy = compilerGetArrayType(compiler);
    LLVMTypeRef cloTy = compilerGetClosureType(compiler);

    int isMap = (valueType == mapTy);
    int isArray = (valueType == arrTy);
    int isBytes = 0;
    if (astType && astType->kind == TYPE_NAMED &&
        astType->name.length == 5 && memcmp(astType->name.start, "bytes", 5) == 0) {
        isBytes = 1;
    }
    int isClosure = (valueType == cloTy);
    StructInfo* structInfo = NULL;
    TraitInfo* traitInfo = NULL;

    if (!isMap && !isArray && !isBytes && !isClosure) {
        if (isTraitObj) {
            if (traitName && traitNameLen > 0) {
                traitInfo = compilerFindTrait(compiler, traitName, traitNameLen);
            }
        } else if (astType && astType->kind == TYPE_NAMED) {
            structInfo = compilerResolveStructByToken(compiler, &astType->name);
        } else if (!structInfo && LLVMGetTypeKind(valueType) == LLVMStructTypeKind) {
            const char* llvmName = LLVMGetStructName(valueType);
            if (llvmName && llvmName[0] != '\0') {
                structInfo = compilerFindStruct(compiler, llvmName, (int)strlen(llvmName));
            }
        }
    }

    if (!isMap && !isArray && !isBytes && !isClosure && !structInfo && !traitInfo) return NULL;

    const char* base = NULL;
    int baseLen = 0;
    if (isMap) { base = "map"; baseLen = 3; }
    else if (isArray) { base = "array"; baseLen = 5; }
    else if (isBytes) { base = "bytes"; baseLen = 5; }
    else if (isClosure) { base = "closure"; baseLen = 7; }
    else if (traitInfo) { base = traitInfo->name; baseLen = traitInfo->nameLength; }
    else if (structInfo) { base = structInfo->name; baseLen = structInfo->nameLength; }

    int fnNameLen = 11 + baseLen; // "__boxdrop__" + base
    char* fnName = malloc((size_t)fnNameLen + 1);
    memcpy(fnName, "__boxdrop__", 11);
    memcpy(fnName + 11, base, (size_t)baseLen);
    fnName[fnNameLen] = '\0';

    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, fnName);
    if (existing) {
        free(fnName);
        return existing;
    }

    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef fnTy = tuaBoxDropFnType(compiler);
    LLVMValueRef fn = LLVMAddFunction(compiler->module, fnName, fnTy);
    LLVMSetLinkage(fn, LLVMInternalLinkage);
    free(fnName);

    LLVMBasicBlockRef savedBlock = LLVMGetInsertBlock(compiler->builder);
    Block* savedCurrent = compiler->current;

    LLVMBasicBlockRef entry = LLVMAppendBasicBlock(fn, "entry");
    LLVMPositionBuilderAtEnd(compiler->builder, entry);

    LLVMValueRef payloadI8 = LLVMGetParam(fn, 0);
    LLVMTypeRef payloadPtrTy = LLVMPointerType(valueType, 0);
    LLVMValueRef payloadPtr = LLVMBuildBitCast(compiler->builder, payloadI8, payloadPtrTy, "p");

    if (traitInfo) {
        LLVMTypeRef objTy = compilerGetTraitObjType(compiler, traitInfo);
        LLVMTypeRef vtTy = compilerGetTraitVtableType(compiler, traitInfo);
        LLVMValueRef cur = LLVMBuildLoad2(compiler->builder, objTy, payloadPtr, "tcur");
        LLVMValueRef data = LLVMBuildExtractValue(compiler->builder, cur, 0, "t_data");
        LLVMValueRef vtp = LLVMBuildExtractValue(compiler->builder, cur, 1, "t_vt");
        LLVMValueRef vtptr = LLVMBuildBitCast(compiler->builder, vtp, LLVMPointerType(vtTy, 0), "t_vtptr");
        LLVMValueRef dropSlot = LLVMBuildStructGEP2(compiler->builder, vtTy, vtptr, 0, "t_drop_p");
        LLVMValueRef dropRaw = LLVMBuildLoad2(compiler->builder, i8ptr, dropSlot, "t_drop");
        LLVMTypeRef dropFnTy = LLVMFunctionType(LLVMVoidTypeInContext(compiler->context), &i8ptr, 1, 0);
        LLVMValueRef dropFn = LLVMBuildBitCast(compiler->builder, dropRaw, LLVMPointerType(dropFnTy, 0), "t_dropfn");
        LLVMBuildCall2(compiler->builder, dropFnTy, dropFn, &data, 1, "");
        LLVMBuildStore(compiler->builder, LLVMConstNull(objTy), payloadPtr);
    } else if (isMap) {
        LLVMValueRef cur = LLVMBuildLoad2(compiler->builder, mapTy, payloadPtr, "mcur");
        LLVMValueRef freeFn = getOrCreateTuaMapFree(compiler);
        if (freeFn) {
            LLVMTypeRef fty = LLVMGlobalGetValueType(freeFn);
            LLVMValueRef args1[1] = { cur };
            LLVMBuildCall2(compiler->builder, fty, freeFn, args1, 1, "");
        }
        LLVMBuildStore(compiler->builder, LLVMConstNull(mapTy), payloadPtr);
    } else if (isArray) {
        LLVMValueRef cur = LLVMBuildLoad2(compiler->builder, arrTy, payloadPtr, "acur");
        LLVMValueRef freeFn = getOrCreateTuaArrayFree(compiler);
        if (freeFn) {
            LLVMTypeRef fty = LLVMGlobalGetValueType(freeFn);
            LLVMValueRef args1[1] = { cur };
            LLVMBuildCall2(compiler->builder, fty, freeFn, args1, 1, "");
        }
        LLVMBuildStore(compiler->builder, LLVMConstNull(arrTy), payloadPtr);
    } else if (isBytes) {
        LLVMTypeRef bytesTy = compilerGetBytesType(compiler);
        LLVMValueRef cur = LLVMBuildLoad2(compiler->builder, bytesTy, payloadPtr, "bcur");
        LLVMValueRef freeFn = getOrCreateTuaBytesFree(compiler);
        if (freeFn) {
            LLVMTypeRef fty = LLVMGlobalGetValueType(freeFn);
            LLVMValueRef args1[1] = { cur };
            LLVMBuildCall2(compiler->builder, fty, freeFn, args1, 1, "");
        }
        LLVMBuildStore(compiler->builder, LLVMConstNull(bytesTy), payloadPtr);
    } else if (isClosure) {
        LLVMValueRef cur = LLVMBuildLoad2(compiler->builder, cloTy, payloadPtr, "ccur");
        LLVMValueRef env = LLVMBuildExtractValue(compiler->builder, cur, 1, "env");
        LLVMValueRef decFn = getOrCreateTuaBoxDec(compiler);
        if (decFn) {
            LLVMTypeRef fty = LLVMGlobalGetValueType(decFn);
            LLVMValueRef arg0 = env;
            LLVMBuildCall2(compiler->builder, fty, decFn, &arg0, 1, "");
        }
        LLVMBuildStore(compiler->builder, LLVMConstNull(cloTy), payloadPtr);
    } else if (structInfo) {
        LLVMValueRef dropFn = compilerGetOrCreateStructDrop(compiler, structInfo);
        if (dropFn) {
            LLVMTypeRef dropTy = LLVMGlobalGetValueType(dropFn);
            LLVMValueRef args1[1] = { payloadPtr };
            LLVMBuildCall2(compiler->builder, dropTy, dropFn, args1, 1, "");
        }
        LLVMBuildStore(compiler->builder, LLVMConstNull(valueType), payloadPtr);
    }

    LLVMBuildRetVoid(compiler->builder);

    if (savedBlock) LLVMPositionBuilderAtEnd(compiler->builder, savedBlock);
    compiler->current = savedCurrent;
    return fn;
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

TraitInfo* compilerResolveTraitByToken(Compiler* compiler, const Token* name) {
    if (!compiler || !name) return NULL;
    SymbolAlias* a = compilerFindAlias(compiler, name->start, name->length);
    if (a && a->kind == ALIAS_TRAIT) {
        return compilerFindTrait(compiler, a->qualified, a->qualifiedLen);
    }
    if (compiler && compiler->currentModulePrefix) {
        int ql = 0;
        char* q = compilerQualifyToken(compiler, name, &ql);
        if (q) {
            TraitInfo* info = compilerFindTrait(compiler, q, ql);
            free(q);
            if (info) return info;
        }
    }
    return compilerFindTrait(compiler, name->start, name->length);
}

LLVMTypeRef compilerGetTraitObjType(Compiler* compiler, TraitInfo* trait) {
    if (!compiler || !trait) return NULL;
    if (trait->objType) return trait->objType;
    // Create a per-trait named object type so we can recover the trait from LLVM types in codegen.
    int len = trait->nameLength + 5; // "__obj"
    char* tn = malloc((size_t)len + 1);
    memcpy(tn, trait->name, (size_t)trait->nameLength);
    memcpy(tn + trait->nameLength, "__obj", 5);
    tn[len] = '\0';
    LLVMTypeRef t = LLVMGetTypeByName2(compiler->context, tn);
    if (!t) t = LLVMStructCreateNamed(compiler->context, tn);
    // Layout: { i8* data, i8* vtable } (opaque vtable ptr; entries are i8*).
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef fields[2] = { i8ptr, i8ptr };
    LLVMStructSetBody(t, fields, 2, 0);
    trait->objType = t;
    free(tn);
    return t;
}

LLVMTypeRef compilerGetTraitVtableType(Compiler* compiler, TraitInfo* trait) {
    if (!compiler || !trait) return NULL;
    if (trait->vtableType) return trait->vtableType;
    if (!trait->decl) return NULL;

    int len = trait->nameLength + 8; // "__vtable"
    char* tn = malloc((size_t)len + 1);
    memcpy(tn, trait->name, (size_t)trait->nameLength);
    memcpy(tn + trait->nameLength, "__vtable", 8);
    tn[len] = '\0';

    LLVMTypeRef t = LLVMGetTypeByName2(compiler->context, tn);
    if (!t) t = LLVMStructCreateNamed(compiler->context, tn);

    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    int mc = trait->decl->methods ? trait->decl->methods->length : 0;
    int fc = 1 + mc; // drop + methods
    LLVMTypeRef* fields = malloc(sizeof(LLVMTypeRef) * (size_t)fc);
    for (int i = 0; i < fc; i++) fields[i] = i8ptr;
    LLVMStructSetBody(t, fields, (unsigned)fc, 0);
    free(fields);

    trait->vtableType = t;
    free(tn);
    return t;
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

GenericFuncTemplate* compilerFindGenericFuncTemplate(Compiler* compiler, const char* qualifiedName, int qualifiedNameLen) {
    if (!compiler || !compiler->genericFuncTemplates || !qualifiedName || qualifiedNameLen <= 0) return NULL;
    for (int i = 0; i < compiler->genericFuncTemplates->length; i++) {
        GenericFuncTemplate* g = listGet(compiler->genericFuncTemplates, i);
        if (!g) continue;
        if (g->qualifiedNameLen != qualifiedNameLen) continue;
        if (memcmp(g->qualifiedName, qualifiedName, (size_t)qualifiedNameLen) == 0) return g;
    }
    return NULL;
}

void compilerRegisterGenericFuncTemplate(
    Compiler* compiler,
    FuncStmt* decl,
    const char* qualifiedName,
    int qualifiedNameLen,
    const char* filePath,
    const char* modulePrefix,
    int modulePrefixLen,
    List* aliases
) {
    if (!compiler || !decl || !qualifiedName || qualifiedNameLen <= 0) return;
    if (!decl->typeParams || decl->typeParams->length <= 0) return;
    if (!decl->body) return;

    if (!compiler->genericFuncTemplates) compiler->genericFuncTemplates = listNew();
    if (compilerFindGenericFuncTemplate(compiler, qualifiedName, qualifiedNameLen)) {
        compilerErrorAtToken(compiler, &decl->name, "duplicate generic function template: %.*s", decl->name.length, decl->name.start);
        return;
    }

    GenericFuncTemplate* g = malloc(sizeof(GenericFuncTemplate));
    memset(g, 0, sizeof(*g));
    g->qualifiedName = malloc((size_t)qualifiedNameLen + 1);
    memcpy(g->qualifiedName, qualifiedName, (size_t)qualifiedNameLen);
    g->qualifiedName[qualifiedNameLen] = '\0';
    g->qualifiedNameLen = qualifiedNameLen;
    g->decl = decl;
    g->filePath = filePath;
    g->modulePrefix = modulePrefix;
    g->modulePrefixLen = modulePrefixLen;
    g->aliases = aliases;
    listAppend(compiler->genericFuncTemplates, g);
}

Type* compilerResolveGenericType(Compiler* compiler, Type* type) {
    if (!compiler || !compiler->genericSubsts || !type) return type;
    if (type->kind != TYPE_NAMED) return type;
    if (type->typeArgs && type->typeArgs->length > 0) return type;
    // Builtins are never generic type params.
    if (type->name.length == 3 && memcmp(type->name.start, "map", 3) == 0) return type;
    if (type->name.length == 6 && memcmp(type->name.start, "Option", 6) == 0) return type;
    if (type->name.length == 3 && memcmp(type->name.start, "ptr", 3) == 0) return type;

    for (ListNode* n = compiler->genericSubsts->head; n != NULL; n = n->next) {
        GenericSubst* s = (GenericSubst*)n->data;
        if (!s || !s->name || s->nameLen <= 0 || !s->type) continue;
        if (s->nameLen != type->name.length) continue;
        if (memcmp(s->name, type->name.start, (size_t)s->nameLen) == 0) {
            return s->type;
        }
    }
    return type;
}

GenericSubst* compilerFindGenericSubst(Compiler* compiler, const char* name, int nameLen) {
    if (!compiler || !compiler->genericSubsts || !name || nameLen <= 0) return NULL;
    for (ListNode* n = compiler->genericSubsts->head; n != NULL; n = n->next) {
        GenericSubst* s = (GenericSubst*)n->data;
        if (!s || !s->name || s->nameLen <= 0) continue;
        if (s->nameLen != nameLen) continue;
        if (memcmp(s->name, name, (size_t)nameLen) == 0) return s;
    }
    return NULL;
}

static LLVMTypeRef typeToLLVMType(Compiler* compiler, Type* type, bool defaultToVoid) {
    if (type == NULL) {
        return defaultToVoid ? LLVMVoidTypeInContext(compiler->context)
                             : LLVMInt32TypeInContext(compiler->context);
    }

    Type* subst = compilerResolveGenericType(compiler, type);
    if (subst && subst != type) {
        return typeToLLVMType(compiler, subst, defaultToVoid);
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
            TraitInfo* ti = compilerResolveTraitByToken(compiler, &type->name);
            if (ti) {
                return compilerGetTraitObjType(compiler, ti);
            }
            if (type->name.length == 3 && memcmp(type->name.start, "ptr", 3) == 0) {
                return LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
            }
            if (type->name.length == 3 && memcmp(type->name.start, "map", 3) == 0) {
                return compilerGetMapType(compiler);
            }
            if (type->name.length == 5 && memcmp(type->name.start, "bytes", 5) == 0) {
                return compilerGetBytesType(compiler);
            }
            if (type->name.length == 5 && memcmp(type->name.start, "Slice", 5) == 0) {
                Type* inner = NULL;
                if (type->typeArgs && type->typeArgs->length == 1) {
                    inner = (Type*)type->typeArgs->head->data;
                }
                LLVMTypeRef innerTy = inner ? typeToLLVMType(compiler, inner, false) : LLVMInt8TypeInContext(compiler->context);
                return compilerGetSliceType(compiler, innerTy);
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

static int isBuiltinNamedTypeToken(const Token* name) {
    if (!name) return 0;
    if (name->length == 3 && memcmp(name->start, "map", 3) == 0) return 1;
    if (name->length == 6 && memcmp(name->start, "Option", 6) == 0) return 1;
    if (name->length == 3 && memcmp(name->start, "ptr", 3) == 0) return 1;
    if (name->length == 5 && memcmp(name->start, "bytes", 5) == 0) return 1;
    if (name->length == 5 && memcmp(name->start, "Slice", 5) == 0) return 1;
    return 0;
}

static Type* canonicalizeTypeForGenericArg(Compiler* compiler, Type* t) {
    if (!t) return NULL;
    Type* src = compilerResolveGenericType(compiler, t);
    if (!src) return NULL;

    Type* out = malloc(sizeof(Type));
    memset(out, 0, sizeof(*out));
    out->kind = src->kind;
    out->name = src->name;
    out->inner = NULL;
    out->typeArgs = NULL;
    out->paramTypes = NULL;
    out->returnTypes = NULL;
    out->arrayLen = src->arrayLen;

    switch (src->kind) {
        case TYPE_REF:
            out->inner = canonicalizeTypeForGenericArg(compiler, src->inner);
            return out;
        case TYPE_ARRAY:
            out->inner = canonicalizeTypeForGenericArg(compiler, src->inner);
            return out;
        case TYPE_FUNC: {
            // v0: allow function types as type args in syntax, but don't attempt deep encoding yet.
            // Keep them structurally canonicalized for future use.
            List* ps = listNew();
            int pc = src->paramTypes ? src->paramTypes->length : 0;
            for (int i = 0; i < pc; i++) {
                Type* pt = listGet(src->paramTypes, i);
                listAppend(ps, canonicalizeTypeForGenericArg(compiler, pt));
            }
            List* rs = listNew();
            int rc = src->returnTypes ? src->returnTypes->length : 0;
            for (int i = 0; i < rc; i++) {
                Type* rt = listGet(src->returnTypes, i);
                listAppend(rs, canonicalizeTypeForGenericArg(compiler, rt));
            }
            out->paramTypes = ps;
            out->returnTypes = rs;
            return out;
        }
        case TYPE_NAMED: {
            // Canonicalize named struct/enum types to their qualified names so instantiation
            // does not depend on the template module's import context.
            if (!isBuiltinNamedTypeToken(&src->name)) {
                StructInfo* si = compilerResolveStructByToken(compiler, &src->name);
                if (si) {
                    out->name.start = si->name;
                    out->name.length = si->nameLength;
                } else {
                    EnumInfo* ei = compilerResolveEnumByToken(compiler, &src->name);
                    if (ei) {
                        out->name.start = ei->name;
                        out->name.length = ei->nameLength;
                    } else {
                        // Best-effort: qualify local names even if the struct/enum hasn't been compiled yet.
                        SymbolAlias* a = compilerFindAlias(compiler, src->name.start, src->name.length);
                        if (a && (a->kind == ALIAS_STRUCT || a->kind == ALIAS_ENUM)) {
                            out->name.start = a->qualified;
                            out->name.length = a->qualifiedLen;
                        } else if (compiler->currentModulePrefix) {
                            int ql = 0;
                            char* q = compilerQualifyToken(compiler, &src->name, &ql);
                            if (q) {
                                out->name.start = q; // keep allocated; caller-owned (freed with type tree)
                                out->name.length = ql;
                            }
                        }
                    }
                }
            }
            if (src->typeArgs && src->typeArgs->length > 0) {
                List* args = listNew();
                for (ListNode* n = src->typeArgs->head; n != NULL; n = n->next) {
                    listAppend(args, canonicalizeTypeForGenericArg(compiler, (Type*)n->data));
                }
                out->typeArgs = args;
            }
            return out;
        }
        default:
            return out;
    }
}

int compilerHasTraitImplPair(Compiler* compiler, const char* traitName, int traitLen, const char* targetName, int targetLen) {
    if (!compiler || !compiler->traitImplPairs || !traitName || !targetName) return 0;
    for (ListNode* n = compiler->traitImplPairs->head; n != NULL; n = n->next) {
        TraitImplPair* p = (TraitImplPair*)n->data;
        if (!p) continue;
        if (p->traitNameLen != traitLen) continue;
        if (p->targetNameLen != targetLen) continue;
        if (memcmp(p->traitName, traitName, (size_t)traitLen) != 0) continue;
        if (memcmp(p->targetName, targetName, (size_t)targetLen) != 0) continue;
        return 1;
    }
    return 0;
}

void compilerRecordTraitImplPair(Compiler* compiler, const char* traitName, int traitLen, const char* targetName, int targetLen) {
    if (!compiler) return;
    if (!compiler->traitImplPairs) compiler->traitImplPairs = listNew();
    if (!traitName || !targetName || traitLen <= 0 || targetLen <= 0) return;
    if (compilerHasTraitImplPair(compiler, traitName, traitLen, targetName, targetLen)) return;

    TraitImplPair* p = malloc(sizeof(TraitImplPair));
    p->traitName = malloc((size_t)traitLen + 1);
    memcpy(p->traitName, traitName, (size_t)traitLen);
    p->traitName[traitLen] = '\0';
    p->traitNameLen = traitLen;
    p->targetName = malloc((size_t)targetLen + 1);
    memcpy(p->targetName, targetName, (size_t)targetLen);
    p->targetName[targetLen] = '\0';
    p->targetNameLen = targetLen;
    listAppend(compiler->traitImplPairs, p);
}

static void sbInit(StrBuf* sb, int cap) {
    sb->len = 0;
    sb->cap = cap > 0 ? cap : 64;
    sb->buf = malloc((size_t)sb->cap);
    sb->buf[0] = '\0';
}

static void sbEnsure(StrBuf* sb, int add) {
    if (sb->len + add + 1 <= sb->cap) return;
    int nc = sb->cap * 2;
    while (sb->len + add + 1 > nc) nc *= 2;
    sb->buf = realloc(sb->buf, (size_t)nc);
    sb->cap = nc;
}

static void sbAppendN(StrBuf* sb, const char* s, int n) {
    if (!s || n <= 0) return;
    sbEnsure(sb, n);
    memcpy(sb->buf + sb->len, s, (size_t)n);
    sb->len += n;
    sb->buf[sb->len] = '\0';
}

static void sbAppendC(StrBuf* sb, char c) {
    sbEnsure(sb, 1);
    sb->buf[sb->len++] = c;
    sb->buf[sb->len] = '\0';
}

static void sbAppendSanitizedToken(StrBuf* sb, const Token* tok) {
    if (!tok || !tok->start || tok->length <= 0) return;
    for (int i = 0; i < tok->length; i++) {
        char c = tok->start[i];
        int ok =
            (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            (c == '_');
        sbAppendC(sb, ok ? c : '_');
    }
}

static void sbAppendTypeMangle(StrBuf* sb, Type* t) {
    if (!sb) return;
    if (!t) {
        sbAppendN(sb, "any", 3);
        return;
    }
    switch (t->kind) {
        case TYPE_BOOL: sbAppendN(sb, "bool", 4); return;
        case TYPE_STRING: sbAppendN(sb, "string", 6); return;
        case TYPE_I8: sbAppendN(sb, "i8", 2); return;
        case TYPE_I16: sbAppendN(sb, "i16", 3); return;
        case TYPE_INT: sbAppendN(sb, "int", 3); return;
        case TYPE_LONG: sbAppendN(sb, "long", 4); return;
        case TYPE_ISIZE: sbAppendN(sb, "isize", 5); return;
        case TYPE_U8: sbAppendN(sb, "u8", 2); return;
        case TYPE_U16: sbAppendN(sb, "u16", 3); return;
        case TYPE_U32: sbAppendN(sb, "u32", 3); return;
        case TYPE_U64: sbAppendN(sb, "u64", 3); return;
        case TYPE_USIZE: sbAppendN(sb, "usize", 5); return;
        case TYPE_BYTE: sbAppendN(sb, "byte", 4); return;
        case TYPE_F16: sbAppendN(sb, "f16", 3); return;
        case TYPE_FLOAT: sbAppendN(sb, "f32", 3); return;
        case TYPE_DOUBLE: sbAppendN(sb, "f64", 3); return;
        case TYPE_PTR: sbAppendN(sb, "ptr", 3); return;
        case TYPE_REF:
            sbAppendN(sb, "Ref_", 4);
            sbAppendTypeMangle(sb, t->inner);
            return;
        case TYPE_ARRAY:
            sbAppendN(sb, "Arr_", 4);
            sbAppendTypeMangle(sb, t->inner);
            if (t->arrayLen < 0) {
                sbAppendN(sb, "_dyn", 4);
            } else {
                sbAppendN(sb, "_N", 2);
                char tmp[32];
                snprintf(tmp, sizeof(tmp), "%lld", (long long)t->arrayLen);
                sbAppendN(sb, tmp, (int)strlen(tmp));
            }
            return;
        case TYPE_FUNC:
            // v0: keep coarse encoding to avoid huge symbol names.
            sbAppendN(sb, "Fn", 2);
            return;
        case TYPE_NAMED:
            sbAppendSanitizedToken(sb, &t->name);
            if (t->typeArgs && t->typeArgs->length > 0) {
                for (ListNode* n = t->typeArgs->head; n != NULL; n = n->next) {
                    sbAppendC(sb, '_');
                    sbAppendTypeMangle(sb, (Type*)n->data);
                }
            }
            return;
        default:
            sbAppendN(sb, "any", 3);
            return;
    }
}

static char* mangleGenericInstanceName(const char* baseName, int baseNameLen, List* canonArgs) {
    StrBuf sb;
    sbInit(&sb, baseNameLen + 64);
    sbAppendN(&sb, baseName, baseNameLen);
    sbAppendN(&sb, "__G__", 5);
    int ac = canonArgs ? canonArgs->length : 0;
    for (int i = 0; i < ac; i++) {
        if (i > 0) sbAppendC(&sb, '_');
        sbAppendTypeMangle(&sb, (Type*)listGet(canonArgs, i));
    }
    return sb.buf;
}

LLVMValueRef compilerInstantiateGenericFunc(Compiler* compiler, const char* baseName, int baseNameLen, List* typeArgs, const Token* callSite) {
    if (!compiler || !baseName || baseNameLen <= 0) return NULL;
    GenericFuncTemplate* tmpl = compilerFindGenericFuncTemplate(compiler, baseName, baseNameLen);
    if (!tmpl || !tmpl->decl) return NULL;

    int expected = tmpl->decl->typeParams ? tmpl->decl->typeParams->length : 0;
    int got = typeArgs ? typeArgs->length : 0;
    if (got != expected) {
        const Token* tok = callSite ? callSite : &tmpl->decl->name;
        compilerErrorAtToken(
            compiler,
            tok,
            "generic type argument count mismatch for '%.*s': expected %d, got %d",
            tok ? tok->length : 0,
            tok ? tok->start : "",
            expected,
            got
        );
        return NULL;
    }

    // Before doing any potentially expensive canonicalization/mangling, guard against runaway
    // recursive instantiation that would otherwise blow up symbol names or crash the compiler.
    if (compiler->genericInstStack && compiler->genericInstStack->length >= TUA_GENERIC_INST_MAX_DEPTH) {
        const Token* tok = callSite ? callSite : &tmpl->decl->name;
        compilerErrorAtToken(
            compiler,
            tok,
            "generic instantiation depth limit (%d) exceeded",
            (int)TUA_GENERIC_INST_MAX_DEPTH
        );
        return NULL;
    }

    List* canon = listNew();
    for (int i = 0; i < got; i++) {
        Type* a = listGet(typeArgs, i);
        Type* ca = canonicalizeTypeForGenericArg(compiler, a);
        listAppend(canon, ca);
    }

    char* instName = mangleGenericInstanceName(baseName, baseNameLen, canon);
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, instName);
    if (existing) {
        free(instName);
        return existing;
    }

    char* pretty = formatGenericInstPretty(baseName, baseNameLen, canon);

    // Detect recursive instantiation cycles of the same concrete instance.
    if (compiler && compiler->genericInstStack && pretty) {
        for (int i = 0; i < compiler->genericInstStack->length; i++) {
            GenericInstFrame* f = (GenericInstFrame*)listGet(compiler->genericInstStack, i);
            if (!f || !f->pretty) continue;
            if (strcmp(f->pretty, pretty) == 0) {
                const Token* tok = callSite ? callSite : &tmpl->decl->name;
                compilerErrorAtToken(
                    compiler,
                    tok,
                    "recursive generic instantiation detected for %s",
                    pretty
                );
                free(instName);
                free(pretty);
                return NULL;
            }
        }
    }

    // Push diagnostic frame at the call site (before switching module context).
    int pushedDiagFrame = 0;
    if (compiler && compiler->genericInstStack && pretty) {
        GenericInstFrame* fr = (GenericInstFrame*)calloc(1, sizeof(GenericInstFrame));
        fr->file = compiler->currentFilePath;
        fr->line = callSite ? callSite->line : 0;
        fr->col = callSite ? callSite->col : 0;
        fr->pretty = pretty;
        listAppend(compiler->genericInstStack, fr);
        pushedDiagFrame = 1;
        pretty = NULL; // owned by frame
    }
    if (pretty) free(pretty);

    // Switch compiler context to the template's module for correct name resolution in the body.
    const char* savedFile = compiler->currentFilePath;
    const char* savedPrefix = compiler->currentModulePrefix;
    int savedPrefixLen = compiler->currentModulePrefixLen;
    List* savedAliases = compiler->currentAliases;
    List* savedSubsts = compiler->genericSubsts;

    compiler->currentFilePath = tmpl->filePath;
    compiler->currentModulePrefix = tmpl->modulePrefix;
    compiler->currentModulePrefixLen = tmpl->modulePrefixLen;
    compiler->currentAliases = tmpl->aliases;

    compiler->genericSubsts = listNew();
    for (int i = 0; i < expected; i++) {
        TypeParamDecl* tp = (TypeParamDecl*)listGet(tmpl->decl->typeParams, i);
        Type* ca = (Type*)listGet(canon, i);
        if (!tp || !ca) continue;
        GenericSubst* s = malloc(sizeof(GenericSubst));
        memset(s, 0, sizeof(*s));
        s->name = tp->name.start;
        s->nameLen = tp->name.length;
        s->type = ca;
        s->boundTraitName = NULL;
        s->boundTraitNameLen = 0;
        if (tp->hasBound) {
            TraitInfo* ti = compilerResolveTraitByToken(compiler, &tp->boundTrait);
            if (!ti) {
                const Token* tok = callSite ? callSite : &tmpl->decl->name;
                compilerErrorAtToken(
                    compiler,
                    tok,
                    "unknown trait in generic bound: %.*s",
                    tp->boundTrait.length,
                    tp->boundTrait.start
                );
            } else {
                s->boundTraitName = ti->name;
                s->boundTraitNameLen = ti->nameLength;
            }
        }
        listAppend(compiler->genericSubsts, s);
    }

    // v1 bounds: verify `T: Trait` constraints using declared `impl Trait for Struct` pairs.
    for (int i = 0; i < expected; i++) {
        TypeParamDecl* tp = (TypeParamDecl*)listGet(tmpl->decl->typeParams, i);
        Type* ca = (Type*)listGet(canon, i);
        if (!tp || !tp->hasBound || !ca) continue;

        if (ca->kind != TYPE_NAMED || (ca->typeArgs && ca->typeArgs->length > 0)) {
            const Token* tok = callSite ? callSite : &tmpl->decl->name;
            compilerErrorAtToken(
                compiler,
                tok,
                "generic bound '%.*s: %.*s' requires a concrete named struct type",
                tp->name.length, tp->name.start,
                tp->boundTrait.length, tp->boundTrait.start
            );
            continue;
        }

        GenericSubst* s = compilerFindGenericSubst(compiler, tp->name.start, tp->name.length);
        const char* traitQ = s ? s->boundTraitName : NULL;
        int traitQL = s ? s->boundTraitNameLen : 0;
        if (!traitQ || traitQL <= 0) continue;

        // Canonicalized type args use qualified names where possible.
        const char* targetQ = ca->name.start;
        int targetQL = ca->name.length;

        if (!compilerHasTraitImplPair(compiler, traitQ, traitQL, targetQ, targetQL)) {
            const Token* tok = callSite ? callSite : &tmpl->decl->name;
            compilerErrorAtToken(
                compiler,
                tok,
                "generic bound not satisfied: '%.*s' does not implement '%.*s' (missing `impl %.*s for %.*s {}`)",
                targetQL, targetQ,
                traitQL, traitQ,
                traitQL, traitQ,
                targetQL, targetQ
            );
        }
    }

    if (compiler->hadError) {
        compiler->genericSubsts = savedSubsts;
        compiler->currentFilePath = savedFile;
        compiler->currentModulePrefix = savedPrefix;
        compiler->currentModulePrefixLen = savedPrefixLen;
        compiler->currentAliases = savedAliases;
        free(instName);

        if (pushedDiagFrame && compiler && compiler->genericInstStack) {
            GenericInstFrame* fr = (GenericInstFrame*)listPop(compiler->genericInstStack);
            if (fr) {
                if (fr->pretty) free(fr->pretty);
                free(fr);
            }
        }
        return NULL;
    }

    // Compile monomorphized instance as a normal function under `instName`.
    FuncStmt tmp = *tmpl->decl;
    Token nt = tmp.name;
    nt.start = instName;
    nt.length = (int)strlen(instName);
    tmp.name = nt;
    tmp.typeParams = NULL;
    compileFuncStmt(compiler, &tmp);

    // Restore context.
    compiler->genericSubsts = savedSubsts;
    compiler->currentFilePath = savedFile;
    compiler->currentModulePrefix = savedPrefix;
    compiler->currentModulePrefixLen = savedPrefixLen;
    compiler->currentAliases = savedAliases;

    LLVMValueRef fn = LLVMGetNamedFunction(compiler->module, instName);
    free(instName);

    // Pop diagnostic frame.
    if (compiler && compiler->genericInstStack) {
        GenericInstFrame* fr = (GenericInstFrame*)listPop(compiler->genericInstStack);
        if (fr) {
            if (fr->pretty) free(fr->pretty);
            free(fr);
        }
    }

    return fn;
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

static void collectTopLevelLambdasExpr(List* lambdas, Expr* e);
static void collectTopLevelLambdasStmt(List* lambdas, Stmt* s) {
    if (!lambdas || !s) return;
    if (s->type == STMT_PRIVATE) {
        collectTopLevelLambdasStmt(lambdas, ((PrivateStmt*)s)->inner);
        return;
    }
    // Do not traverse into nested named function declarations; they are compiled separately.
    if (s->type == STMT_FUNC) return;

    switch (s->type) {
        case STMT_VAR:
            collectTopLevelLambdasExpr(lambdas, ((VarStmt*)s)->initializer);
            break;
        case STMT_DESTRUCTURE:
            collectTopLevelLambdasExpr(lambdas, ((DestructureStmt*)s)->value);
            break;
        case STMT_EXPR:
            collectTopLevelLambdasExpr(lambdas, ((ExprStmt*)s)->expression);
            break;
        case STMT_RETURN: {
            ReturnStmt* r = (ReturnStmt*)s;
            if (r->values) {
                for (ListNode* n = r->values->head; n != NULL; n = n->next) {
                    collectTopLevelLambdasExpr(lambdas, (Expr*)n->data);
                }
            } else {
                collectTopLevelLambdasExpr(lambdas, r->value);
            }
            break;
        }
        case STMT_BLOCK: {
            BlockStmt* b = (BlockStmt*)s;
            for (ListNode* n = b->statements ? b->statements->head : NULL; n != NULL; n = n->next) {
                collectTopLevelLambdasStmt(lambdas, (Stmt*)n->data);
            }
            break;
        }
        case STMT_IF: {
            IfStmt* i = (IfStmt*)s;
            collectTopLevelLambdasExpr(lambdas, i->condition);
            collectTopLevelLambdasStmt(lambdas, i->thenBranch);
            collectTopLevelLambdasStmt(lambdas, i->elseBranch);
            break;
        }
        case STMT_FOR: {
            ForStmt* f = (ForStmt*)s;
            collectTopLevelLambdasStmt(lambdas, f->initializer);
            collectTopLevelLambdasExpr(lambdas, f->condition);
            collectTopLevelLambdasExpr(lambdas, f->increment);
            collectTopLevelLambdasStmt(lambdas, f->body);
            break;
        }
        case STMT_FOR_IN: {
            ForInStmt* fi = (ForInStmt*)s;
            collectTopLevelLambdasExpr(lambdas, fi->range);
            collectTopLevelLambdasStmt(lambdas, fi->body);
            break;
        }
        case STMT_WHILE: {
            WhileStmt* w = (WhileStmt*)s;
            collectTopLevelLambdasExpr(lambdas, w->condition);
            collectTopLevelLambdasStmt(lambdas, w->body);
            break;
        }
        case STMT_DO_WHILE: {
            DoWhileStmt* dw = (DoWhileStmt*)s;
            collectTopLevelLambdasStmt(lambdas, dw->body);
            collectTopLevelLambdasExpr(lambdas, dw->condition);
            break;
        }
        default:
            break;
    }
}

static void collectTopLevelLambdasExpr(List* lambdas, Expr* e) {
    if (!lambdas || !e) return;
    if (e->type == EXPR_LAMBDA) {
        listAppend(lambdas, e);
        return; // stop at nested lambda; captured vars will be handled transitively by this lambda's free set
    }
    switch (e->type) {
        case EXPR_BINARY: {
            BinaryExpr* b = (BinaryExpr*)e;
            collectTopLevelLambdasExpr(lambdas, b->left);
            collectTopLevelLambdasExpr(lambdas, b->right);
            break;
        }
        case EXPR_UNARY:
            collectTopLevelLambdasExpr(lambdas, ((UnaryExpr*)e)->right);
            break;
        case EXPR_GROUPING:
            collectTopLevelLambdasExpr(lambdas, ((GroupingExpr*)e)->expression);
            break;
        case EXPR_CALL: {
            CallExpr* c = (CallExpr*)e;
            collectTopLevelLambdasExpr(lambdas, c->callee);
            for (ListNode* n = c->arguments ? c->arguments->head : NULL; n != NULL; n = n->next) {
                collectTopLevelLambdasExpr(lambdas, (Expr*)n->data);
            }
            break;
        }
        case EXPR_ASSIGN:
            collectTopLevelLambdasExpr(lambdas, ((AssignExpr*)e)->value);
            break;
        case EXPR_GET:
            collectTopLevelLambdasExpr(lambdas, ((GetExpr*)e)->object);
            break;
        case EXPR_SET: {
            SetExpr* s = (SetExpr*)e;
            collectTopLevelLambdasExpr(lambdas, s->object);
            collectTopLevelLambdasExpr(lambdas, s->value);
            break;
        }
        case EXPR_CAST:
            collectTopLevelLambdasExpr(lambdas, ((CastExpr*)e)->value);
            break;
        case EXPR_POSTFIX:
            collectTopLevelLambdasExpr(lambdas, ((PostfixExpr*)e)->operand);
            break;
        case EXPR_PREFIX:
            collectTopLevelLambdasExpr(lambdas, ((PrefixExpr*)e)->operand);
            break;
        case EXPR_MAP_LITERAL: {
            MapLiteralExpr* m = (MapLiteralExpr*)e;
            for (ListNode* n = m->entries ? m->entries->head : NULL; n != NULL; n = n->next) {
                MapEntry* me = (MapEntry*)n->data;
                if (me) collectTopLevelLambdasExpr(lambdas, me->value);
            }
            break;
        }
        case EXPR_ARRAY_LITERAL: {
            ArrayLiteralExpr* a = (ArrayLiteralExpr*)e;
            for (ListNode* n = a->elements ? a->elements->head : NULL; n != NULL; n = n->next) {
                collectTopLevelLambdasExpr(lambdas, (Expr*)n->data);
            }
            break;
        }
        case EXPR_INDEX: {
            IndexExpr* i = (IndexExpr*)e;
            collectTopLevelLambdasExpr(lambdas, i->object);
            collectTopLevelLambdasExpr(lambdas, i->index);
            break;
        }
        case EXPR_INDEX_SET: {
            IndexSetExpr* s = (IndexSetExpr*)e;
            collectTopLevelLambdasExpr(lambdas, s->object);
            collectTopLevelLambdasExpr(lambdas, s->index);
            collectTopLevelLambdasExpr(lambdas, s->value);
            break;
        }
        case EXPR_STRUCT_INIT: {
            StructInitExpr* si = (StructInitExpr*)e;
            collectTopLevelLambdasExpr(lambdas, si->callee);
            for (ListNode* n = si->fields ? si->fields->head : NULL; n != NULL; n = n->next) {
                StructFieldInit* f = (StructFieldInit*)n->data;
                if (f) collectTopLevelLambdasExpr(lambdas, f->value);
            }
            break;
        }
        default:
            break;
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
        case STMT_TRAIT:
            compileTraitStmt(compiler, (TraitStmt*)stmt);
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
        case STMT_TRAIT_IMPL:
            compileTraitImplStmt(compiler, (TraitImplStmt*)stmt);
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
        compilerEmitDropForBlockVars(compiler, scoped);
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
        compilerEmitDropForCurrentFunctionScopes(compiler);
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
                compilerEmitDropForCurrentFunctionScopes(compiler);
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
        compilerEmitDropForCurrentFunctionScopes(compiler);
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

    // Trait object return: if the function return type is a trait object (`Trait__obj`) and the
    // return expression produced a concrete struct value, box it and return an owning interface value.
    if (compiler && returnValue && returnType) {
        TraitInfo* retTrait = NULL;
        if (compiler->traits) {
            for (ListNode* n = compiler->traits->head; n != NULL; n = n->next) {
                TraitInfo* ti = (TraitInfo*)n->data;
                if (!ti) continue;
                LLVMTypeRef objTy = compilerGetTraitObjType(compiler, ti);
                if (objTy && objTy == returnType) {
                    retTrait = ti;
                    break;
                }
            }
        }
        if (retTrait) {
            LLVMTypeRef objTy = compilerGetTraitObjType(compiler, retTrait);
            if (objTy && LLVMTypeOf(returnValue) != objTy) {
                LLVMTypeRef concreteTy = LLVMTypeOf(returnValue);
                if (LLVMGetTypeKind(concreteTy) != LLVMStructTypeKind) {
                    compilerErrorAt(compiler, stmt ? stmt->keyword.line : 0, "trait object return requires a struct value");
                    return;
                }
                const char* structName = LLVMGetStructName(concreteTy);
                int structLen = structName ? (int)strlen(structName) : 0;
                if (!structName || structLen <= 0) {
                    compilerErrorAt(compiler, stmt ? stmt->keyword.line : 0, "trait object return requires a named struct type");
                    return;
                }

                // vtable global name: `__VT__<Trait>__<Struct>`
                const char* prefix = "__VT__";
                const int prefixLen = 5;
                const int sepLen = 2;
                int gLen = prefixLen + retTrait->nameLength + sepLen + structLen;
                char* gname = malloc((size_t)gLen + 1);
                memcpy(gname, prefix, (size_t)prefixLen);
                memcpy(gname + prefixLen, retTrait->name, (size_t)retTrait->nameLength);
                memcpy(gname + prefixLen + retTrait->nameLength, "__", (size_t)sepLen);
                memcpy(gname + prefixLen + retTrait->nameLength + sepLen, structName, (size_t)structLen);
                gname[gLen] = '\0';

                LLVMValueRef vt = LLVMGetNamedGlobal(compiler->module, gname);
                free(gname);
                if (!vt) {
                    compilerErrorAt(
                        compiler,
                        stmt ? stmt->keyword.line : 0,
                        "missing trait impl for trait object return"
                    );
                    return;
                }

                // Declare malloc if needed.
                LLVMValueRef mallocFn = LLVMGetNamedFunction(compiler->module, "malloc");
                if (!mallocFn) {
                    LLVMTypeRef i64 = LLVMInt64TypeInContext(compiler->context);
                    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
                    LLVMTypeRef mt = LLVMFunctionType(i8ptr, &i64, 1, 0);
                    mallocFn = LLVMAddFunction(compiler->module, "malloc", mt);
                }

                LLVMValueRef sizeV = LLVMSizeOf(concreteTy);
                LLVMValueRef raw = LLVMBuildCall2(
                    compiler->builder,
                    LLVMGlobalGetValueType(mallocFn),
                    mallocFn,
                    &sizeV,
                    1,
                    "malloc"
                );
                LLVMValueRef cell = LLVMBuildBitCast(compiler->builder, raw, LLVMPointerType(concreteTy, 0), "cell");
                LLVMBuildStore(compiler->builder, returnValue, cell);

                LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
                LLVMValueRef dataI8 = LLVMBuildBitCast(compiler->builder, cell, i8ptr, "data");
                LLVMValueRef vtI8 = LLVMBuildBitCast(compiler->builder, vt, i8ptr, "vt");
                LLVMValueRef obj = LLVMGetUndef(objTy);
                obj = LLVMBuildInsertValue(compiler->builder, obj, dataI8, 0, "o0");
                obj = LLVMBuildInsertValue(compiler->builder, obj, vtI8, 1, "o1");
                returnValue = obj;
            }
        }
    }

    returnValue = castValueToType(compiler, returnValue, returnType);
    compilerEmitDropForCurrentFunctionScopes(compiler);
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
    if (!info->methods) info->methods = listNew();

    Token structTok = stmt->name;
    structTok.start = info->name;
    structTok.length = info->nameLength;

    for (ListNode* node = stmt->methods->head; node != NULL; node = node->next) {
        FuncStmt* method = (FuncStmt*)node->data;
        if (!method) continue;
        if (info->methods) listAppend(info->methods, method);

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
        thisParam->mode = PARAM_CONST;
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

static int tokenEqualsTokenRaw(const Token* a, const Token* b) {
    if (!a || !b) return 0;
    if (a->length != b->length) return 0;
    if (!a->start || !b->start) return 0;
    return memcmp(a->start, b->start, (size_t)a->length) == 0;
}

static int astTypeIsBuiltinNamed(const Token* name) {
    if (!name) return 0;
    if (name->length == 3 && memcmp(name->start, "map", 3) == 0) return 1;
    if (name->length == 6 && memcmp(name->start, "Option", 6) == 0) return 1;
    if (name->length == 3 && memcmp(name->start, "ptr", 3) == 0) return 1;
    if (name->length == 5 && memcmp(name->start, "bytes", 5) == 0) return 1;
    if (name->length == 5 && memcmp(name->start, "Slice", 5) == 0) return 1;
    return 0;
}

static int astTypeEquals(Compiler* compiler, Type* a, Type* b) {
    if (a == b) return 1;
    if (!a || !b) return 0;
    if (a->kind != b->kind) return 0;

    switch (a->kind) {
        case TYPE_ANY:
        case TYPE_VOID:
        case TYPE_BOOL:
        case TYPE_STRING:
        case TYPE_PTR:
        case TYPE_I8:
        case TYPE_I16:
        case TYPE_INT:
        case TYPE_LONG:
        case TYPE_ISIZE:
        case TYPE_U8:
        case TYPE_U16:
        case TYPE_U32:
        case TYPE_U64:
        case TYPE_USIZE:
        case TYPE_BYTE:
        case TYPE_F8:
        case TYPE_F16:
        case TYPE_FLOAT:
        case TYPE_DOUBLE:
        case TYPE_BF8:
        case TYPE_BF16:
            return 1;
        case TYPE_REF:
            return astTypeEquals(compiler, a->inner, b->inner);
        case TYPE_ARRAY:
            if (a->arrayLen != b->arrayLen) return 0;
            return astTypeEquals(compiler, a->inner, b->inner);
        case TYPE_FUNC: {
            int ap = a->paramTypes ? a->paramTypes->length : 0;
            int bp = b->paramTypes ? b->paramTypes->length : 0;
            if (ap != bp) return 0;
            for (int i = 0; i < ap; i++) {
                Type* at = listGet(a->paramTypes, i);
                Type* bt = listGet(b->paramTypes, i);
                if (!astTypeEquals(compiler, at, bt)) return 0;
            }
            int ar = a->returnTypes ? a->returnTypes->length : 0;
            int br = b->returnTypes ? b->returnTypes->length : 0;
            if (ar != br) return 0;
            for (int i = 0; i < ar; i++) {
                Type* at = listGet(a->returnTypes, i);
                Type* bt = listGet(b->returnTypes, i);
                if (!astTypeEquals(compiler, at, bt)) return 0;
            }
            return 1;
        }
        case TYPE_NAMED: {
            // Builtins and generic builtins match by name+args.
            if (astTypeIsBuiltinNamed(&a->name) || astTypeIsBuiltinNamed(&b->name)) {
                if (!tokenEqualsTokenRaw(&a->name, &b->name)) return 0;
            } else {
                // Prefer semantic resolution across modules/imports.
                StructInfo* as = compiler ? compilerResolveStructByToken(compiler, &a->name) : NULL;
                StructInfo* bs = compiler ? compilerResolveStructByToken(compiler, &b->name) : NULL;
                if (as || bs) {
                    if (!as || !bs) return 0;
                    if (as->nameLength != bs->nameLength) return 0;
                    if (memcmp(as->name, bs->name, (size_t)as->nameLength) != 0) return 0;
                } else {
                    EnumInfo* ae = compiler ? compilerResolveEnumByToken(compiler, &a->name) : NULL;
                    EnumInfo* be = compiler ? compilerResolveEnumByToken(compiler, &b->name) : NULL;
                    if (ae || be) {
                        if (!ae || !be) return 0;
                        if (ae->nameLength != be->nameLength) return 0;
                        if (memcmp(ae->name, be->name, (size_t)ae->nameLength) != 0) return 0;
                    } else {
                        if (!tokenEqualsTokenRaw(&a->name, &b->name)) return 0;
                    }
                }
            }

            int ac = a->typeArgs ? a->typeArgs->length : 0;
            int bc = b->typeArgs ? b->typeArgs->length : 0;
            if (ac != bc) return 0;
            for (int i = 0; i < ac; i++) {
                Type* at = listGet(a->typeArgs, i);
                Type* bt = listGet(b->typeArgs, i);
                if (!astTypeEquals(compiler, at, bt)) return 0;
            }
            return 1;
        }
    }
    return 0;
}

static int paramListEquals(Compiler* compiler, List* requiredParams, List* implParams) {
    int rc = requiredParams ? requiredParams->length : 0;
    int ic = implParams ? implParams->length : 0;
    if (rc != ic) return 0;
    for (int i = 0; i < rc; i++) {
        Parameter* rp = listGet(requiredParams, i);
        Parameter* ip = listGet(implParams, i);
        if (!rp || !ip) return 0;
        if (rp->mode != ip->mode) return 0;
        if (!rp->type || !ip->type) return 0;
        if (!astTypeEquals(compiler, rp->type, ip->type)) return 0;
    }
    return 1;
}

static int returnTypesEquals(Compiler* compiler, List* required, List* impl) {
    int rc = required ? required->length : 0;
    int ic = impl ? impl->length : 0;
    if (rc != ic) return 0;
    for (int i = 0; i < rc; i++) {
        Type* rt = listGet(required, i);
        Type* it = listGet(impl, i);
        if (!rt || !it) return 0;
        if (!astTypeEquals(compiler, rt, it)) return 0;
    }
    return 1;
}

static FuncStmt* structFindDirectMethodByName(StructInfo* info, const Token* methodName, int* outCount) {
    if (outCount) *outCount = 0;
    if (!info || !info->methods || !methodName) return NULL;
    FuncStmt* found = NULL;
    int count = 0;
    for (ListNode* n = info->methods->head; n != NULL; n = n->next) {
        FuncStmt* m = (FuncStmt*)n->data;
        if (!m) continue;
        if (!tokenEqualsTokenRaw(&m->name, methodName)) continue;
        found = m;
        count++;
    }
    if (outCount) *outCount = count;
    return found;
}

typedef struct {
    int depth;           // number of embedded steps from root to target receiver type
    int indices[16];     // embedded field indices at each step
    StructInfo* target;  // struct that defines the method
    FuncStmt* method;    // method AST (signature)
    int isAmbiguous;
} PromotedMethodSigPath;

static void promotedMethodSigSearch(
    Compiler* compiler,
    StructInfo* info,
    const Token* methodName,
    int depth,
    int indices[16],
    PromotedMethodSigPath* ioBest
) {
    if (!compiler || !info || !info->decl || !info->decl->fields || !methodName || !ioBest) return;
    if (depth < 0 || depth >= (int)(sizeof(ioBest->indices) / sizeof(ioBest->indices[0]))) return;

    int directCount = 0;
    FuncStmt* direct = structFindDirectMethodByName(info, methodName, &directCount);
    if (directCount > 1) {
        ioBest->isAmbiguous = 1;
        return;
    }
    if (direct) {
        if (ioBest->target) {
            ioBest->isAmbiguous = 1;
            return;
        }
        ioBest->depth = depth;
        for (int i = 0; i < depth; i++) ioBest->indices[i] = indices[i];
        ioBest->target = info;
        ioBest->method = direct;
        return;
    }

    for (int i = 0; i < info->decl->fields->length; i++) {
        FieldDeclaration* f = listGet(info->decl->fields, i);
        if (!f || !f->isEmbedded || !f->type) continue;
        if (f->type->kind != TYPE_NAMED) continue;
        StructInfo* inner = compilerResolveStructByToken(compiler, &f->type->name);
        if (!inner) continue;
        indices[depth] = i;
        promotedMethodSigSearch(compiler, inner, methodName, depth + 1, indices, ioBest);
        if (ioBest->isAmbiguous) return;
    }
}

// Returns 1 on success, 0 if not found, -1 if ambiguous.
static int resolvePromotedMethodSigPath(Compiler* compiler, StructInfo* root, const Token* methodName, PromotedMethodSigPath* out) {
    if (out) memset(out, 0, sizeof(*out));
    if (!compiler || !root || !methodName || !out) return 0;

    // Direct method wins (shadows embedded).
    int directCount = 0;
    FuncStmt* direct = structFindDirectMethodByName(root, methodName, &directCount);
    if (directCount > 1) return -1;
    if (direct) {
        out->depth = 0;
        out->target = root;
        out->method = direct;
        return 1;
    }

    int tmp[16] = {0};
    promotedMethodSigSearch(compiler, root, methodName, 0, tmp, out);
    if (out->isAmbiguous) return -1;
    if (!out->target || !out->method) return 0;
    return 1;
}

void compileTraitStmt(Compiler* compiler, TraitStmt* stmt) {
    if (!compiler || !stmt) return;

    char* traitName = malloc((size_t)stmt->name.length + 1);
    memcpy(traitName, stmt->name.start, (size_t)stmt->name.length);
    traitName[stmt->name.length] = '\0';

    if (compilerFindTrait(compiler, stmt->name.start, stmt->name.length)) {
        compilerErrorAtToken(compiler, &stmt->name, "duplicate trait: %.*s", stmt->name.length, stmt->name.start);
        free(traitName);
        return;
    }

    // Basic validation: unique method names and explicit param types.
    if (stmt->methods) {
        for (int i = 0; i < stmt->methods->length; i++) {
            TraitMethodDecl* m = listGet(stmt->methods, i);
            if (!m) continue;
            for (int j = i + 1; j < stmt->methods->length; j++) {
                TraitMethodDecl* n = listGet(stmt->methods, j);
                if (!n) continue;
                if (tokenEqualsTokenRaw(&m->name, &n->name)) {
                    compilerErrorAtToken(compiler, &n->name, "duplicate trait method: %.*s", n->name.length, n->name.start);
                    break;
                }
            }
            for (ListNode* pn = m->params ? m->params->head : NULL; pn != NULL; pn = pn->next) {
                Parameter* p = (Parameter*)pn->data;
                if (!p) continue;
                if (!p->type) {
                    compilerErrorAtToken(compiler, &p->name, "trait method parameter requires an explicit type");
                    break;
                }
            }
        }
    }

    TraitInfo* info = malloc(sizeof(TraitInfo));
    info->name = traitName;
    info->nameLength = stmt->name.length;
    info->decl = stmt;
    info->objType = NULL;
    info->vtableType = NULL;
    listAppend(compiler->traits, info);
}

static int traitMethodMatchesFunc(Compiler* compiler, TraitMethodDecl* req, FuncStmt* impl) {
    if (!req || !impl) return 0;
    if (!paramListEquals(compiler, req->params, impl->params)) return 0;
    return returnTypesEquals(compiler, req->returnTypes, impl->returnTypes);
}

static TraitMethodDecl* traitFindMethodDeclByName(TraitInfo* trait, const Token* name) {
    if (!trait || !trait->decl || !name) return NULL;
    for (ListNode* mn = trait->decl->methods ? trait->decl->methods->head : NULL; mn != NULL; mn = mn->next) {
        TraitMethodDecl* req = (TraitMethodDecl*)mn->data;
        if (req && tokenEqualsTokenRaw(&req->name, name)) return req;
    }
    return NULL;
}

static int traitMethodIndexByName(TraitInfo* trait, const Token* name) {
    if (!trait || !trait->decl || !name) return -1;
    int idx = 0;
    for (ListNode* mn = trait->decl->methods ? trait->decl->methods->head : NULL; mn != NULL; mn = mn->next, idx++) {
        TraitMethodDecl* req = (TraitMethodDecl*)mn->data;
        if (!req) continue;
        if (tokenEqualsTokenRaw(&req->name, name)) return idx;
    }
    return -1;
}

static LLVMTypeRef llvmTypeFromTraitReturnTypes(Compiler* compiler, TraitMethodDecl* m) {
    if (!compiler || !m) return LLVMVoidTypeInContext(compiler->context);
    int rc = m->returnTypes ? m->returnTypes->length : 0;
    if (rc <= 0) return LLVMVoidTypeInContext(compiler->context);
    if (rc == 1) {
        Type* t = (Type*)listGet(m->returnTypes, 0);
        return typeToLLVMType(compiler, t, true);
    }
    LLVMTypeRef* rts = malloc(sizeof(LLVMTypeRef) * (size_t)rc);
    for (int i = 0; i < rc; i++) {
        Type* t = (Type*)listGet(m->returnTypes, i);
        rts[i] = typeToLLVMType(compiler, t, false);
    }
    LLVMTypeRef out = LLVMStructTypeInContext(compiler->context, rts, (unsigned)rc, 0);
    free(rts);
    return out;
}

static LLVMTypeRef llvmTypeFromTraitParam(Compiler* compiler, Parameter* p) {
    if (!compiler || !p) return LLVMInt32TypeInContext(compiler->context);
    LLVMTypeRef pt = typeToLLVMType(compiler, p->type, false);
    if (p->mode != PARAM_MOVE && astTypeIsNamedStructValue(compiler, p->type)) {
        pt = LLVMPointerType(pt, 0);
    }
    return pt;
}

static char* mangleVtableGlobalName(const char* traitName, int traitLen, const char* structName, int structLen) {
    const char* prefix = "__VT__";
    const int prefixLen = 5;
    const int sepLen = 2;
    int len = prefixLen + traitLen + sepLen + structLen;
    char* s = malloc((size_t)len + 1);
    memcpy(s, prefix, (size_t)prefixLen);
    memcpy(s + prefixLen, traitName, (size_t)traitLen);
    memcpy(s + prefixLen + traitLen, "__", (size_t)sepLen);
    memcpy(s + prefixLen + traitLen + sepLen, structName, (size_t)structLen);
    s[len] = '\0';
    return s;
}

static LLVMValueRef getOrCreateTraitVtableGlobal(Compiler* compiler, TraitInfo* trait, StructInfo* target) {
    if (!compiler || !trait || !target) return NULL;
    if (!trait->decl || !target->decl) return NULL;

    LLVMTypeRef vtTy = compilerGetTraitVtableType(compiler, trait);
    if (!vtTy) return NULL;

    char* gname = mangleVtableGlobalName(trait->name, trait->nameLength, target->name, target->nameLength);
    LLVMValueRef existing = LLVMGetNamedGlobal(compiler->module, gname);
    if (existing) {
        free(gname);
        return existing;
    }

    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);

    // Ensure struct drop exists for this concrete type.
    LLVMValueRef dropStruct = compilerGetOrCreateStructDrop(compiler, target);
    if (!dropStruct) {
        compilerErrorAt(compiler, 0, "failed to create drop for struct");
        free(gname);
        return NULL;
    }

    // Drop wrapper: void(i8*) which calls `<Struct>__drop` then `free`.
    int dropWrapLen = 0;
    Token traitTok = (Token){TOKEN_IDENTIFIER, trait->name, trait->nameLength, 0, 0, 0};
    Token structTok = (Token){TOKEN_IDENTIFIER, target->name, target->nameLength, 0, 0, 0};
    Token dropTok = (Token){TOKEN_IDENTIFIER, "drop", 4, 0, 0, 0};
    char* ts = mangleTwo(&traitTok, &structTok, "__", &dropWrapLen);
    Token tsTok = (Token){TOKEN_IDENTIFIER, ts, dropWrapLen, 0, 0, 0};
    char* dropWrapName = mangleTwo(&tsTok, &dropTok, "__VT__", &dropWrapLen);
    free(ts);

    LLVMValueRef dropWrap = LLVMGetNamedFunction(compiler->module, dropWrapName);
    LLVMTypeRef dropWrapTy = LLVMFunctionType(LLVMVoidTypeInContext(compiler->context), &i8ptr, 1, 0);
    if (!dropWrap) {
        dropWrap = LLVMAddFunction(compiler->module, dropWrapName, dropWrapTy);
        LLVMSetLinkage(dropWrap, LLVMInternalLinkage);

        LLVMBasicBlockRef saved = LLVMGetInsertBlock(compiler->builder);
        LLVMBasicBlockRef entry = LLVMAppendBasicBlock(dropWrap, "entry");
        LLVMPositionBuilderAtEnd(compiler->builder, entry);

        LLVMValueRef data = LLVMGetParam(dropWrap, 0);
        LLVMValueRef self = LLVMBuildBitCast(compiler->builder, data, LLVMPointerType(target->type, 0), "self");
        LLVMTypeRef sdTy = LLVMGlobalGetValueType(dropStruct);
        LLVMBuildCall2(compiler->builder, sdTy, dropStruct, &self, 1, "");
        LLVMValueRef freeFn = getOrCreateFree(compiler);
        LLVMTypeRef freeTy = LLVMGlobalGetValueType(freeFn);
        LLVMBuildCall2(compiler->builder, freeTy, freeFn, &data, 1, "");
        LLVMBuildRetVoid(compiler->builder);

        if (saved) LLVMPositionBuilderAtEnd(compiler->builder, saved);
    }

    int mc = trait->decl->methods ? trait->decl->methods->length : 0;
    int fc = 1 + mc;
    LLVMValueRef* fields = malloc(sizeof(LLVMValueRef) * (size_t)fc);
    fields[0] = LLVMConstBitCast(dropWrap, i8ptr);

    // Method wrappers and vtable entries.
    for (int mi = 0; mi < mc; mi++) {
        TraitMethodDecl* req = (TraitMethodDecl*)listGet(trait->decl->methods, mi);
        if (!req) {
            fields[1 + mi] = LLVMConstNull(i8ptr);
            continue;
        }

        int wrapNameLen = 0;
        Token mnTok = req->name;
        char* tss = mangleTwo(&traitTok, &structTok, "__", &wrapNameLen);
        Token tssTok = (Token){TOKEN_IDENTIFIER, tss, wrapNameLen, 0, 0, 0};
        char* wrapName = mangleTwo(&tssTok, &mnTok, "__VT__", &wrapNameLen);
        free(tss);

        LLVMTypeRef retTy = llvmTypeFromTraitReturnTypes(compiler, req);
        int argc = req->params ? req->params->length : 0;
        LLVMTypeRef* wps = malloc(sizeof(LLVMTypeRef) * (size_t)(1 + argc));
        wps[0] = i8ptr;
        for (int ai = 0; ai < argc; ai++) {
            Parameter* p = (Parameter*)listGet(req->params, ai);
            wps[1 + ai] = llvmTypeFromTraitParam(compiler, p);
        }
        LLVMTypeRef wty = LLVMFunctionType(retTy, wps, (unsigned)(1 + argc), 0);
        free(wps);

        LLVMValueRef wrap = LLVMGetNamedFunction(compiler->module, wrapName);
        if (!wrap) {
            wrap = LLVMAddFunction(compiler->module, wrapName, wty);
            LLVMSetLinkage(wrap, LLVMInternalLinkage);

            LLVMBasicBlockRef saved = LLVMGetInsertBlock(compiler->builder);
            LLVMBasicBlockRef entry = LLVMAppendBasicBlock(wrap, "entry");
            LLVMPositionBuilderAtEnd(compiler->builder, entry);

            LLVMValueRef data = LLVMGetParam(wrap, 0);
            LLVMValueRef root = LLVMBuildBitCast(compiler->builder, data, LLVMPointerType(target->type, 0), "root");

            // Resolve the concrete implementation: prefer `Struct__Trait__m`, else inherent/promoted `Struct__m`.
            LLVMValueRef implFn = NULL;
            {
                int stLen = 0;
                char* st = mangleTwo(&structTok, &traitTok, "__", &stLen);
                Token stTok = (Token){TOKEN_IDENTIFIER, st, stLen, 0, 0, 0};
                int fullLen = 0;
                char* full = mangleTwo(&stTok, &req->name, "__", &fullLen);
                free(st);
                implFn = LLVMGetNamedFunction(compiler->module, full);
                free(full);
            }

            PromotedMethodSigPath path = {0};
            if (!implFn) {
                int r = resolvePromotedMethodSigPath(compiler, target, &req->name, &path);
                if (r > 0 && path.target) {
                    Token it = (Token){TOKEN_IDENTIFIER, path.target->name, path.target->nameLength, 0, 0, 0};
                    int ml = 0;
                    char* n = mangleTwo(&it, &req->name, "__", &ml);
                    implFn = LLVMGetNamedFunction(compiler->module, n);
                    free(n);
                }
            }

            if (!implFn) {
                // leave a null slot; dispatch will trap if called.
                if (LLVMGetTypeKind(retTy) == LLVMVoidTypeKind) LLVMBuildRetVoid(compiler->builder);
                else LLVMBuildRet(compiler->builder, LLVMConstNull(retTy));
                if (saved) LLVMPositionBuilderAtEnd(compiler->builder, saved);
                free(wrapName);
                continue;
            }

            // Compute promoted receiver pointer if needed.
            LLVMValueRef recv = root;
            LLVMTypeRef curTy = target->type;
            StructInfo* curInfo = target;
            if (path.target && path.depth > 0) {
                for (int di = 0; di < path.depth; di++) {
                    int embIdx = path.indices[di];
                    FieldDeclaration* f = curInfo && curInfo->decl ? (FieldDeclaration*)listGet(curInfo->decl->fields, embIdx) : NULL;
                    if (!f || !f->type || f->type->kind != TYPE_NAMED) break;
                    StructInfo* inner = compilerResolveStructByToken(compiler, &f->type->name);
                    if (!inner) break;
                    recv = LLVMBuildStructGEP2(compiler->builder, curTy, recv, (unsigned)embIdx, "emb_ptr");
                    curTy = inner->type;
                    curInfo = inner;
                }
            }

            LLVMTypeRef ifTy = LLVMGlobalGetValueType(implFn);
            unsigned expected = LLVMCountParamTypes(ifTy);
            LLVMTypeRef* ipt = NULL;
            if (expected > 0) {
                ipt = malloc(sizeof(LLVMTypeRef) * (size_t)expected);
                LLVMGetParamTypes(ifTy, ipt);
            }
            LLVMValueRef* args = NULL;
            if (expected > 0) {
                args = malloc(sizeof(LLVMValueRef) * (size_t)expected);
                args[0] = castValueToType(compiler, recv, ipt[0]);
                for (unsigned ai = 1; ai < expected; ai++) {
                    LLVMValueRef av = LLVMGetParam(wrap, ai);
                    args[ai] = castValueToType(compiler, av, ipt[ai]);
                }
            }
            LLVMValueRef call = LLVMBuildCall2(compiler->builder, ifTy, implFn, args, expected, "");
            if (ipt) free(ipt);
            if (args) free(args);

            if (LLVMGetTypeKind(retTy) == LLVMVoidTypeKind) LLVMBuildRetVoid(compiler->builder);
            else LLVMBuildRet(compiler->builder, call);

            if (saved) LLVMPositionBuilderAtEnd(compiler->builder, saved);
        }

        fields[1 + mi] = LLVMConstBitCast(wrap, i8ptr);
        free(wrapName);
    }

    LLVMValueRef gv = LLVMAddGlobal(compiler->module, vtTy, gname);
    LLVMSetLinkage(gv, LLVMInternalLinkage);
    LLVMValueRef init = LLVMConstNamedStruct(vtTy, fields, (unsigned)fc);
    LLVMSetInitializer(gv, init);
    LLVMSetGlobalConstant(gv, 1);

    free(fields);
    free(gname);
    free(dropWrapName);
    return gv;
}

void compileTraitImplStmt(Compiler* compiler, TraitImplStmt* stmt) {
    if (!compiler || !stmt) return;

    TraitInfo* trait = compilerResolveTraitByToken(compiler, &stmt->traitName);
    if (!trait || !trait->decl) {
        compilerErrorAtToken(compiler, &stmt->traitName, "unknown trait: %.*s", stmt->traitName.length, stmt->traitName.start);
        return;
    }

    StructInfo* target = compilerResolveStructByToken(compiler, &stmt->targetName);
    if (!target || !target->decl) {
        compilerErrorAtToken(compiler, &stmt->targetName, "unknown struct for trait impl: %.*s", stmt->targetName.length, stmt->targetName.start);
        return;
    }

    // Ensure the impl pair is recorded for generic bounds checks (prepass should do this too).
    compilerRecordTraitImplPair(compiler, trait->name, trait->nameLength, target->name, target->nameLength);

    // Compile inline methods inside `impl Trait for Struct { ... }` as trait-namespaced methods:
    // `<Struct>__<Trait>__<method>`. They do NOT participate in the struct's inherent method set.
    if (stmt->methods && stmt->methods->length > 0) {
        Token structTok = stmt->targetName;
        structTok.start = target->name;
        structTok.length = target->nameLength;
        Token traitTok = stmt->traitName;
        traitTok.start = trait->name;
        traitTok.length = trait->nameLength;

        // Reject duplicates inside the same trait impl block.
        for (int i = 0; i < stmt->methods->length; i++) {
            FuncStmt* m = (FuncStmt*)listGet(stmt->methods, i);
            if (!m) continue;
            for (int j = i + 1; j < stmt->methods->length; j++) {
                FuncStmt* n = (FuncStmt*)listGet(stmt->methods, j);
                if (!n) continue;
                if (tokenEqualsTokenRaw(&m->name, &n->name)) {
                    compilerErrorAtToken(compiler, &n->name, "duplicate trait impl method: %.*s", n->name.length, n->name.start);
                    break;
                }
            }
        }

        for (ListNode* node = stmt->methods->head; node != NULL; node = node->next) {
            FuncStmt* method = (FuncStmt*)node->data;
            if (!method) continue;

            TraitMethodDecl* req = traitFindMethodDeclByName(trait, &method->name);
            if (!req) {
                compilerErrorAtToken(
                    compiler,
                    &method->name,
                    "unknown trait method '%.*s' in `impl %.*s for %.*s`",
                    method->name.length, method->name.start,
                    stmt->traitName.length, stmt->traitName.start,
                    stmt->targetName.length, stmt->targetName.start
                );
                continue;
            }
            if (!traitMethodMatchesFunc(compiler, req, method)) {
                compilerErrorAtToken(
                    compiler,
                    &method->name,
                    "trait impl signature mismatch for '%.*s.%.*s'",
                    stmt->traitName.length, stmt->traitName.start,
                    method->name.length, method->name.start
                );
                continue;
            }

            int stLen = 0;
            char* st = mangleTwo(&structTok, &traitTok, "__", &stLen);
            Token stTok = (Token){TOKEN_IDENTIFIER, st, stLen, method->name.line, method->name.col, 0};

            int mangledLen = 0;
            char* mangled = mangleTwo(&stTok, &method->name, "__", &mangledLen);

            LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, mangled);
            if (existing) {
                compilerErrorAtToken(
                    compiler,
                    &method->name,
                    "duplicate trait impl method definition: %.*s for %.*s",
                    stmt->traitName.length, stmt->traitName.start,
                    stmt->targetName.length, stmt->targetName.start
                );
                free(st);
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
            thisInner->typeArgs = NULL;

            Type* thisType = malloc(sizeof(Type));
            thisType->kind = TYPE_REF;
            thisType->name = (Token){0};
            thisType->inner = thisInner;
            thisType->paramTypes = NULL;
            thisType->returnTypes = NULL;
            thisType->typeArgs = NULL;

            Parameter* thisParam = malloc(sizeof(Parameter));
            thisParam->name = thisNameTok;
            thisParam->type = thisType;
            thisParam->mode = PARAM_CONST;
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

            free(st);
            free(mangled);
        }
        if (compiler->hadError) return;
    }

    // Validate trait satisfaction using method set (includes promoted methods).
    for (ListNode* mn = trait->decl->methods ? trait->decl->methods->head : NULL; mn != NULL; mn = mn->next) {
        TraitMethodDecl* req = (TraitMethodDecl*)mn->data;
        if (!req) continue;

        // If this method is implemented in the trait impl block, it satisfies the requirement directly.
        int hasInline = 0;
        for (ListNode* in = stmt->methods ? stmt->methods->head : NULL; in != NULL; in = in->next) {
            FuncStmt* im = (FuncStmt*)in->data;
            if (im && tokenEqualsTokenRaw(&im->name, &req->name)) { hasInline = 1; break; }
        }
        if (hasInline) continue;

        PromotedMethodSigPath path = {0};
        int r = resolvePromotedMethodSigPath(compiler, target, &req->name, &path);
        if (r < 0) {
            compilerErrorAtToken(
                compiler,
                &req->name,
                "trait '%.*s' not satisfied by '%.*s': ambiguous method '%.*s' (write explicit path)",
                stmt->traitName.length, stmt->traitName.start,
                stmt->targetName.length, stmt->targetName.start,
                req->name.length, req->name.start
            );
            continue;
        }
        if (r == 0 || !path.method) {
            compilerErrorAtToken(
                compiler,
                &req->name,
                "trait '%.*s' not satisfied by '%.*s': missing method '%.*s'",
                stmt->traitName.length, stmt->traitName.start,
                stmt->targetName.length, stmt->targetName.start,
                req->name.length, req->name.start
            );
            continue;
        }

        if (!traitMethodMatchesFunc(compiler, req, path.method)) {
            compilerErrorAtToken(
                compiler,
                &req->name,
                "trait '%.*s' not satisfied by '%.*s': signature mismatch for method '%.*s'",
                stmt->traitName.length, stmt->traitName.start,
                stmt->targetName.length, stmt->targetName.start,
                req->name.length, req->name.start
            );
        }
    }

    if (compiler->hadError) return;
    // Ensure dynamic-dispatch vtable exists for this impl pair (used when a trait is used as a value type).
    (void)getOrCreateTraitVtableGlobal(compiler, trait, target);
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

            int isBoxed = compilerShouldBoxLocal(compiler, nameTok->start, nameTok->length);
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

            VariableRef* variable = (VariableRef*)calloc(1, sizeof(VariableRef));
            variable->name = varName;
            variable->length = nameTok->length;
            variable->value = slot;
            variable->type = targetType;
            variable->pointeeType = NULL;

            const char* typeName = llvmStructNameOrNull(targetType);
            if (typeName) {
                variable->typeName = typeName;
                variable->typeNameLength = (int)strlen(typeName);
            } else {
                variable->typeName = NULL;
                variable->typeNameLength = 0;
            }

            variable->isConst = stmt->isConst ? 1 : 0;
            variable->isBorrowed = 0;
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
            // Do NOT infer `bytes` from LLVM pointer equality under opaque pointers; use AST type names only.
            variable->isBytes = 0;
            variable->isSlice = 0;
            variable->sliceElemType = NULL;
            variable->sliceElemKind = TYPE_ANY;
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

    if (stmt && stmt->typeParams && stmt->typeParams->length > 0) {
        compilerErrorAtToken(
            compiler,
            &stmt->name,
            "generic function templates are not compiled directly; call with explicit type arguments (e.g. %.*s<int>(...))",
            stmt->name.length,
            stmt->name.start
        );
        return;
    }

    char* funcName = malloc((size_t)stmt->name.length + 1);
    memcpy(funcName, stmt->name.start, (size_t)stmt->name.length);
    funcName[stmt->name.length] = '\0';

    // Escape analysis v1 (closures): box only locals/params captured by any top-level lambda in this function.
    // This keeps closure-by-reference safe while avoiding boxing unrelated locals.
    int savedBox = compiler->boxAllLocals;
    List* savedBoxedLocals = compiler->boxedLocals;
    int containsLambda = 0;
    for (ListNode* n = stmt->body ? stmt->body->head : NULL; n != NULL; n = n->next) {
        if (stmtHasLambdaLiteral((Stmt*)n->data)) { containsLambda = 1; break; }
    }
    compiler->boxAllLocals = savedBox;
    compiler->boxedLocals = NULL;
    if (containsLambda) {
        List* lambdas = listNew();
        for (ListNode* n = stmt->body ? stmt->body->head : NULL; n != NULL; n = n->next) {
            collectTopLevelLambdasStmt(lambdas, (Stmt*)n->data);
        }

        List* boxed = listNew();
        for (ListNode* n = lambdas->head; n != NULL; n = n->next) {
            LambdaExpr* le = (LambdaExpr*)n->data;
            if (!le) continue;
            List* freeNames = compilerComputeLambdaFreeNames(compiler, le);
            for (ListNode* m = freeNames ? freeNames->head : NULL; m != NULL; m = m->next) {
                Token* t = (Token*)m->data;
                if (t) tokenSetAdd(boxed, t->start, t->length);
            }
            tokenSetFree(freeNames);
        }
        listFree(lambdas);
        compiler->boxedLocals = boxed->length > 0 ? boxed : (tokenSetFree(boxed), (List*)NULL);
    }

    int paramCount = stmt->params ? stmt->params->length : 0;
    LLVMTypeRef* paramTypes = NULL;
    if (paramCount > 0) {
        paramTypes = malloc(sizeof(LLVMTypeRef) * (size_t)paramCount);
        for (int i = 0; i < paramCount; i++) {
            Parameter* p = listGet(stmt->params, i);
            LLVMTypeRef pt = typeToLLVMType(compiler, p ? p->type : NULL, false);
            if (p && p->mode != PARAM_MOVE && astTypeIsNamedStructValue(compiler, p->type)) {
                pt = LLVMPointerType(pt, 0);
            }
            paramTypes[i] = pt;
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

    // Tail recursion elimination: enable only when this function does not use boxing for closures.
    // Boxing makes tail-call frame reuse observable for captured variables.
    TailrecState tr = {0};
    tr.enabled = (compiler->boxAllLocals || (compiler->boxedLocals && compiler->boxedLocals->length > 0)) ? 0 : 1;
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
        int isBoxed = compilerShouldBoxLocal(compiler, p->name.start, p->name.length);
        LLVMTypeRef valueType = paramTypes[i];
        LLVMTypeRef boxPtrType = isBoxed ? LLVMPointerType(valueType, 0) : NULL;
        Type* pType = (p && p->type) ? compilerResolveGenericType(compiler, p->type) : NULL;
        TraitInfo* paramTrait = NULL;
        if (pType && pType->kind == TYPE_NAMED) {
            paramTrait = compilerResolveTraitByToken(compiler, &pType->name);
        }
        int isBorrowedParam = (pType && pType->kind == TYPE_REF) ? 1 : ((p && p->mode != PARAM_MOVE) ? 1 : 0);

        if (isBoxed) {
            slot = LLVMBuildAlloca(compiler->builder, boxPtrType, paramName);
            LLVMValueRef boxAlloc = getOrCreateTuaBoxAlloc(compiler);
            LLVMValueRef dropFn = NULL;
            if (!isBorrowedParam) {
                dropFn = compilerGetOrCreateBoxDropFn(
                    compiler,
                    valueType,
                    pType,
                    paramTrait ? 1 : 0,
                    paramTrait ? paramTrait->name : NULL,
                    paramTrait ? paramTrait->nameLength : 0
                );
            }
            LLVMTypeRef i64 = LLVMInt64TypeInContext(compiler->context);
            LLVMValueRef sizeV = LLVMSizeOf(valueType);
            LLVMValueRef size64 = sizeV;
            if (LLVMTypeOf(sizeV) != i64) {
                size64 = LLVMBuildZExt(compiler->builder, sizeV, i64, "bsz");
            }
            LLVMTypeRef dropFnPtrTy = LLVMPointerType(tuaBoxDropFnType(compiler), 0);
            LLVMValueRef dropArg = dropFn ? LLVMBuildBitCast(compiler->builder, dropFn, dropFnPtrTy, "dropfn") : LLVMConstNull(dropFnPtrTy);
            LLVMTypeRef allocTy = LLVMGlobalGetValueType(boxAlloc);
            LLVMValueRef args2[2] = { size64, dropArg };
            LLVMValueRef raw = LLVMBuildCall2(compiler->builder, allocTy, boxAlloc, args2, 2, "box");
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

        VariableRef* variable = (VariableRef*)calloc(1, sizeof(VariableRef));
        variable->name = paramName;
        variable->length = p->name.length;
        variable->value = slot;
        variable->type = valueType;
        variable->pointeeType = (pType && pType->kind == TYPE_REF)
                                    ? (pType->inner ? typeToLLVMType(compiler, pType->inner, false)
                                                    : LLVMInt32TypeInContext(compiler->context))
                                    : NULL;
        if (pType && pType->kind == TYPE_NAMED) {
            StructInfo* info = compilerResolveStructByToken(compiler, &pType->name);
            if (info) {
                variable->typeName = info->name;
                variable->typeNameLength = info->nameLength;
            } else {
                variable->typeName = pType->name.start;
                variable->typeNameLength = pType->name.length;
            }
        } else if (pType && pType->kind == TYPE_REF && pType->inner && pType->inner->kind == TYPE_NAMED) {
            StructInfo* info = compilerResolveStructByToken(compiler, &pType->inner->name);
            if (info) {
                variable->typeName = info->name;
                variable->typeNameLength = info->nameLength;
            } else {
                variable->typeName = pType->inner->name.start;
                variable->typeNameLength = pType->inner->name.length;
            }
        } else {
            variable->typeName = NULL;
            variable->typeNameLength = 0;
        }

        // If the parameter is a trait object type (no `dyn` keyword), record it and avoid treating it as a struct.
        if (pType && pType->kind == TYPE_NAMED) {
            TraitInfo* ti = compilerResolveTraitByToken(compiler, &pType->name);
            if (ti) {
                variable->isTraitObj = 1;
                variable->traitName = ti->name;
                variable->traitNameLength = ti->nameLength;
                variable->typeName = NULL;
                variable->typeNameLength = 0;
            }
        }
        variable->isConst = (p && p->mode == PARAM_CONST) ? 1 : 0;
        variable->isBorrowed = (p && p->type && p->type->kind == TYPE_REF) ? 1 : ((p && p->mode != PARAM_MOVE) ? 1 : 0);
        variable->isGlobal = 0;
        variable->isBoxed = isBoxed;
        variable->boxOwns = isBoxed ? 1 : 0;
        variable->boxPtrType = isBoxed ? boxPtrType : NULL;
        variable->isMap = 0;
        variable->isBytes = 0;
        variable->isSlice = 0;
        variable->sliceElemType = NULL;
        variable->sliceElemKind = TYPE_ANY;
        variable->isTypedMap = 0;
        variable->mapKeyType = NULL;
        variable->mapValueType = NULL;
        variable->isArray = 0;
        variable->arrayElemType = NULL;
        variable->arrayFixedLen = -1;
        variable->isStackArray = 0;
        variable->stackArrayData = NULL;
        variable->genericParamName = NULL;
        variable->genericParamNameLength = 0;
        variable->genericBoundTraitName = NULL;
        variable->genericBoundTraitNameLength = 0;

        // Record generic type-param origin (for trait static dispatch).
        if (p && p->type) {
            if (p->type->kind == TYPE_NAMED && (!p->type->typeArgs || p->type->typeArgs->length == 0) &&
                !astTypeIsBuiltinNamed(&p->type->name)) {
                GenericSubst* gs = compilerFindGenericSubst(compiler, p->type->name.start, p->type->name.length);
                if (gs) {
                    variable->genericParamName = gs->name;
                    variable->genericParamNameLength = gs->nameLen;
                    variable->genericBoundTraitName = gs->boundTraitName;
                    variable->genericBoundTraitNameLength = gs->boundTraitNameLen;
                }
            } else if (p->type->kind == TYPE_REF && p->type->inner && p->type->inner->kind == TYPE_NAMED &&
                       (!p->type->inner->typeArgs || p->type->inner->typeArgs->length == 0) &&
                       !astTypeIsBuiltinNamed(&p->type->inner->name)) {
                GenericSubst* gs = compilerFindGenericSubst(compiler, p->type->inner->name.start, p->type->inner->name.length);
                if (gs) {
                    variable->genericParamName = gs->name;
                    variable->genericParamNameLength = gs->nameLen;
                    variable->genericBoundTraitName = gs->boundTraitName;
                    variable->genericBoundTraitNameLength = gs->boundTraitNameLen;
                }
            }
        }

        if (p->type && p->type->kind == TYPE_NAMED &&
            p->type->name.length == 3 && memcmp(p->type->name.start, "map", 3) == 0 &&
            p->type->typeArgs && p->type->typeArgs->length == 2) {
            variable->isMap = 1;
            Type* kAst = (Type*)p->type->typeArgs->head->data;
            Type* vAst = (Type*)p->type->typeArgs->head->next->data;
            int okKey = kAst && (kAst->kind == TYPE_STRING || kAst->kind == TYPE_INT || kAst->kind == TYPE_LONG);
            int okVal = 0;
            if (vAst) {
                if (vAst->kind == TYPE_STRING || vAst->kind == TYPE_INT || vAst->kind == TYPE_LONG ||
                    vAst->kind == TYPE_FLOAT || vAst->kind == TYPE_DOUBLE || vAst->kind == TYPE_BOOL) {
                    okVal = 1;
                } else if (vAst->kind == TYPE_ARRAY) {
                    okVal = 1;
                } else if (vAst->kind == TYPE_NAMED) {
                    okVal = 1;
                }
            }
            if (!okKey) {
                error("map<K,V> key type must be string/int/long for now\n");
            } else if (!okVal) {
                error("map<K,V> value type must be scalar/struct/map/array for now\n");
            } else {
                variable->isTypedMap = 1;
                variable->mapKeyType = typeToLLVMType(compiler, kAst, false);
                variable->mapValueType = typeToLLVMType(compiler, vAst, false);
                variable->mapKeyKind = kAst ? kAst->kind : TYPE_ANY;
                variable->mapValueKind = vAst ? vAst->kind : TYPE_ANY;
                variable->mapValueIsMap = (vAst && vAst->kind == TYPE_NAMED &&
                                           vAst->name.length == 3 && memcmp(vAst->name.start, "map", 3) == 0)
                                              ? 1
                                              : 0;
                variable->mapValueTypeName = NULL;
                variable->mapValueTypeNameLength = 0;
                if (vAst && vAst->kind == TYPE_NAMED && !(vAst->name.length == 3 && memcmp(vAst->name.start, "map", 3) == 0)) {
                    StructInfo* info = compilerResolveStructByToken(compiler, &vAst->name);
                    if (info) {
                        variable->mapValueTypeName = info->name;
                        variable->mapValueTypeNameLength = info->nameLength;
                    } else {
                        variable->mapValueTypeName = vAst->name.start;
                        variable->mapValueTypeNameLength = vAst->name.length;
                    }
                }
            }
        }

        if (p->type && p->type->kind == TYPE_NAMED &&
            p->type->name.length == 3 && memcmp(p->type->name.start, "map", 3) == 0 &&
            (!p->type->typeArgs || p->type->typeArgs->length == 0)) {
            variable->isMap = 1;
        }

        if (p->type && p->type->kind == TYPE_NAMED &&
            p->type->name.length == 5 && memcmp(p->type->name.start, "bytes", 5) == 0) {
            variable->isBytes = 1;
        }

        if (p->type && p->type->kind == TYPE_NAMED &&
            p->type->name.length == 5 && memcmp(p->type->name.start, "Slice", 5) == 0) {
            variable->isSlice = 1;
            if (p->type->typeArgs && p->type->typeArgs->length == 1) {
                Type* inner = (Type*)p->type->typeArgs->head->data;
                variable->sliceElemType = inner ? typeToLLVMType(compiler, inner, false) : LLVMInt8TypeInContext(compiler->context);
                variable->sliceElemKind = inner ? inner->kind : TYPE_BYTE;
            } else {
                variable->sliceElemType = LLVMInt8TypeInContext(compiler->context);
                variable->sliceElemKind = TYPE_BYTE;
            }
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
        compilerEmitDropForBlockVars(compiler, funcBlock);
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
    if (compiler->boxedLocals && compiler->boxedLocals != savedBoxedLocals) tokenSetFree(compiler->boxedLocals);
    compiler->boxedLocals = savedBoxedLocals;
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

    StructInfo* info = compilerFindStruct(compiler, stmt->name.start, stmt->name.length);
    if (!info) {
        info = malloc(sizeof(StructInfo));
        info->name = structName;
        info->nameLength = stmt->name.length;
        info->type = structType;
        info->decl = stmt;
        info->methods = listNew();
        listAppend(compiler->structs, info);
    } else {
        free(structName);
        if (!info->methods) info->methods = listNew();
        // Prefer the first declaration we saw; keep existing info->decl.
    }

    // Compile methods as `Struct__method(this: Struct*, ...)`
    if (stmt->methods) {
        // Collect method signatures for trait/promotion checks.
        for (ListNode* node = stmt->methods->head; node != NULL; node = node->next) {
            FuncStmt* method = (FuncStmt*)node->data;
            if (method && info && info->methods) listAppend(info->methods, method);
        }
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
            thisParam->mode = PARAM_CONST;
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
