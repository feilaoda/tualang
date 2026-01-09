#include "llvm.h"
#include "compiler.h"
#include "debug.h"

static Expr* unwrapGroupingExpr(Expr* e);

static int isOptionLLVMType(LLVMTypeRef t);
static LLVMValueRef collapseMultiReturnIfNeeded(Compiler* compiler, LLVMValueRef func, LLVMValueRef call);
static LLVMValueRef getOrCreatePrintf(Compiler* compiler);
static LLVMValueRef getOrCreateTuaPrintValue(Compiler* compiler);
static LLVMTypeRef getPrintfType(Compiler* compiler);
static LLVMValueRef castForPrintf(Compiler* compiler, LLVMValueRef value);
static const char* formatForValue(LLVMValueRef value, int addNewline);

static LLVMValueRef getOrCreateTuaBoxDec(Compiler* compiler) {
    if (!compiler) return NULL;
    LLVMValueRef fn = LLVMGetNamedFunction(compiler->module, "tua_box_dec");
    if (fn) return fn;
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef params[1] = { i8ptr };
    LLVMTypeRef fty = LLVMFunctionType(LLVMVoidTypeInContext(compiler->context), params, 1, 0);
    return LLVMAddFunction(compiler->module, "tua_box_dec", fty);
}

static int exprIsMoveArg(Expr* e) {
    Expr* a = unwrapGroupingExpr(e);
    if (!a) return 0;
    if (a->type != EXPR_UNARY) return 0;
    UnaryExpr* un = (UnaryExpr*)a;
    return un->operator.type == TOKEN_MOVE;
}

static int exprIsVarArg(Expr* e) {
    Expr* a = unwrapGroupingExpr(e);
    if (!a) return 0;
    return a->type == EXPR_VARIABLE;
}

static int endsWithRawN(const char* s, int len, const char* suffix) {
    if (!s || !suffix) return 0;
    int n = len > 0 ? len : (int)strlen(s);
    int m = (int)strlen(suffix);
    if (n < m) return 0;
    return memcmp(s + (n - m), suffix, (size_t)m) == 0;
}

static int aliasIsStdBytesModule(SymbolAlias* a) {
    if (!a || !a->qualified || a->qualifiedLen <= 0) return 0;
    // Module prefix is a sanitized absolute path; for stdlib bytes this suffix is stable.
    return endsWithRawN(a->qualified, a->qualifiedLen, "_std_bytes_tua");
}

static void dropTemporaryClosureArgs(Compiler* compiler, List* argList, LLVMValueRef* args, unsigned argsStartIndex) {
    if (!compiler || !argList || !args) return;
    LLVMTypeRef closureTy = compilerGetClosureType(compiler);
    LLVMValueRef decFn = getOrCreateTuaBoxDec(compiler);
    if (!closureTy || !decFn) return;
    LLVMTypeRef decTy = LLVMGlobalGetValueType(decFn);

    unsigned j = 0;
    for (ListNode* n = argList->head; n != NULL; n = n->next, j++) {
        Expr* argExpr = (Expr*)n->data;
        if (!argExpr) continue;
        if (exprIsVarArg(argExpr)) continue;
        if (exprIsMoveArg(argExpr)) continue;
        LLVMValueRef av = args[argsStartIndex + j];
        if (!av) continue;
        if (LLVMTypeOf(av) != closureTy) continue;
        LLVMValueRef env = LLVMBuildExtractValue(compiler->builder, av, 1, "tmp_env");
        LLVMBuildCall2(compiler->builder, decTy, decFn, &env, 1, "");
    }
}

typedef enum {
    BI_NONE = 0,

    BI_PRINT,
    BI_PRINTLN,
    BI_ASSERT,
    BI_LEN,
    BI_SOME,
    BI_NONE_CTOR,

    BI_TUA_PARSE_INT,
    BI_TUA_FREE,
    BI_TUA_DEADLINE_AFTER_MS,
    BI_TUA_TIME_MONO_NS,
    BI_TUA_TIME_REAL_NS,
    BI_TUA_SLEEP_NS,
    BI_TUA_TIMER_AFTER_MS_CL,
    BI_TUA_TIMER_EVERY_MS_CL,
    BI_TUA_TIMER_EVERY_CANCEL_CL,
    BI_TUA_LOOP_CREATE,
    BI_TUA_LOOP_RUN,
    BI_TUA_LOOP_STOP,
    BI_TUA_LOOP_POST_CL,
    BI_TUA_LOOP_FREE,
    BI_TUA_WORKQUEUE_CREATE,
    BI_TUA_WORKQUEUE_FREE,

    BI_TUA_TCP_LISTEN,
    BI_TUA_TCP_LISTENER_LOCAL_PORT,
    BI_TUA_TCP_LISTENER_CLOSE,
    BI_TUA_TCP_SOCKET_CLOSE,
    BI_TUA_TCP_CONNECT_ASYNC_CL,
    BI_TUA_TCP_CONNECT_PORT_ASYNC_CL,
    BI_TUA_TCP_ACCEPT_START_CL,
    BI_TUA_TCP_ACCEPT_CANCEL_CL,
    BI_TUA_TCP_READ_ALLOC_ASYNC_CL,
    BI_TUA_TCP_WRITE_STR_ASYNC_CL,

    BI_TUA_FS_READFILE_ALLOC,
    BI_TUA_FS_WRITEFILE_STR,
    BI_TUA_FS_STAT_SIMPLE,
    BI_TUA_FS_MKDIR,
    BI_TUA_FS_REALPATH_ALLOC,
    BI_TUA_FS_READDIR_ARR,

    BI_TUA_FS_READFILE_ALLOC_ASYNC_CL,
    BI_TUA_FS_WRITEFILE_STR_ASYNC_CL,
    BI_TUA_FS_STAT_ASYNC_CL,
    BI_TUA_FS_READDIR_ASYNC_CL,
    BI_TUA_FS_STRING_ARRAY_FREE,
} BuiltinId;

typedef struct {
    const char* name;
    uint8_t len;
    BuiltinId id;
} BuiltinEntry;

static BuiltinId lookupBuiltinId(const Token* token) {
    if (!token || !token->start || token->length <= 0) return BI_NONE;
#define BI_ENTRY(s, bid) { (s), (uint8_t)(sizeof(s) - 1), (bid) }
    static const BuiltinEntry builtins[] = {
        BI_ENTRY("Some", BI_SOME),
        BI_ENTRY("None", BI_NONE_CTOR),
        BI_ENTRY("assert", BI_ASSERT),
        BI_ENTRY("len", BI_LEN),
        BI_ENTRY("print", BI_PRINT),
        BI_ENTRY("println", BI_PRINTLN),
    };
    for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++) {
        const BuiltinEntry* e = &builtins[i];
        if (token->length != (int)e->len) continue;
        if (memcmp(token->start, e->name, (size_t)e->len) == 0) return e->id;
    }
#undef BI_ENTRY
    return BI_NONE;
}

static int tokenEquals(const Token* token, const char* s) {
    int n = (int)strlen(s);
    return token->length == n && memcmp(token->start, s, (size_t)n) == 0;
}

typedef enum {
    ARRAY_M_UNKNOWN = 0,
    ARRAY_M_LEN,
    ARRAY_M_CLONE,
    ARRAY_M_PUSH,
    ARRAY_M_SLICE,
} ArrayMethodId;

typedef enum {
    BYTES_M_UNKNOWN = 0,
    BYTES_M_LEN,
    BYTES_M_GET,       // get/getU8
    BYTES_M_SET,       // set/setU8
    BYTES_M_SLICE,
    BYTES_M_COPY,
    BYTES_M_ISREADONLY,
} BytesMethodId;

typedef enum {
    MAP_M_UNKNOWN = 0,
    MAP_M_LEN,
    MAP_M_HASKEY,
    MAP_M_DELETE,
    MAP_M_GET,
    MAP_M_GETMUT,
    MAP_M_GETREF,
    MAP_M_GETREFWRITE,
    MAP_M_CLEAR,
} MapMethodId;

static ArrayMethodId arrayMethodId(const Token* name) {
    if (!name || !name->start) return ARRAY_M_UNKNOWN;
    switch (name->length) {
        case 3:
            if (memcmp(name->start, "len", 3) == 0) return ARRAY_M_LEN;
            break;
        case 4:
            if (memcmp(name->start, "push", 4) == 0) return ARRAY_M_PUSH;
            break;
        case 5:
            if (memcmp(name->start, "clone", 5) == 0) return ARRAY_M_CLONE;
            if (memcmp(name->start, "slice", 5) == 0) return ARRAY_M_SLICE;
            break;
        default:
            break;
    }
    return ARRAY_M_UNKNOWN;
}

static BytesMethodId bytesMethodId(const Token* name) {
    if (!name || !name->start) return BYTES_M_UNKNOWN;
    switch (name->length) {
        case 3:
            if (memcmp(name->start, "len", 3) == 0) return BYTES_M_LEN;
            if (memcmp(name->start, "get", 3) == 0) return BYTES_M_GET;
            if (memcmp(name->start, "set", 3) == 0) return BYTES_M_SET;
            break;
        case 4:
            if (memcmp(name->start, "copy", 4) == 0) return BYTES_M_COPY;
            break;
        case 5:
            if (memcmp(name->start, "getU8", 5) == 0) return BYTES_M_GET;
            if (memcmp(name->start, "setU8", 5) == 0) return BYTES_M_SET;
            if (memcmp(name->start, "slice", 5) == 0) return BYTES_M_SLICE;
            break;
        case 10:
            if (memcmp(name->start, "isReadonly", 10) == 0) return BYTES_M_ISREADONLY;
            break;
        default:
            break;
    }
    return BYTES_M_UNKNOWN;
}

static MapMethodId mapMethodId(const Token* name) {
    if (!name || !name->start) return MAP_M_UNKNOWN;
    switch (name->length) {
        case 3:
            if (memcmp(name->start, "len", 3) == 0) return MAP_M_LEN;
            if (memcmp(name->start, "get", 3) == 0) return MAP_M_GET;
            break;
        case 5:
            if (memcmp(name->start, "clear", 5) == 0) return MAP_M_CLEAR;
            break;
        case 6:
            if (memcmp(name->start, "hasKey", 6) == 0) return MAP_M_HASKEY;
            if (memcmp(name->start, "delete", 6) == 0) return MAP_M_DELETE;
            if (memcmp(name->start, "getMut", 6) == 0) return MAP_M_GETMUT;
            if (memcmp(name->start, "getRef", 6) == 0) return MAP_M_GETREF;
            break;
        case 11:
            if (memcmp(name->start, "getRefWrite", 11) == 0) return MAP_M_GETREFWRITE;
            break;
        default:
            break;
    }
    return MAP_M_UNKNOWN;
}

static char* tokenToCString(const Token* token) {
    char* s = malloc((size_t)token->length + 1);
    memcpy(s, token->start, (size_t)token->length);
    s[token->length] = '\0';
    return s;
}

static char* dupStringLiteralToken(Token token) {
    if (!token.start || token.length < 2) return tokenToCString(&token);
    // Best-effort: strip surrounding quotes without unescaping (consistent with llvm_expr.c).
    int innerLen = token.length - 2;
    if (innerLen < 0) innerLen = 0;
    char* s = malloc((size_t)innerLen + 1);
    memcpy(s, token.start + 1, (size_t)innerLen);
    s[innerLen] = '\0';
    return s;
}

static const char* callInstNameForFnType(LLVMTypeRef fnType) {
    if (!fnType) return "call";
    LLVMTypeRef ret = LLVMGetReturnType(fnType);
    if (ret && LLVMGetTypeKind(ret) == LLVMVoidTypeKind) return "";
    return "call";
}

static LLVMValueRef emitPrintValue(Compiler* compiler, LLVMValueRef argValue, int newline) {
    if (!compiler || !argValue) return NULL;
    LLVMContextRef context = compiler->context;
    LLVMValueRef printfFunc = getOrCreatePrintf(compiler);
    LLVMTypeRef printfType = getPrintfType(compiler);

    if (LLVMTypeOf(argValue) == compilerGetTuaValueType(compiler)) {
        LLVMValueRef fn = getOrCreateTuaPrintValue(compiler);
        LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
        LLVMValueRef nl = LLVMConstInt(LLVMInt32TypeInContext(context), newline ? 1 : 0, 0);
        LLVMValueRef args2[2] = { argValue, nl };
        LLVMBuildCall2(compiler->builder, fnType, fn, args2, 2, "");
        return LLVMConstInt(LLVMInt32TypeInContext(context), 0, 0);
    }

    const char* fmt = formatForValue(argValue, newline ? 1 : 0);
    if (!fmt) return NULL;
    argValue = castForPrintf(compiler, argValue);

    LLVMValueRef formatStr = LLVMBuildGlobalStringPtr(compiler->builder, fmt, "fmt");
    LLVMValueRef args[] = { formatStr, argValue };
    return LLVMBuildCall2(compiler->builder, printfType, printfFunc, args, 2, "");
}

static int countBracePlaceholders(const char* s) {
    if (!s) return 0;
    int n = 0;
    for (const char* p = s; p[0] != '\0'; p++) {
        if (p[0] == '{' && p[1] == '}') {
            n++;
            p++;
        }
    }
    return n;
}

static LLVMValueRef castValueToType(Compiler* compiler, LLVMValueRef value, LLVMTypeRef targetType) {
    if (!value) return NULL;
    LLVMTypeRef srcType = LLVMTypeOf(value);
    if (srcType == targetType) return value;

    LLVMTypeKind srcKind = LLVMGetTypeKind(srcType);
    LLVMTypeKind dstKind = LLVMGetTypeKind(targetType);

    if (isOptionLLVMType(srcType) && isOptionLLVMType(targetType)) {
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

static LLVMTypeRef typeToLLVMType(Compiler* compiler, Type* type) {
    if (!type) return LLVMInt32TypeInContext(compiler->context);
    Type* subst = compilerResolveGenericType(compiler, type);
    if (subst && subst != type) return typeToLLVMType(compiler, subst);
    switch (type->kind) {
        case TYPE_ANY:
            return compilerGetTuaValueType(compiler);
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
        case TYPE_USIZE:
            return LLVMIntTypeInContext(compiler->context, (unsigned)(sizeof(void*) * 8));
        case TYPE_F16:
            return LLVMHalfTypeInContext(compiler->context);
        case TYPE_DOUBLE:
            return LLVMDoubleTypeInContext(compiler->context);
        case TYPE_FLOAT:
            return LLVMFloatTypeInContext(compiler->context);
        case TYPE_BF16:
            return LLVMBFloatTypeInContext(compiler->context);
        case TYPE_BOOL: return LLVMInt1TypeInContext(compiler->context);
        case TYPE_STRING: return LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
        case TYPE_PTR: return LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
        case TYPE_NAMED: {
            TraitInfo* ti = compilerResolveTraitByToken(compiler, &type->name);
            if (ti) return compilerGetTraitObjType(compiler, ti);
            if (type->name.length == 3 && memcmp(type->name.start, "any", 3) == 0) {
                return compilerGetTuaValueType(compiler);
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
                LLVMTypeRef innerTy = inner ? typeToLLVMType(compiler, inner) : LLVMInt8TypeInContext(compiler->context);
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
                LLVMTypeRef innerTy = typeToLLVMType(compiler, inner);
                return compilerGetOptionType(compiler, innerTy);
            }
            StructInfo* info = compilerResolveStructByToken(compiler, &type->name);
            if (!info) {
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
            LLVMTypeRef inner = typeToLLVMType(compiler, type->inner);
            return LLVMPointerType(inner, 0);
        }
        case TYPE_FUNC:
            return compilerGetClosureType(compiler);
        default: return LLVMInt32TypeInContext(compiler->context);
    }
}

static Expr* unwrapGroupingExpr(Expr* e);
static Type* cloneTypeTreeDeep(const Type* t);

static Type* inferTypeFromValueExpr(Compiler* compiler, Expr* e) {
    if (!compiler || !e) return NULL;
    e = unwrapGroupingExpr(e);
    if (!e) return NULL;

    if (e->type == EXPR_LITERAL) {
        LiteralExpr* lit = (LiteralExpr*)e;
        TypeKind k = TYPE_VOID;
        switch (lit->value.type) {
            case TOKEN_INT: k = TYPE_INT; break;
            case TOKEN_LONG: k = TYPE_LONG; break;
            case TOKEN_FLOAT: k = TYPE_FLOAT; break;
            case TOKEN_DOUBLE: k = TYPE_DOUBLE; break;
            case TOKEN_STRING_LITERAL: k = TYPE_STRING; break;
            case TOKEN_TRUE:
            case TOKEN_FALSE:
                k = TYPE_BOOL;
                break;
            default:
                break;
        }
        if (k != TYPE_VOID) {
            Type* t = malloc(sizeof(Type));
            memset(t, 0, sizeof(*t));
            t->kind = k;
            return t;
        }
    }

    if (e->type == EXPR_VARIABLE) {
        VariableRef v = findVariableExpr(compiler, e);
        if (v.astType) {
            return cloneTypeTreeDeep(v.astType);
        }
        if (v.typeName && v.typeNameLength > 0) {
            Type* t = malloc(sizeof(Type));
            memset(t, 0, sizeof(*t));
            t->kind = TYPE_NAMED;
            t->name = (Token){TOKEN_IDENTIFIER, v.typeName, v.typeNameLength, e->token.line, e->token.col, 0};
            return t;
        }
        TypeKind k = e->inferredType;
        if (k == TYPE_INT || k == TYPE_LONG || k == TYPE_BOOL || k == TYPE_STRING ||
            k == TYPE_FLOAT || k == TYPE_DOUBLE ||
            k == TYPE_I8 || k == TYPE_I16 || k == TYPE_ISIZE ||
            k == TYPE_U8 || k == TYPE_U16 || k == TYPE_U32 || k == TYPE_U64 || k == TYPE_USIZE ||
            k == TYPE_BYTE || k == TYPE_F16 || k == TYPE_BF16 ||
            k == TYPE_F8 || k == TYPE_BF8) {
            Type* t = malloc(sizeof(Type));
            memset(t, 0, sizeof(*t));
            t->kind = k;
            return t;
        }
        return NULL;
    }

    // Best-effort: treat `TypeName(...)` and `ns.TypeName(...)` as constructing a named type.
    if (e->type == EXPR_CALL) {
        CallExpr* c = (CallExpr*)e;
        Expr* callee = c->callee ? unwrapGroupingExpr(c->callee) : NULL;

        // Builtins: `Some(x)` / `None()` => Option<T>.
        if (callee && callee->type == EXPR_VARIABLE) {
            VariableExpr* ve = (VariableExpr*)callee;
            if (tokenEquals(&ve->name, "Some") || tokenEquals(&ve->name, "None")) {
                Type* inner = NULL;
                if (tokenEquals(&ve->name, "Some")) {
                    Expr* arg0 = c->arguments && c->arguments->head ? (Expr*)c->arguments->head->data : NULL;
                    inner = inferTypeFromValueExpr(compiler, arg0);
                }
                if (!inner) {
                    inner = (Type*)malloc(sizeof(Type));
                    memset(inner, 0, sizeof(*inner));
                    inner->kind = TYPE_ANY;
                }

                Type* opt = (Type*)malloc(sizeof(Type));
                memset(opt, 0, sizeof(*opt));
                opt->kind = TYPE_NAMED;
                opt->name = (Token){TOKEN_IDENTIFIER, "Option", 6, e->token.line, e->token.col, 0};
                opt->typeArgs = listNew();
                listAppend(opt->typeArgs, inner);
                return opt;
            }
        }

        // Best-effort: treat `x.slice(...)` on bytes/arrays as producing `Slice<T>`.
        if (callee && callee->type == EXPR_GET) {
            GetExpr* ge = (GetExpr*)callee;
            if (tokenEquals(&ge->name, "slice")) {
                Expr* recv = unwrapGroupingExpr(ge->object);
                Type* inner = NULL;
                if (recv && recv->type == EXPR_VARIABLE) {
                    VariableRef base = findVariableExpr(compiler, recv);
                    if (base.isBytes) {
                        inner = (Type*)malloc(sizeof(Type));
                        memset(inner, 0, sizeof(*inner));
                        inner->kind = TYPE_BYTE;
                    } else if (base.isArray && base.astType && base.astType->kind == TYPE_ARRAY) {
                        inner = cloneTypeTreeDeep(base.astType->inner);
                    }
                }
                if (!inner) {
                    inner = (Type*)malloc(sizeof(Type));
                    memset(inner, 0, sizeof(*inner));
                    inner->kind = TYPE_ANY;
                }
                Type* st = (Type*)malloc(sizeof(Type));
                memset(st, 0, sizeof(*st));
                st->kind = TYPE_NAMED;
                st->name = (Token){TOKEN_IDENTIFIER, "Slice", 5, e->token.line, e->token.col, 0};
                st->typeArgs = listNew();
                listAppend(st->typeArgs, inner);
                return st;
            }
        }

        if (callee && callee->type == EXPR_VARIABLE) {
            VariableExpr* ve = (VariableExpr*)callee;
            StructInfo* si = compilerResolveStructByToken(compiler, &ve->name);
            if (si) {
                Type* t = malloc(sizeof(Type));
                memset(t, 0, sizeof(*t));
                t->kind = TYPE_NAMED;
                t->name = (Token){TOKEN_IDENTIFIER, si->name, si->nameLength, e->token.line, e->token.col, 0};
                return t;
            }
        }
        if (callee && callee->type == EXPR_GET) {
            GetExpr* ge = (GetExpr*)callee;
            // `ns.S(...)` where ge->name is S
            StructInfo* si = compilerResolveStructByToken(compiler, &ge->name);
            if (si) {
                Type* t = malloc(sizeof(Type));
                memset(t, 0, sizeof(*t));
                t->kind = TYPE_NAMED;
                t->name = (Token){TOKEN_IDENTIFIER, si->name, si->nameLength, e->token.line, e->token.col, 0};
                return t;
            }
        }
    }

    if (e->type == EXPR_STRUCT_INIT) {
        StructInitExpr* si = (StructInitExpr*)e;
        Expr* callee = si->callee ? unwrapGroupingExpr(si->callee) : NULL;
        if (callee && callee->type == EXPR_VARIABLE) {
            VariableExpr* ve = (VariableExpr*)callee;
            StructInfo* info = compilerResolveStructByToken(compiler, &ve->name);
            if (info) {
                Type* t = malloc(sizeof(Type));
                memset(t, 0, sizeof(*t));
                t->kind = TYPE_NAMED;
                t->name = (Token){TOKEN_IDENTIFIER, info->name, info->nameLength, e->token.line, e->token.col, 0};
                return t;
            }
        }
        if (callee && callee->type == EXPR_GET) {
            GetExpr* ge = (GetExpr*)callee;
            StructInfo* info = compilerResolveStructByToken(compiler, &ge->name);
            if (info) {
                Type* t = malloc(sizeof(Type));
                memset(t, 0, sizeof(*t));
                t->kind = TYPE_NAMED;
                t->name = (Token){TOKEN_IDENTIFIER, info->name, info->nameLength, e->token.line, e->token.col, 0};
                return t;
            }
        }
    }

    // Fallback for literals based on inferred kind.
    TypeKind k = e->inferredType;
    if (k == TYPE_INT || k == TYPE_LONG || k == TYPE_BOOL || k == TYPE_STRING ||
        k == TYPE_FLOAT || k == TYPE_DOUBLE) {
        Type* t = malloc(sizeof(Type));
        memset(t, 0, sizeof(*t));
        t->kind = k;
        return t;
    }
    return NULL;
}

static int astNamedTypeEqualsName(Type* t, const char* name, int nameLen) {
    if (!t || t->kind != TYPE_NAMED) return 0;
    if (t->typeArgs && t->typeArgs->length > 0) return 0;
    if (t->name.length != nameLen) return 0;
    return memcmp(t->name.start, name, (size_t)nameLen) == 0;
}

static int astTypeEqualsDeep(Type* a, Type* b) {
    if (a == b) return 1;
    if (!a || !b) return 0;
    if (a->kind != b->kind) return 0;
    switch (a->kind) {
        case TYPE_NAMED: {
            if (a->name.length != b->name.length) return 0;
            if (memcmp(a->name.start, b->name.start, (size_t)a->name.length) != 0) return 0;
            int ac = a->typeArgs ? a->typeArgs->length : 0;
            int bc = b->typeArgs ? b->typeArgs->length : 0;
            if (ac != bc) return 0;
            for (int i = 0; i < ac; i++) {
                if (!astTypeEqualsDeep((Type*)listGet(a->typeArgs, i), (Type*)listGet(b->typeArgs, i))) return 0;
            }
            return 1;
        }
        case TYPE_REF:
            return astTypeEqualsDeep(a->inner, b->inner);
        case TYPE_ARRAY:
            if (a->arrayLen != b->arrayLen) return 0;
            return astTypeEqualsDeep(a->inner, b->inner);
        case TYPE_FUNC:
            // Keep coarse equality for now; generic inference does not depend on deep func-type matching.
            return 1;
        default:
            // scalar/builtin kinds compare by kind only.
            return 1;
    }
}

typedef enum {
    GEN_INFER_OK = 0,
    GEN_INFER_NOT_APPLICABLE,
    GEN_INFER_ARGC_MISMATCH,
    GEN_INFER_MISSING_TP,
    GEN_INFER_CONFLICT,
    GEN_INFER_TRAIT_OBJECT_ARG,
    GEN_INFER_UNSUPPORTED_SHAPE
} GenericInferStatus;

typedef struct {
    GenericInferStatus status;
    int expectedTypeParams;
    int argc;
    int pc;
    int tpIndex;        // which type param (0-based), when applicable
    int firstArgIndex;  // 1-based
    int secondArgIndex; // 1-based
    Token tpName;       // type param name token (best-effort)
} GenericInferDiag;

static void initGenericInferDiag(GenericInferDiag* d) {
    if (!d) return;
    memset(d, 0, sizeof(*d));
    d->status = GEN_INFER_NOT_APPLICABLE;
    d->tpIndex = -1;
    d->firstArgIndex = 0;
    d->secondArgIndex = 0;
}

static void freeTypeTreeDeep(Type* t) {
    if (!t) return;
    if (t->inner) freeTypeTreeDeep(t->inner);
    if (t->typeArgs) {
        for (ListNode* n = t->typeArgs->head; n != NULL; n = n->next) {
            freeTypeTreeDeep((Type*)n->data);
        }
        listFree(t->typeArgs);
    }
    if (t->paramTypes) {
        for (ListNode* n = t->paramTypes->head; n != NULL; n = n->next) {
            freeTypeTreeDeep((Type*)n->data);
        }
        listFree(t->paramTypes);
    }
    if (t->returnTypes) {
        for (ListNode* n = t->returnTypes->head; n != NULL; n = n->next) {
            freeTypeTreeDeep((Type*)n->data);
        }
        listFree(t->returnTypes);
    }
    free(t);
}

static Type* cloneTypeTreeDeep(const Type* t) {
    if (!t) return NULL;
    Type* out = (Type*)malloc(sizeof(Type));
    memset(out, 0, sizeof(*out));
    out->kind = t->kind;
    out->name = t->name;
    out->arrayLen = t->arrayLen;
    out->inner = t->inner ? cloneTypeTreeDeep(t->inner) : NULL;

    if (t->typeArgs && t->typeArgs->length > 0) {
        out->typeArgs = listNew();
        for (ListNode* n = t->typeArgs->head; n != NULL; n = n->next) {
            listAppend(out->typeArgs, cloneTypeTreeDeep((Type*)n->data));
        }
    }
    if (t->paramTypes && t->paramTypes->length > 0) {
        out->paramTypes = listNew();
        for (ListNode* n = t->paramTypes->head; n != NULL; n = n->next) {
            listAppend(out->paramTypes, cloneTypeTreeDeep((Type*)n->data));
        }
    }
    if (t->returnTypes && t->returnTypes->length > 0) {
        out->returnTypes = listNew();
        for (ListNode* n = t->returnTypes->head; n != NULL; n = n->next) {
            listAppend(out->returnTypes, cloneTypeTreeDeep((Type*)n->data));
        }
    }
    return out;
}

static int astTypeContainsTypeParamDeep(Type* t, const Token* tpName) {
    if (!t || !tpName || !tpName->start || tpName->length <= 0) return 0;
    switch (t->kind) {
        case TYPE_NAMED: {
            if (t->name.length == tpName->length &&
                memcmp(t->name.start, tpName->start, (size_t)tpName->length) == 0) {
                // If this is `T` itself, it counts; if it's `Option<T>`, we still recurse below.
                if (!t->typeArgs || t->typeArgs->length == 0) return 1;
            }
            for (ListNode* n = t->typeArgs ? t->typeArgs->head : NULL; n != NULL; n = n->next) {
                if (astTypeContainsTypeParamDeep((Type*)n->data, tpName)) return 1;
            }
            return 0;
        }
        case TYPE_REF:
            return astTypeContainsTypeParamDeep(t->inner, tpName);
        case TYPE_ARRAY:
            return astTypeContainsTypeParamDeep(t->inner, tpName);
        case TYPE_FUNC: {
            for (ListNode* n = t->paramTypes ? t->paramTypes->head : NULL; n != NULL; n = n->next) {
                if (astTypeContainsTypeParamDeep((Type*)n->data, tpName)) return 1;
            }
            for (ListNode* n = t->returnTypes ? t->returnTypes->head : NULL; n != NULL; n = n->next) {
                if (astTypeContainsTypeParamDeep((Type*)n->data, tpName)) return 1;
            }
            if (t->inner && astTypeContainsTypeParamDeep(t->inner, tpName)) return 1;
            return 0;
        }
        default:
            return 0;
    }
}

static List* inferTypeArgsForGenericCall(Compiler* compiler, GenericFuncTemplate* tmpl, CallExpr* call, GenericInferDiag* diag) {
    initGenericInferDiag(diag);
    if (!compiler || !tmpl || !tmpl->decl || !call) return NULL;
    int expected = tmpl->decl->typeParams ? tmpl->decl->typeParams->length : 0;
    if (expected <= 0) return NULL;
    if (diag) {
        diag->status = GEN_INFER_NOT_APPLICABLE;
        diag->expectedTypeParams = expected;
    }

    int argc = call->arguments ? call->arguments->length : 0;
    int pc = tmpl->decl->params ? tmpl->decl->params->length : 0;
    if (argc != pc) {
        if (diag) {
            diag->status = GEN_INFER_ARGC_MISMATCH;
            diag->argc = argc;
            diag->pc = pc;
        }
        return NULL;
    }

    Type** inferred = malloc(sizeof(Type*) * (size_t)expected);
    int* inferredFromArg = malloc(sizeof(int) * (size_t)expected);
    int* nestedSeen = malloc(sizeof(int) * (size_t)expected);
    int* nestedFromArg = malloc(sizeof(int) * (size_t)expected);
    for (int i = 0; i < expected; i++) inferred[i] = NULL;
    for (int i = 0; i < expected; i++) inferredFromArg[i] = -1;
    for (int i = 0; i < expected; i++) nestedSeen[i] = 0;
    for (int i = 0; i < expected; i++) nestedFromArg[i] = -1;

    for (int i = 0; i < pc; i++) {
        Parameter* p = (Parameter*)listGet(tmpl->decl->params, i);
        Expr* arg = (Expr*)listGet(call->arguments, i);
        if (!p || !p->type) continue;

        Type* pt = compilerResolveGenericType(compiler, p->type);

	        // v0.6: support inference through a single-layer generic wrapper for builtin types:
	        // - `Option<T>` from an argument of type `Option<U>`
	        // - `Slice<T>`  from an argument of type `Slice<U>`
	        if (pt && pt->kind == TYPE_NAMED && pt->typeArgs && pt->typeArgs->length == 1) {
	            int isOption = (pt->name.length == 6 && memcmp(pt->name.start, "Option", 6) == 0);
	            int isSlice = (pt->name.length == 5 && memcmp(pt->name.start, "Slice", 5) == 0);
	            if (isOption || isSlice) {
                Type* targ0 = (Type*)pt->typeArgs->head->data;
                if (targ0 && targ0->kind == TYPE_NAMED && (!targ0->typeArgs || targ0->typeArgs->length == 0)) {
                    int tpIndex = -1;
                    for (int j = 0; j < expected; j++) {
                        TypeParamDecl* td = (TypeParamDecl*)listGet(tmpl->decl->typeParams, j);
                        if (!td) continue;
                        if (td->name.length == targ0->name.length &&
                            memcmp(td->name.start, targ0->name.start, (size_t)td->name.length) == 0) {
                            tpIndex = j;
                            break;
                        }
                    }

                    if (tpIndex >= 0) {
                        Type* at = inferTypeFromValueExpr(compiler, arg);
                        Type* innerInferred = NULL;
                        if (at && at->kind == TYPE_NAMED && at->typeArgs && at->typeArgs->length == 1 &&
                            at->name.length == pt->name.length &&
                            memcmp(at->name.start, pt->name.start, (size_t)pt->name.length) == 0) {
                            innerInferred = cloneTypeTreeDeep((Type*)at->typeArgs->head->data);
                        }
                        if (at) freeTypeTreeDeep(at);

                        if (innerInferred) {
                            if (!inferred[tpIndex]) {
                                inferred[tpIndex] = innerInferred;
                                inferredFromArg[tpIndex] = i;
                            } else if (inferred[tpIndex]->kind == TYPE_ANY && innerInferred->kind != TYPE_ANY) {
                                freeTypeTreeDeep(inferred[tpIndex]);
                                inferred[tpIndex] = innerInferred;
                                inferredFromArg[tpIndex] = i;
                            } else if (innerInferred->kind == TYPE_ANY && inferred[tpIndex]->kind != TYPE_ANY) {
                                freeTypeTreeDeep(innerInferred);
                            } else if (!astTypeEqualsDeep(inferred[tpIndex], innerInferred)) {
                                for (int j = 0; j < expected; j++) {
                                    if (inferred[j]) freeTypeTreeDeep(inferred[j]);
                                }
                                freeTypeTreeDeep(innerInferred);
                                free(inferredFromArg);
                                free(inferred);
                                free(nestedSeen);
                                free(nestedFromArg);
                                if (diag) {
                                    diag->status = GEN_INFER_CONFLICT;
                                    diag->tpIndex = tpIndex;
                                    diag->firstArgIndex = inferredFromArg[tpIndex] + 1;
                                    diag->secondArgIndex = i + 1;
                                }
                                return NULL;
                            } else {
                                freeTypeTreeDeep(innerInferred);
                            }
                        }
                    }
                }
	            }
	        }

	        // `array<T>` / `T[]`: infer T from an argument of type `array<U>` / `U[]`.
	        if (pt && pt->kind == TYPE_ARRAY && pt->inner && pt->inner->kind == TYPE_NAMED &&
	            (!pt->inner->typeArgs || pt->inner->typeArgs->length == 0)) {
	            Type* targ0 = pt->inner;
	            int tpIndex = -1;
	            for (int j = 0; j < expected; j++) {
	                TypeParamDecl* td = (TypeParamDecl*)listGet(tmpl->decl->typeParams, j);
	                if (!td) continue;
	                if (td->name.length == targ0->name.length &&
	                    memcmp(td->name.start, targ0->name.start, (size_t)td->name.length) == 0) {
	                    tpIndex = j;
	                    break;
	                }
	            }

	            if (tpIndex >= 0) {
	                Type* at = inferTypeFromValueExpr(compiler, arg);
	                Type* innerInferred = NULL;
	                if (at && at->kind == TYPE_ARRAY && at->inner) {
	                    innerInferred = cloneTypeTreeDeep(at->inner);
	                }
	                if (at) freeTypeTreeDeep(at);

	                if (innerInferred) {
	                    if (!inferred[tpIndex]) {
	                        inferred[tpIndex] = innerInferred;
	                        inferredFromArg[tpIndex] = i;
	                    } else if (inferred[tpIndex]->kind == TYPE_ANY && innerInferred->kind != TYPE_ANY) {
	                        freeTypeTreeDeep(inferred[tpIndex]);
	                        inferred[tpIndex] = innerInferred;
	                        inferredFromArg[tpIndex] = i;
	                    } else if (innerInferred->kind == TYPE_ANY && inferred[tpIndex]->kind != TYPE_ANY) {
	                        freeTypeTreeDeep(innerInferred);
	                    } else if (!astTypeEqualsDeep(inferred[tpIndex], innerInferred)) {
	                        for (int j = 0; j < expected; j++) {
	                            if (inferred[j]) freeTypeTreeDeep(inferred[j]);
	                        }
	                        freeTypeTreeDeep(innerInferred);
	                        free(inferredFromArg);
	                        free(inferred);
	                        free(nestedSeen);
	                        free(nestedFromArg);
	                        if (diag) {
	                            diag->status = GEN_INFER_CONFLICT;
	                            diag->tpIndex = tpIndex;
	                            diag->firstArgIndex = inferredFromArg[tpIndex] + 1;
	                            diag->secondArgIndex = i + 1;
	                        }
	                        return NULL;
	                    } else {
	                        freeTypeTreeDeep(innerInferred);
	                    }
	                }
	            }
	        }

	        // Only infer for top-level occurrences of type params: `x: T` or `x: Ref<T>` (aka `&T`).
	        const char* tpName = NULL;
	        int tpLen = 0;
        if (pt && pt->kind == TYPE_NAMED) {
            tpName = pt->name.start;
            tpLen = pt->name.length;
        } else if (pt && pt->kind == TYPE_REF && pt->inner && pt->inner->kind == TYPE_NAMED) {
            tpName = pt->inner->name.start;
            tpLen = pt->inner->name.length;
        }
        // Record nested occurrences of type params so we can produce a better error when inference fails.
        // We do this for all non-trivial parameter types, not just ones with no top-level name.
        for (int j = 0; j < expected; j++) {
            TypeParamDecl* td = (TypeParamDecl*)listGet(tmpl->decl->typeParams, j);
            if (!td) continue;
            // Skip the supported top-level shapes `T` / `&T`; those are handled below.
            int isTopLevel = 0;
            if (tpName && tpLen > 0 && td->name.length == tpLen && memcmp(td->name.start, tpName, (size_t)tpLen) == 0) {
                isTopLevel = 1;
            }
            if (!isTopLevel && astTypeContainsTypeParamDeep(pt, &td->name)) {
                nestedSeen[j] = 1;
                if (nestedFromArg[j] < 0) nestedFromArg[j] = i;
            }
        }
        if (!tpName || tpLen <= 0) continue;

        int tpIndex = -1;
        for (int j = 0; j < expected; j++) {
            TypeParamDecl* td = (TypeParamDecl*)listGet(tmpl->decl->typeParams, j);
            if (!td) continue;
            if (td->name.length == tpLen && memcmp(td->name.start, tpName, (size_t)tpLen) == 0) {
                tpIndex = j;
                break;
            }
        }
        if (tpIndex < 0) continue;
        if (diag) {
            diag->tpName = ((TypeParamDecl*)listGet(tmpl->decl->typeParams, tpIndex))->name;
        }

        Type* it = inferTypeFromValueExpr(compiler, arg);
        if (!it) continue;
        // If the argument's inferred type is itself a trait object type, do not try to
        // instantiate `T: Trait` with `T = TraitName`; let normal (dynamic) resolution win.
        if ((it->kind == TYPE_NAMED && compilerResolveTraitByToken(compiler, &it->name)) ||
            (it->kind == TYPE_REF && it->inner && it->inner->kind == TYPE_NAMED && compilerResolveTraitByToken(compiler, &it->inner->name))) {
            for (int j = 0; j < expected; j++) {
                if (inferred[j]) freeTypeTreeDeep(inferred[j]);
            }
            freeTypeTreeDeep(it);
            free(inferredFromArg);
            free(inferred);
            free(nestedSeen);
            free(nestedFromArg);
            if (diag) {
                diag->status = GEN_INFER_TRAIT_OBJECT_ARG;
                diag->tpIndex = tpIndex;
                diag->secondArgIndex = i + 1;
            }
            return NULL;
        }
        if (!inferred[tpIndex]) {
            inferred[tpIndex] = it;
            inferredFromArg[tpIndex] = i;
        } else if (inferred[tpIndex]->kind == TYPE_ANY && it->kind != TYPE_ANY) {
            // Upgrade from wildcard `any` to a concrete inference.
            freeTypeTreeDeep(inferred[tpIndex]);
            inferred[tpIndex] = it;
            inferredFromArg[tpIndex] = i;
        } else if (it->kind == TYPE_ANY && inferred[tpIndex]->kind != TYPE_ANY) {
            // Keep existing concrete inference.
            freeTypeTreeDeep(it);
        } else {
            // Consistency check across arguments.
            if (!astTypeEqualsDeep(inferred[tpIndex], it)) {
                for (int j = 0; j < expected; j++) {
                    if (inferred[j]) freeTypeTreeDeep(inferred[j]);
                }
                freeTypeTreeDeep(it);
                free(inferredFromArg);
                free(inferred);
                free(nestedSeen);
                free(nestedFromArg);
                if (diag) {
                    diag->status = GEN_INFER_CONFLICT;
                    diag->tpIndex = tpIndex;
                    diag->firstArgIndex = inferredFromArg[tpIndex] + 1;
                    diag->secondArgIndex = i + 1;
                }
                return NULL;
            }
            freeTypeTreeDeep(it);
        }
    }

    List* out = listNew();
    for (int i = 0; i < expected; i++) {
        if (!inferred[i]) {
            listFree(out);
            for (int j = 0; j < expected; j++) {
                if (inferred[j]) freeTypeTreeDeep(inferred[j]);
            }
            free(inferredFromArg);
            free(inferred);
            if (nestedSeen[i]) {
                if (diag) {
                    diag->status = GEN_INFER_UNSUPPORTED_SHAPE;
                    diag->tpIndex = i;
                    TypeParamDecl* td = (TypeParamDecl*)listGet(tmpl->decl->typeParams, i);
                    if (td) diag->tpName = td->name;
                    diag->secondArgIndex = nestedFromArg[i] + 1;
                }
            } else if (diag) {
                diag->status = GEN_INFER_MISSING_TP;
                diag->tpIndex = i;
                TypeParamDecl* td = (TypeParamDecl*)listGet(tmpl->decl->typeParams, i);
                if (td) diag->tpName = td->name;
            }
            free(nestedSeen);
            free(nestedFromArg);
            return NULL;
        }
        listAppend(out, inferred[i]);
    }
    if (diag) diag->status = GEN_INFER_OK;
    free(inferredFromArg);
    free(inferred);
    free(nestedSeen);
    free(nestedFromArg);
    return out;
}

static LLVMValueRef castForPrintf(Compiler* compiler, LLVMValueRef value) {
    if (!value) return NULL;

    LLVMTypeRef type = LLVMTypeOf(value);
    LLVMTypeKind kind = LLVMGetTypeKind(type);
    if (kind == LLVMFloatTypeKind) {
        return LLVMBuildFPExt(compiler->builder, value, LLVMDoubleTypeInContext(compiler->context), "fpext_printf");
    }
    if (kind == LLVMDoubleTypeKind) return value;
    if (kind != LLVMIntegerTypeKind) return value;

    unsigned bits = LLVMGetIntTypeWidth(type);
    if (bits < 32) {
        return LLVMBuildZExt(compiler->builder, value, LLVMInt32TypeInContext(compiler->context), "zext_printf");
    }
    if (bits == 32) return value;
    if (bits < 64) {
        return LLVMBuildSExt(compiler->builder, value, LLVMInt64TypeInContext(compiler->context), "sext_printf");
    }
    return value;
}

static char* mangleRawAndToken(const char* left, int leftLen, const Token* right, int* outLen);

static LLVMValueRef getOrCreateMalloc(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "malloc");
    if (existing) return existing;
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef i64 = LLVMInt64TypeInContext(compiler->context);
    LLVMTypeRef fnType = LLVMFunctionType(i8ptr, &i64, 1, 0);
    return LLVMAddFunction(compiler->module, "malloc", fnType);
}

static TraitInfo* findTraitByObjType(Compiler* compiler, LLVMTypeRef t) {
    if (!compiler || !compiler->traits || !t) return NULL;
    for (ListNode* n = compiler->traits->head; n != NULL; n = n->next) {
        TraitInfo* ti = (TraitInfo*)n->data;
        if (!ti) continue;
        if (!ti->objType) continue;
        if (ti->objType == t) return ti;
    }
    return NULL;
}

static char* vtableGlobalNameForTraitAndStruct(const char* traitName, int traitLen, const char* structName, int structLen) {
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

static int traitMethodIndex(TraitInfo* trait, const Token* methodName) {
    if (!trait || !trait->decl || !methodName) return -1;
    int idx = 0;
    for (ListNode* mn = trait->decl->methods ? trait->decl->methods->head : NULL; mn != NULL; mn = mn->next, idx++) {
        TraitMethodDecl* m = (TraitMethodDecl*)mn->data;
        if (!m) continue;
        if (m->name.length != methodName->length) continue;
        if (memcmp(m->name.start, methodName->start, (size_t)methodName->length) == 0) return idx;
    }
    return -1;
}

static TraitMethodDecl* traitMethodDeclAt(TraitInfo* trait, int idx) {
    if (!trait || !trait->decl || idx < 0) return NULL;
    return trait->decl->methods ? (TraitMethodDecl*)listGet(trait->decl->methods, idx) : NULL;
}

static int astTypeIsNamedStructValueForCall(Compiler* compiler, Type* t) {
    if (!compiler || !t) return 0;
    t = compilerResolveGenericType(compiler, t);
    if (!t || t->kind != TYPE_NAMED) return 0;
    if (t->typeArgs && t->typeArgs->length > 0) return 0;
    if (t->name.length == 3 && memcmp(t->name.start, "map", 3) == 0) return 0;
    if (t->name.length == 6 && memcmp(t->name.start, "Option", 6) == 0) return 0;
    if (t->name.length == 3 && memcmp(t->name.start, "ptr", 3) == 0) return 0;
    if (compilerResolveTraitByToken(compiler, &t->name)) return 0;
    StructInfo* si = compilerResolveStructByToken(compiler, &t->name);
    return si != NULL;
}

static LLVMTypeRef llvmReturnTypeForTraitMethod(Compiler* compiler, TraitMethodDecl* m) {
    if (!compiler || !m) return LLVMVoidTypeInContext(compiler->context);
    int rc = m->returnTypes ? m->returnTypes->length : 0;
    if (rc <= 0) return LLVMVoidTypeInContext(compiler->context);
    if (rc == 1) {
        Type* t = (Type*)listGet(m->returnTypes, 0);
        return typeToLLVMType(compiler, t);
    }
    LLVMTypeRef* rts = malloc(sizeof(LLVMTypeRef) * (size_t)rc);
    for (int i = 0; i < rc; i++) {
        Type* t = (Type*)listGet(m->returnTypes, i);
        rts[i] = typeToLLVMType(compiler, t);
    }
    LLVMTypeRef out = LLVMStructTypeInContext(compiler->context, rts, (unsigned)rc, 0);
    free(rts);
    return out;
}

static LLVMTypeRef llvmParamTypeForTraitMethod(Compiler* compiler, Parameter* p) {
    if (!compiler || !p) return LLVMInt32TypeInContext(compiler->context);
    LLVMTypeRef pt = typeToLLVMType(compiler, p->type);
    if (p->mode != PARAM_MOVE && astTypeIsNamedStructValueForCall(compiler, p->type)) {
        pt = LLVMPointerType(pt, 0);
    }
    return pt;
}

static LLVMTypeRef resolveClosureReturnSigForSimpleCallAt(Compiler* compiler, CallExpr* call, int level) {
    if (!compiler || !call || !call->callee) return NULL;
    if (call->callee->type != EXPR_VARIABLE) return NULL;

    VariableExpr* callee = (VariableExpr*)call->callee;

    // Inside an `object` method, allow unqualified calls to refer to sibling methods,
    // enabling patterns like `makeAdder(1)(2)` where `makeAdder` is an object method.
    int isSelf = compiler->currentObjectPrefix &&
                 compiler->currentObjectMethodName &&
                 compiler->currentObjectMethodNameLen == callee->name.length &&
                 memcmp(compiler->currentObjectMethodName, callee->name.start, (size_t)callee->name.length) == 0;
    if (compiler->currentObjectPrefix && !isSelf) {
        int ql = 0;
        char* q = mangleRawAndToken(compiler->currentObjectPrefix, compiler->currentObjectPrefixLen, &callee->name, &ql);
        if (q) {
            LLVMTypeRef t = compilerFindClosureReturnSigAt(compiler, q, ql, level);
            free(q);
            if (t) return t;
        }
    }

    SymbolAlias* a = compilerFindAlias(compiler, callee->name.start, callee->name.length);
    if (a && a->kind == ALIAS_FUNC) {
        return compilerFindClosureReturnSigAt(compiler, a->qualified, a->qualifiedLen, level);
    }

    if (compiler->currentModulePrefix) {
        int ql = 0;
        char* q = compilerQualifyToken(compiler, &callee->name, &ql);
        if (q) {
            LLVMTypeRef t = compilerFindClosureReturnSigAt(compiler, q, ql, level);
            free(q);
            if (t) return t;
        }
    }

    // For self-recursion inside an object method, prefer module/global resolution first
    // (so wrappers can call same-named `extern fn`), then fall back to the object method.
    if (compiler->currentObjectPrefix && isSelf) {
        int ql = 0;
        char* q = mangleRawAndToken(compiler->currentObjectPrefix, compiler->currentObjectPrefixLen, &callee->name, &ql);
        if (q) {
            LLVMTypeRef t = compilerFindClosureReturnSigAt(compiler, q, ql, level);
            free(q);
            if (t) return t;
        }
    }

    return compilerFindClosureReturnSigAt(compiler, callee->name.start, callee->name.length, level);
}

static LLVMTypeRef resolveClosureReturnSigForSimpleCall(Compiler* compiler, CallExpr* call) {
    return resolveClosureReturnSigForSimpleCallAt(compiler, call, 0);
}

static Expr* unwrapGroupingExpr(Expr* e) {
    while (e && e->type == EXPR_GROUPING) {
        e = ((GroupingExpr*)e)->expression;
    }
    return e;
}

static LLVMValueRef implicitBorrowAddrOfVar(Compiler* compiler, VariableRef var) {
    if (!compiler || !var.value) return NULL;
    if (var.isBoxed) {
        if (!var.boxPtrType) {
            emitDebug("Missing boxed pointer type metadata\n");
            return NULL;
        }
        return LLVMBuildLoad2(compiler->builder, var.boxPtrType, var.value, "boxptr");
    }
    // Slot is already a pointer to the value.
    return var.value;
}

static LLVMValueRef implicitBorrowAddrOfVar(Compiler* compiler, VariableRef var);
static LLVMValueRef loadLocalValue(Compiler* compiler, VariableRef var);

static LLVMValueRef compileCallArgForParam(Compiler* compiler, Expr* argExpr, LLVMTypeRef paramType) {
    if (!compiler || !argExpr || !paramType) return NULL;
    Expr* a = unwrapGroupingExpr(argExpr);

    LLVMValueRef v = compileExpr(compiler, a);
    if (!v) return NULL;

    // Trait object ("no dyn" interface value) conversion:
    // If the callee expects a per-trait object type, convert a concrete struct value to a temporary trait object.
    // - Default (no `move`): build a non-owning view (data points to an lvalue address or a temp alloca).
    // - With `move`: box the value onto the heap and return an owning trait object.
    TraitInfo* traitObj = findTraitByObjType(compiler, paramType);
    if (traitObj) {
        LLVMTypeRef objTy = compilerGetTraitObjType(compiler, traitObj);
        LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);

        // Already a trait object value of the expected trait.
        if (LLVMTypeOf(v) == objTy) {
            return v;
        }

        // Determine if the argument is `move <expr>`.
        int isMove = 0;
        Expr* base = a;
        if (base && base->type == EXPR_UNARY) {
            UnaryExpr* un = (UnaryExpr*)base;
            if (un->operator.type == TOKEN_MOVE) {
                isMove = 1;
                base = unwrapGroupingExpr(un->right);
            }
        }

        // Resolve the concrete struct name for vtable lookup.
        const char* structName = NULL;
        int structLen = 0;
        LLVMTypeRef concreteTy = LLVMTypeOf(v);

        // Prefer lvalue metadata for variables.
        LLVMValueRef dataAddr = NULL;
        if (base && base->type == EXPR_VARIABLE) {
            VariableRef var = findVariableExpr(compiler, base);
            if (var.typeName && var.typeNameLength > 0) {
                structName = var.typeName;
                structLen = var.typeNameLength;
            }
            if (!isMove) {
                // For view conversion, point to the lvalue address.
                if (var.value && var.type && LLVMGetTypeKind(var.type) != LLVMPointerTypeKind) {
                    dataAddr = implicitBorrowAddrOfVar(compiler, var);
                } else if (var.value && var.type && LLVMGetTypeKind(var.type) == LLVMPointerTypeKind) {
                    // For plain refs, the slot already holds an address.
                    dataAddr = loadLocalValue(compiler, var);
                }
            }
        }

        if (!structName && LLVMGetTypeKind(concreteTy) == LLVMStructTypeKind) {
            const char* n = LLVMGetStructName(concreteTy);
            if (n) {
                structName = n;
                structLen = (int)strlen(n);
            }
        }

        if (!structName || structLen <= 0) {
            compilerErrorAtToken(compiler, &a->token, "cannot form trait object: unknown concrete type");
            return NULL;
        }

        char* vtName = vtableGlobalNameForTraitAndStruct(traitObj->name, traitObj->nameLength, structName, structLen);
        LLVMValueRef vt = LLVMGetNamedGlobal(compiler->module, vtName);
        free(vtName);
        if (!vt) {
            compilerErrorAtToken(
                compiler,
                &a->token,
                "missing trait impl: '%.*s' does not implement '%.*s'",
                structLen,
                structName,
                traitObj->nameLength,
                traitObj->name
            );
            return NULL;
        }

        LLVMValueRef dataI8 = NULL;
        if (isMove) {
            // Own: heap box the concrete value.
            if (LLVMGetTypeKind(concreteTy) != LLVMStructTypeKind) {
                compilerErrorAtToken(compiler, &a->token, "trait object move conversion requires a struct value");
                return NULL;
            }
            LLVMValueRef mallocFn = getOrCreateMalloc(compiler);
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
            LLVMBuildStore(compiler->builder, v, cell);
            dataI8 = LLVMBuildBitCast(compiler->builder, cell, i8ptr, "data");
        } else {
            // View: data points to an address.
            if (!dataAddr) {
                // Materialize a temporary for rvalues.
                if (LLVMGetTypeKind(concreteTy) != LLVMStructTypeKind) {
                    compilerErrorAtToken(compiler, &a->token, "trait object conversion requires a struct value");
                    return NULL;
                }
                LLVMValueRef tmp = LLVMBuildAlloca(compiler->builder, concreteTy, "to_tmp");
                LLVMBuildStore(compiler->builder, v, tmp);
                dataAddr = tmp;
            }
            dataI8 = LLVMBuildBitCast(compiler->builder, dataAddr, i8ptr, "data");
        }

        LLVMValueRef vtI8 = LLVMBuildBitCast(compiler->builder, vt, i8ptr, "vt");
        LLVMValueRef out = LLVMGetUndef(objTy);
        out = LLVMBuildInsertValue(compiler->builder, out, dataI8, 0, "o0");
        out = LLVMBuildInsertValue(compiler->builder, out, vtI8, 1, "o1");
        return out;
    }

    // Default: values are passed by value (with best-effort casts).
    if (LLVMGetTypeKind(paramType) != LLVMPointerTypeKind) {
        return castValueToType(compiler, v, paramType);
    }

    // If the argument already produces a pointer value (string/map/array/&T/etc), pass it directly.
    if (LLVMGetTypeKind(LLVMTypeOf(v)) == LLVMPointerTypeKind) {
        return castValueToType(compiler, v, paramType);
    }

    // Otherwise, the callee expects a pointer but the argument is a value (e.g. borrowing a struct by value).
    // Prefer borrowing an existing lvalue; fall back to materializing a temporary.
    if (a && a->type == EXPR_VARIABLE) {
        VariableRef var = findVariableExpr(compiler, a);
        if (var.value && var.type && LLVMGetTypeKind(var.type) != LLVMPointerTypeKind) {
            LLVMValueRef addr = implicitBorrowAddrOfVar(compiler, var);
            if (!addr) return NULL;
            if (LLVMTypeOf(addr) != paramType) {
                addr = LLVMBuildBitCast(compiler->builder, addr, paramType, "argptrcast");
            }
            return addr;
        }
    }

    LLVMValueRef tmp = LLVMBuildAlloca(compiler->builder, LLVMTypeOf(v), "argtmp");
    LLVMBuildStore(compiler->builder, v, tmp);
    LLVMValueRef p = tmp;
    if (LLVMTypeOf(p) != paramType) {
        p = LLVMBuildBitCast(compiler->builder, p, paramType, "argtmpcast");
    }
    return p;
}

// For a call-chain like `f(...)(...)(...)`, return the innermost call (`f(...)`) and set depth.
// Depth is the number of CallExpr nodes in the chain.
static CallExpr* findInnermostCallInChain(Expr* e, int* outDepth) {
    if (outDepth) *outDepth = 0;
    Expr* cur = unwrapGroupingExpr(e);
    if (!cur || cur->type != EXPR_CALL) return NULL;

    int depth = 0;
    CallExpr* base = NULL;
    while (cur && cur->type == EXPR_CALL) {
        CallExpr* c = (CallExpr*)cur;
        depth++;
        base = c;
        Expr* next = unwrapGroupingExpr(c->callee);
        if (!next || next->type != EXPR_CALL) break;
        cur = next;
    }

    if (outDepth) *outDepth = depth;
    return base;
}

static char* mangleRawAndToken(const char* left, int leftLen, const Token* right, int* outLen) {
    const int sepLen = 2;
    int len = leftLen + sepLen + right->length;
    char* s = malloc((size_t)len + 1);
    memcpy(s, left, (size_t)leftLen);
    memcpy(s + leftLen, "__", (size_t)sepLen);
    memcpy(s + leftLen + sepLen, right->start, (size_t)right->length);
    s[len] = '\0';
    if (outLen) *outLen = len;
    return s;
}

// Forward decl (used by early helpers).
static LLVMValueRef collapseMultiReturnByTypeIfNeeded(Compiler* compiler, LLVMValueRef call, LLVMTypeRef retType);

static int isBuiltinHandleTypeName(const char* name, int nameLen) {
    if (!name || nameLen <= 0) return 0;
    if (nameLen == 5 && memcmp(name, "bytes", 5) == 0) return 1;
    if (nameLen == 3 && memcmp(name, "map", 3) == 0) return 1;
    return 0;
}

// Try to resolve and call a std-defined `impl <builtin-handle> { ... }` method:
// `<TypeName>__<method>(this, ...)`. Returns NULL if not found.
static LLVMValueRef tryEmitBuiltinHandleImplMethodCall(
    Compiler* compiler,
    VariableRef recvVar,
    GetExpr* get,
    CallExpr* expr,
    int wantMultiForThisCall
) {
    if (!compiler || !get || !expr) return NULL;
    if (!recvVar.value) return NULL;
    if (!recvVar.typeName || recvVar.typeNameLength <= 0) return NULL;
    if (!isBuiltinHandleTypeName(recvVar.typeName, recvVar.typeNameLength)) return NULL;
    if (expr->typeArgs && expr->typeArgs->length > 0) {
        compilerErrorAtToken(compiler, &get->name, "generic type arguments on instance methods are not supported yet");
        return NULL;
    }

    int mangledLen = 0;
    char* mangled = mangleRawAndToken(recvVar.typeName, recvVar.typeNameLength, &get->name, &mangledLen);
    LLVMValueRef func = LLVMGetNamedFunction(compiler->module, mangled);
    free(mangled);
    if (!func) return NULL;

    LLVMTypeRef funcType = LLVMGlobalGetValueType(func);
    unsigned expected = LLVMCountParamTypes(funcType);
    unsigned got = expr->arguments ? (unsigned)expr->arguments->length : 0;
    if (expected != got + 1) {
        compilerErrorAtToken(
            compiler,
            &get->name,
            "argument count mismatch for call '%.*s': expected %u, got %u",
            get->name.length,
            get->name.start,
            expected > 0 ? (unsigned)(expected - 1) : 0,
            got
        );
        return NULL;
    }

    LLVMTypeRef* paramTypes = NULL;
    if (expected > 0) {
        paramTypes = malloc(sizeof(LLVMTypeRef) * (size_t)expected);
        LLVMGetParamTypes(funcType, paramTypes);
    }

    LLVMValueRef thisArg = NULL;
    if (recvVar.isBoxed) {
        if (!recvVar.boxPtrType) {
            if (paramTypes) free(paramTypes);
            compilerErrorAtToken(compiler, &get->name, "missing boxed pointer type for receiver");
            return NULL;
        }
        LLVMValueRef cellPtr = LLVMBuildLoad2(compiler->builder, recvVar.boxPtrType, recvVar.value, "cellptr");
        thisArg = LLVMBuildLoad2(compiler->builder, recvVar.type, cellPtr, "this");
    } else {
        thisArg = LLVMBuildLoad2(compiler->builder, recvVar.type, recvVar.value, "this");
    }

    LLVMValueRef* args = NULL;
    if (expected > 0) {
        args = malloc(sizeof(LLVMValueRef) * (size_t)expected);
        args[0] = castValueToType(compiler, thisArg, paramTypes ? paramTypes[0] : LLVMTypeOf(thisArg));
        ListNode* node = expr->arguments ? expr->arguments->head : NULL;
        for (unsigned i = 1; i < expected; i++) {
            LLVMTypeRef pt = paramTypes ? paramTypes[i] : NULL;
            LLVMValueRef av = compileCallArgForParam(compiler, (Expr*)node->data, pt);
            if (!av) {
                if (paramTypes) free(paramTypes);
                if (args) free(args);
                return NULL;
            }
            args[i] = av;
            node = node->next;
        }
    }

    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
    LLVMValueRef call = LLVMBuildCall2(compiler->builder, funcType, func, args, expected, callInstNameForFnType(funcType));
    LLVMTypeRef retType = LLVMGetReturnType(funcType);
    LLVMValueRef out = collapseMultiReturnByTypeIfNeeded(compiler, call, retType);
    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;

    if (paramTypes) free(paramTypes);
    if (args) free(args);
    return out;
}

// Slice type cache entry (must match `SliceTypeEntry` in `src/compiler.c`).
typedef struct {
    LLVMTypeRef elem;
    LLVMTypeRef slice;
} SliceTypeEntry;

static LLVMTypeRef sliceElemTypeFromSliceStruct(Compiler* compiler, LLVMTypeRef sliceStruct) {
    if (!compiler || !sliceStruct || !compiler->sliceTypes) return NULL;
    for (int i = 0; i < compiler->sliceTypes->length; i++) {
        SliceTypeEntry* e = (SliceTypeEntry*)listGet(compiler->sliceTypes, i);
        if (e && e->slice == sliceStruct) return e->elem;
    }
    return NULL;
}

// Try to resolve and call a std-defined `impl <builtin-value> { ... }` method:
// `<TypeName>__<method>(this, ...)`.
// Returns NULL if not found or if a generic template exists but cannot be instantiated.
static LLVMValueRef tryEmitBuiltinValueImplMethodCall(
    Compiler* compiler,
    const char* typeName,
    int typeNameLen,
    Expr* recvExpr,
    LLVMValueRef recvVal,
    GetExpr* get,
    CallExpr* expr,
    int wantMultiForThisCall,
    int* outSawTemplate
) {
    if (outSawTemplate) *outSawTemplate = 0;
    if (!compiler || !typeName || typeNameLen <= 0 || !recvVal || !get || !expr) return NULL;

    int mangledLen = 0;
    char* mangled = mangleRawAndToken(typeName, typeNameLen, &get->name, &mangledLen);
    if (!mangled) return NULL;

    // Direct (non-generic) function.
    LLVMValueRef func = LLVMGetNamedFunction(compiler->module, mangled);
    if (func) {
        LLVMTypeRef funcType = LLVMGlobalGetValueType(func);
        unsigned expected = LLVMCountParamTypes(funcType);
        unsigned got = expr->arguments ? (unsigned)expr->arguments->length : 0;
        if (expected != got + 1) {
            compilerErrorAtToken(
                compiler,
                &get->name,
                "argument count mismatch for call '%.*s': expected %u, got %u",
                get->name.length,
                get->name.start,
                expected > 0 ? (unsigned)(expected - 1) : 0,
                got
            );
            free(mangled);
            return NULL;
        }

        LLVMTypeRef* paramTypes = NULL;
        if (expected > 0) {
            paramTypes = malloc(sizeof(LLVMTypeRef) * (size_t)expected);
            LLVMGetParamTypes(funcType, paramTypes);
        }

        LLVMValueRef* args = NULL;
        if (expected > 0) {
            args = malloc(sizeof(LLVMValueRef) * (size_t)expected);
            args[0] = castValueToType(compiler, recvVal, paramTypes ? paramTypes[0] : LLVMTypeOf(recvVal));
            ListNode* node = expr->arguments ? expr->arguments->head : NULL;
            for (unsigned i = 1; i < expected; i++) {
                LLVMTypeRef pt = paramTypes ? paramTypes[i] : NULL;
                LLVMValueRef av = compileCallArgForParam(compiler, (Expr*)node->data, pt);
                if (!av) {
                    if (paramTypes) free(paramTypes);
                    if (args) free(args);
                    free(mangled);
                    return NULL;
                }
                args[i] = av;
                node = node->next;
            }
        }

        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
        LLVMValueRef call = LLVMBuildCall2(compiler->builder, funcType, func, args, expected, callInstNameForFnType(funcType));
        LLVMTypeRef retType = LLVMGetReturnType(funcType);
        LLVMValueRef out = collapseMultiReturnByTypeIfNeeded(compiler, call, retType);
        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;

        if (paramTypes) free(paramTypes);
        if (args) free(args);
        free(mangled);
        return out;
    }

    // Generic template.
    GenericFuncTemplate* tmpl = compilerFindGenericFuncTemplate(compiler, mangled, mangledLen);
    if (!tmpl || !tmpl->qualifiedName || tmpl->qualifiedNameLen <= 0) {
        free(mangled);
        return NULL;
    }
    if (outSawTemplate) *outSawTemplate = 1;

    LLVMValueRef inst = NULL;
    if (expr->typeArgs && expr->typeArgs->length > 0) {
        inst = compilerInstantiateGenericFunc(compiler, tmpl->qualifiedName, tmpl->qualifiedNameLen, expr->typeArgs, &get->name);
        if (!inst) {
            free(mangled);
            return NULL;
        }
    } else {
        // Build a synthetic call for inference: the receiver is the first argument.
        CallExpr fake;
        memset(&fake, 0, sizeof(fake));
        fake.base.type = EXPR_CALL;
        fake.base.token = expr->base.token;
        fake.arguments = listNew();
        if (recvExpr) listAppend(fake.arguments, recvExpr);
        for (ListNode* n = expr->arguments ? expr->arguments->head : NULL; n != NULL; n = n->next) {
            listAppend(fake.arguments, n->data);
        }

        GenericInferDiag inferDiag;
        initGenericInferDiag(&inferDiag);
        List* inferred = inferTypeArgsForGenericCall(compiler, tmpl, &fake, &inferDiag);
        listFree(fake.arguments);

        if (inferred) {
            inst = compilerInstantiateGenericFunc(compiler, tmpl->qualifiedName, tmpl->qualifiedNameLen, inferred, &get->name);
            // inferred list elements are heap-allocated Type*; keep for compiler cache lifetime.
            listFree(inferred);
        }
    }
    if (!inst) {
        free(mangled);
        return NULL;
    }

    LLVMTypeRef funcType = LLVMGlobalGetValueType(inst);
    unsigned expected = LLVMCountParamTypes(funcType);
    unsigned got = expr->arguments ? (unsigned)expr->arguments->length : 0;
    if (expected != got + 1) {
        compilerErrorAtToken(
            compiler,
            &get->name,
            "argument count mismatch for call '%.*s': expected %u, got %u",
            get->name.length,
            get->name.start,
            expected > 0 ? (unsigned)(expected - 1) : 0,
            got
        );
        free(mangled);
        return NULL;
    }

    LLVMTypeRef* paramTypes = NULL;
    if (expected > 0) {
        paramTypes = malloc(sizeof(LLVMTypeRef) * (size_t)expected);
        LLVMGetParamTypes(funcType, paramTypes);
    }

    LLVMValueRef* args = NULL;
    if (expected > 0) {
        args = malloc(sizeof(LLVMValueRef) * (size_t)expected);
        args[0] = castValueToType(compiler, recvVal, paramTypes ? paramTypes[0] : LLVMTypeOf(recvVal));
        ListNode* node = expr->arguments ? expr->arguments->head : NULL;
        for (unsigned i = 1; i < expected; i++) {
            LLVMTypeRef pt = paramTypes ? paramTypes[i] : NULL;
            LLVMValueRef av = compileCallArgForParam(compiler, (Expr*)node->data, pt);
            if (!av) {
                if (paramTypes) free(paramTypes);
                if (args) free(args);
                free(mangled);
                return NULL;
            }
            args[i] = av;
            node = node->next;
        }
    }

    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
    LLVMValueRef call = LLVMBuildCall2(compiler->builder, funcType, inst, args, expected, callInstNameForFnType(funcType));
    LLVMTypeRef retType = LLVMGetReturnType(funcType);
    LLVMValueRef out = collapseMultiReturnByTypeIfNeeded(compiler, call, retType);
    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;

    if (paramTypes) free(paramTypes);
    if (args) free(args);
    free(mangled);
    return out;
}

typedef struct {
    int depth;           // number of embedded steps from root to target receiver type
    int indices[16];     // embedded field indices at each step
    StructInfo* target;  // struct that defines the method
    LLVMValueRef func;   // resolved function pointer (Struct__method)
    int isAmbiguous;
} PromotedMethodPath;

static int traitDeclHasMethod(TraitInfo* trait, const Token* methodName) {
    if (!trait || !trait->decl || !methodName) return 0;
    for (ListNode* mn = trait->decl->methods ? trait->decl->methods->head : NULL; mn != NULL; mn = mn->next) {
        TraitMethodDecl* req = (TraitMethodDecl*)mn->data;
        if (!req) continue;
        if (req->name.length != methodName->length) continue;
        if (memcmp(req->name.start, methodName->start, (size_t)methodName->length) == 0) return 1;
    }
    return 0;
}

static int fieldLooksEmbedded(const FieldDeclaration* f) {
    if (!f) return 0;
    if (f->isEmbedded) return 1;
    if (f->isConst) return 0;
    if (f->initializer) return 0;
    if (!f->type || f->type->kind != TYPE_NAMED) return 0;
    if (f->name.length != f->type->name.length) return 0;
    if (f->name.length <= 0) return 0;
    const char* tn = f->type->name.start;
    const char* fn = f->name.start;
    if (!tn || !fn) return 0;
    char c0 = tn[0];
    if (c0 >= 'A' && c0 <= 'Z') c0 = (char)(c0 - 'A' + 'a');
    if (fn[0] != c0) return 0;
    if (f->name.length > 1 && memcmp(fn + 1, tn + 1, (size_t)f->name.length - 1) != 0) return 0;
    return 1;
}

static void promotedMethodSearch(
    Compiler* compiler,
    StructInfo* info,
    const Token* methodName,
    int depth,
    int indices[16],
    PromotedMethodPath* ioBest
) {
    if (!compiler || !info || !info->decl || !info->decl->fields || !methodName || !ioBest) return;
    if (depth < 0 || depth >= (int)(sizeof(ioBest->indices) / sizeof(ioBest->indices[0]))) return;

    int mangledLen = 0;
    char* mangled = mangleRawAndToken(info->name, info->nameLength, methodName, &mangledLen);
    LLVMValueRef fn = LLVMGetNamedFunction(compiler->module, mangled);
    free(mangled);

    if (fn) {
        if (ioBest->target) {
            ioBest->isAmbiguous = 1;
            return;
        }
        ioBest->depth = depth;
        for (int i = 0; i < depth; i++) ioBest->indices[i] = indices[i];
        ioBest->target = info;
        ioBest->func = fn;
        return;
    }

    for (int i = 0; i < info->decl->fields->length; i++) {
        FieldDeclaration* f = listGet(info->decl->fields, i);
        if (!f || !fieldLooksEmbedded(f) || !f->type) continue;
        if (f->type->kind != TYPE_NAMED) continue;
        StructInfo* inner = compilerResolveStructByToken(compiler, &f->type->name);
        if (!inner) continue;
        indices[depth] = i;
        promotedMethodSearch(compiler, inner, methodName, depth + 1, indices, ioBest);
        if (ioBest->isAmbiguous) return;
    }
}

// Resolve `root.method(...)` through embedded-field promotion.
// Returns 1 on success, 0 if not found, -1 if ambiguous.
static int resolvePromotedMethodPath(Compiler* compiler, StructInfo* root, const Token* methodName, PromotedMethodPath* out) {
    if (out) memset(out, 0, sizeof(*out));
    if (!compiler || !root || !methodName || !out) return 0;
    // Direct method wins (no promotion needed).
    int mangledLen = 0;
    char* mangled = mangleRawAndToken(root->name, root->nameLength, methodName, &mangledLen);
    LLVMValueRef direct = LLVMGetNamedFunction(compiler->module, mangled);
    free(mangled);
    if (direct) {
        out->depth = 0;
        out->target = root;
        out->func = direct;
        return 1;
    }
    int tmp[16] = {0};
    promotedMethodSearch(compiler, root, methodName, 0, tmp, out);
    if (out->isAmbiguous) return -1;
    if (!out->target || !out->func) return 0;
    return 1;
}

static LLVMValueRef emitDirectFuncCall(Compiler* compiler, LLVMValueRef func, CallExpr* expr, int errLine) {
    if (!compiler || !func || !expr) return NULL;
    LLVMTypeRef funcType = LLVMGlobalGetValueType(func);
    unsigned expected = LLVMCountParamTypes(funcType);
    unsigned got = expr->arguments ? (unsigned)expr->arguments->length : 0;
    if (expected != got) {
        compilerErrorAt(compiler, errLine, "argument count mismatch");
        return NULL;
    }

    LLVMTypeRef* paramTypes = NULL;
    if (expected > 0) {
        paramTypes = malloc(sizeof(LLVMTypeRef) * (size_t)expected);
        LLVMGetParamTypes(funcType, paramTypes);
    }

    LLVMValueRef* args = NULL;
    if (expected > 0) {
        args = malloc(sizeof(LLVMValueRef) * (size_t)expected);
        ListNode* node = expr->arguments->head;
        for (unsigned i = 0; i < expected; i++) {
            LLVMValueRef av = compileCallArgForParam(compiler, (Expr*)node->data, paramTypes[i]);
            if (!av) {
                if (paramTypes) free(paramTypes);
                if (args) free(args);
                return NULL;
            }
            args[i] = av;
            node = node->next;
        }
    }

    LLVMValueRef call = LLVMBuildCall2(compiler->builder, funcType, func, args, expected, callInstNameForFnType(funcType));
    if (expr->arguments && expected > 0) {
        dropTemporaryClosureArgs(compiler, expr->arguments, args, 0);
    }
    if (paramTypes) free(paramTypes);
    if (args) free(args);
    return collapseMultiReturnIfNeeded(compiler, func, call);
}

static LLVMValueRef emitStructConstructor(Compiler* compiler, StructInfo* info, CallExpr* expr) {
    if (!compiler || !info) return NULL;
    int argCount = expr->arguments ? expr->arguments->length : 0;
    // Value semantics: build a stack temporary and return the loaded value.
    LLVMValueRef tmp = LLVMBuildAlloca(compiler->builder, info->type, "ctor_tmp");
    LLVMValueRef obj = tmp;

    // Initialize fields
    int fieldCount = info->decl && info->decl->fields ? info->decl->fields->length : 0;
    if (argCount > fieldCount) {
        emitDebug("Too many constructor args\n");
        return NULL;
    }

    ListNode* argNode = expr->arguments ? expr->arguments->head : NULL;
    for (int i = 0; i < fieldCount; i++) {
        FieldDeclaration* field = listGet(info->decl->fields, i);
        LLVMValueRef fieldPtr = LLVMBuildStructGEP2(compiler->builder, info->type, obj, (unsigned)i, "field_ptr");
        LLVMTypeRef fType = typeToLLVMType(compiler, field ? field->type : NULL);

        LLVMValueRef initVal = NULL;
        if (i < argCount) {
            initVal = compileExpr(compiler, (Expr*)argNode->data);
            argNode = argNode->next;
        } else if (field && field->initializer) {
            initVal = compileExpr(compiler, field->initializer);
        }
        if (!initVal) {
            initVal = LLVMConstNull(fType);
        }
        initVal = castValueToType(compiler, initVal, fType);
        LLVMBuildStore(compiler->builder, initVal, fieldPtr);
    }

    return LLVMBuildLoad2(compiler->builder, info->type, tmp, "ctor");
}

static int tokenEqualsToken(const Token* a, const Token* b) {
    if (!a || !b) return 0;
    if (a->length != b->length) return 0;
    return memcmp(a->start, b->start, (size_t)a->length) == 0;
}

static StructInfo* resolveStructForInitExpr(Compiler* compiler, Expr* callee, Token* errTok) {
    if (!compiler || !callee) return NULL;

    if (callee->type == EXPR_VARIABLE) {
        VariableExpr* v = (VariableExpr*)callee;
        if (errTok) *errTok = v->name;
        return compilerResolveStructByToken(compiler, &v->name);
    }

    if (callee->type == EXPR_GET) {
        GetExpr* get = (GetExpr*)callee;
        if (errTok) *errTok = get->name;

        if (get->object && get->object->type == EXPR_VARIABLE) {
            VariableExpr* ns = (VariableExpr*)get->object;
            SymbolAlias* a = compilerFindAlias(compiler, ns->name.start, ns->name.length);
            if (a && a->kind == ALIAS_MODULE) {
                int ql = 0;
                char* q = mangleRawAndToken(a->qualified, a->qualifiedLen, &get->name, &ql);
                StructInfo* info = compilerFindStruct(compiler, q, ql);
                free(q);
                return info;
            }
        }

        // Support `ns.A.B{...}` when `ns` is a module alias.
        if (get->object && get->object->type == EXPR_GET) {
            GetExpr* inner = (GetExpr*)get->object;
            if (inner->object && inner->object->type == EXPR_VARIABLE) {
                VariableExpr* ns = (VariableExpr*)inner->object;
                SymbolAlias* a = compilerFindAlias(compiler, ns->name.start, ns->name.length);
                if (a && a->kind == ALIAS_MODULE) {
                    int ql1 = 0;
                    char* q1 = mangleRawAndToken(a->qualified, a->qualifiedLen, &inner->name, &ql1);
                    int ql2 = 0;
                    char* q2 = mangleRawAndToken(q1, ql1, &get->name, &ql2);
                    free(q1);
                    StructInfo* info = compilerFindStruct(compiler, q2, ql2);
                    free(q2);
                    return info;
                }
            }
        }
    }

    return NULL;
}

static void moveOutOnStructFieldInitIfNeeded(Compiler* compiler, Expr* srcExpr) {
    if (!compiler || !srcExpr) return;
    if (srcExpr->type != EXPR_VARIABLE) return;
    VariableRef v = findVariableExpr(compiler, srcExpr);
    if (!v.value || !v.type) return;
    if (v.isArray && v.isStackArray) return;
    LLVMTypeRef cloTy = compilerGetClosureType(compiler);
    if (!v.isMap && !v.isArray && !v.isBytes && !v.isTraitObj && v.type != cloTy) return;

    LLVMValueRef nullv = LLVMConstNull(v.type);
    if (v.isBoxed) {
        if (!v.boxPtrType) return;
        LLVMValueRef cell = LLVMBuildLoad2(compiler->builder, v.boxPtrType, v.value, "mv_cell");
        LLVMBuildStore(compiler->builder, nullv, cell);
    } else {
        LLVMBuildStore(compiler->builder, nullv, v.value);
    }
}

LLVMValueRef emitStructInitExpr(Compiler* compiler, StructInitExpr* expr) {
    if (!compiler || !expr || !expr->callee) return NULL;

    Token errTok = expr->base.token;
    StructInfo* info = resolveStructForInitExpr(compiler, expr->callee, &errTok);
    if (!info || !info->decl) {
        compilerErrorAtToken(compiler, &errTok, "unknown struct in initializer");
        return NULL;
    }

    int fieldCount = info->decl->fields ? info->decl->fields->length : 0;
    Expr** provided = NULL;
    if (fieldCount > 0) {
        provided = calloc((size_t)fieldCount, sizeof(Expr*));
    }

    for (ListNode* n = expr->fields ? expr->fields->head : NULL; n != NULL; n = n->next) {
        StructFieldInit* f = (StructFieldInit*)n->data;
        if (!f) continue;

        int idx = -1;
        for (int i = 0; i < fieldCount; i++) {
            FieldDeclaration* fd = listGet(info->decl->fields, i);
            if (fd && tokenEqualsToken(&fd->name, &f->name)) {
                idx = i;
                break;
            }
        }
        if (idx < 0) {
            compilerErrorAtToken(compiler, &f->name, "unknown field '%.*s' in struct initializer", f->name.length, f->name.start);
            if (provided) free(provided);
            return NULL;
        }
        if (provided && provided[idx]) {
            compilerErrorAtToken(compiler, &f->name, "duplicate field '%.*s' in struct initializer", f->name.length, f->name.start);
            if (provided) free(provided);
            return NULL;
        }
        if (provided) provided[idx] = f->value;
    }

    // Value semantics: build a stack temporary and return the loaded value.
    LLVMValueRef tmp = LLVMBuildAlloca(compiler->builder, info->type, "sinit_tmp");
    LLVMValueRef obj = tmp;

    for (int i = 0; i < fieldCount; i++) {
        FieldDeclaration* field = listGet(info->decl->fields, i);
        LLVMValueRef fieldPtr = LLVMBuildStructGEP2(compiler->builder, info->type, obj, (unsigned)i, "field_ptr");
        LLVMTypeRef fType = typeToLLVMType(compiler, field ? field->type : NULL);

        LLVMValueRef initVal = NULL;
        if (provided && provided[i]) {
            initVal = compileExpr(compiler, provided[i]);
        } else if (field && field->initializer) {
            initVal = compileExpr(compiler, field->initializer);
        }
        if (!initVal) {
            initVal = LLVMConstNull(fType);
        }
        initVal = castValueToType(compiler, initVal, fType);
        LLVMBuildStore(compiler->builder, initVal, fieldPtr);

        // Move-only fields: when initializing from a variable, move ownership into the struct
        // by nulling out the source slot to prevent later drops (map/array/bytes/trait objects/closures).
        if (provided && provided[i]) {
            moveOutOnStructFieldInitIfNeeded(compiler, provided[i]);
        }
    }

    if (provided) free(provided);
    return LLVMBuildLoad2(compiler->builder, info->type, tmp, "sinit");
}

static LLVMValueRef getOrCreatePrintf(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "printf");
    if (existing) return existing;

    LLVMTypeRef contextI8 = LLVMInt8TypeInContext(compiler->context);
    LLVMTypeRef printfParamTypes[] = { LLVMPointerType(contextI8, 0) };
    LLVMTypeRef printfType = LLVMFunctionType(
        LLVMInt32TypeInContext(compiler->context),
        printfParamTypes,
        1,
        1
    );

    return LLVMAddFunction(compiler->module, "printf", printfType);
}

static LLVMValueRef getOrCreateTuaPrintValue(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_print_value");
    if (existing) return existing;
    LLVMTypeRef vt = compilerGetTuaValueType(compiler);
    LLVMTypeRef i32 = LLVMInt32TypeInContext(compiler->context);
    LLVMTypeRef params[2] = { vt, i32 };
    LLVMTypeRef fnType = LLVMFunctionType(LLVMVoidTypeInContext(compiler->context), params, 2, 0);
    return LLVMAddFunction(compiler->module, "tua_print_value", fnType);
}

static LLVMValueRef getOrCreateTuaAssertFail(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_assert_fail");
    if (existing) return existing;
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef i32 = LLVMInt32TypeInContext(compiler->context);
    LLVMTypeRef params[2] = { i8ptr, i32 };
    LLVMTypeRef fnType = LLVMFunctionType(LLVMVoidTypeInContext(compiler->context), params, 2, 0);
    return LLVMAddFunction(compiler->module, "tua_assert_fail", fnType);
}

static LLVMValueRef getOrCreateTuaPanic(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_panic");
    if (existing) return existing;
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef params[1] = { i8ptr };
    LLVMTypeRef fnType = LLVMFunctionType(LLVMVoidTypeInContext(compiler->context), params, 1, 0);
    return LLVMAddFunction(compiler->module, "tua_panic", fnType);
}

static LLVMValueRef getOrCreateTuaMapHas(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_map_has");
    if (existing) return existing;
    LLVMTypeRef mapType = compilerGetMapType(compiler);
    LLVMTypeRef vt = compilerGetTuaValueType(compiler);
    LLVMTypeRef params[2] = { mapType, vt };
    LLVMTypeRef fnType = LLVMFunctionType(LLVMInt32TypeInContext(compiler->context), params, 2, 0);
    return LLVMAddFunction(compiler->module, "tua_map_has", fnType);
}

static LLVMValueRef getOrCreateTuaMapLen(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_map_len");
    if (existing) return existing;
    LLVMTypeRef mapType = compilerGetMapType(compiler);
    LLVMTypeRef params[1] = { mapType };
    LLVMTypeRef fnType = LLVMFunctionType(LLVMInt32TypeInContext(compiler->context), params, 1, 0);
    return LLVMAddFunction(compiler->module, "tua_map_len", fnType);
}

static LLVMValueRef getOrCreateTuaMapGetWithOk(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_map_get_with_ok");
    if (existing) return existing;
    LLVMTypeRef mapType = compilerGetMapType(compiler);
    LLVMTypeRef vt = compilerGetTuaValueType(compiler);
    LLVMTypeRef i32 = LLVMInt32TypeInContext(compiler->context);
    LLVMTypeRef params[3] = { mapType, vt, LLVMPointerType(i32, 0) };
    LLVMTypeRef fnType = LLVMFunctionType(vt, params, 3, 0);
    return LLVMAddFunction(compiler->module, "tua_map_get_with_ok", fnType);
}

static LLVMValueRef getOrCreateTuaMapGetRefWithOk(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_map_get_ref_with_ok");
    if (existing) return existing;
    LLVMTypeRef mapType = compilerGetMapType(compiler);
    LLVMTypeRef vt = compilerGetTuaValueType(compiler);
    LLVMTypeRef vtPtr = LLVMPointerType(vt, 0);
    LLVMTypeRef i32 = LLVMInt32TypeInContext(compiler->context);
    LLVMTypeRef params[3] = { mapType, vt, LLVMPointerType(i32, 0) };
    LLVMTypeRef fnType = LLVMFunctionType(vtPtr, params, 3, 0);
    return LLVMAddFunction(compiler->module, "tua_map_get_ref_with_ok", fnType);
}

static LLVMValueRef getOrCreateTuaMapDelete(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_map_delete");
    if (existing) return existing;
    LLVMTypeRef mapType = compilerGetMapType(compiler);
    LLVMTypeRef vt = compilerGetTuaValueType(compiler);
    LLVMTypeRef params[2] = { mapType, vt };
    LLVMTypeRef fnType = LLVMFunctionType(LLVMInt32TypeInContext(compiler->context), params, 2, 0);
    return LLVMAddFunction(compiler->module, "tua_map_delete", fnType);
}

static LLVMValueRef getOrCreateTuaMapClear(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_map_clear");
    if (existing) return existing;
    LLVMTypeRef mapType = compilerGetMapType(compiler);
    LLVMTypeRef params[1] = { mapType };
    LLVMTypeRef fnType = LLVMFunctionType(LLVMVoidTypeInContext(compiler->context), params, 1, 0);
    return LLVMAddFunction(compiler->module, "tua_map_clear", fnType);
}

static LLVMValueRef getOrCreateTuaArrayClone(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_array_clone");
    if (existing) return existing;
    LLVMTypeRef arrType = compilerGetArrayType(compiler);
    LLVMTypeRef params[1] = { arrType };
    LLVMTypeRef fnType = LLVMFunctionType(arrType, params, 1, 0);
    return LLVMAddFunction(compiler->module, "tua_array_clone", fnType);
}

static LLVMValueRef getOrCreateTuaArrayPush(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_array_push");
    if (existing) return existing;
    LLVMTypeRef arrType = compilerGetArrayType(compiler);
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef params[2] = { arrType, i8ptr };
    LLVMTypeRef fnType = LLVMFunctionType(LLVMInt64TypeInContext(compiler->context), params, 2, 0);
    return LLVMAddFunction(compiler->module, "tua_array_push", fnType);
}

static LLVMValueRef getOrCreateTuaBytesLen(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_bytes_len");
    if (existing) return existing;
    LLVMTypeRef bytesTy = compilerGetBytesType(compiler);
    LLVMTypeRef params[1] = { bytesTy };
    LLVMTypeRef fnType = LLVMFunctionType(LLVMInt64TypeInContext(compiler->context), params, 1, 0);
    return LLVMAddFunction(compiler->module, "tua_bytes_len", fnType);
}

static LLVMValueRef getOrCreateTuaBytesData(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_bytes_data");
    if (existing) return existing;
    LLVMTypeRef bytesTy = compilerGetBytesType(compiler);
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef params[1] = { bytesTy };
    LLVMTypeRef fnType = LLVMFunctionType(i8ptr, params, 1, 0);
    return LLVMAddFunction(compiler->module, "tua_bytes_data", fnType);
}

static LLVMValueRef getOrCreateTuaBytesGetU8(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_bytes_get_u8");
    if (existing) return existing;
    LLVMTypeRef bytesTy = compilerGetBytesType(compiler);
    LLVMTypeRef i64 = LLVMInt64TypeInContext(compiler->context);
    LLVMTypeRef i32 = LLVMInt32TypeInContext(compiler->context);
    LLVMTypeRef params[3] = { bytesTy, i64, LLVMPointerType(i32, 0) };
    LLVMTypeRef fnType = LLVMFunctionType(i32, params, 3, 0);
    return LLVMAddFunction(compiler->module, "tua_bytes_get_u8", fnType);
}

static LLVMValueRef getOrCreateTuaBytesSetU8(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_bytes_set_u8");
    if (existing) return existing;
    LLVMTypeRef bytesTy = compilerGetBytesType(compiler);
    LLVMTypeRef i64 = LLVMInt64TypeInContext(compiler->context);
    LLVMTypeRef i32 = LLVMInt32TypeInContext(compiler->context);
    LLVMTypeRef params[3] = { bytesTy, i64, i32 };
    LLVMTypeRef fnType = LLVMFunctionType(i32, params, 3, 0);
    return LLVMAddFunction(compiler->module, "tua_bytes_set_u8", fnType);
}

static LLVMValueRef getOrCreateTuaBytesIsReadonly(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_bytes_is_readonly");
    if (existing) return existing;
    LLVMTypeRef bytesTy = compilerGetBytesType(compiler);
    LLVMTypeRef i32 = LLVMInt32TypeInContext(compiler->context);
    LLVMTypeRef params[1] = { bytesTy };
    LLVMTypeRef fnType = LLVMFunctionType(i32, params, 1, 0);
    return LLVMAddFunction(compiler->module, "tua_bytes_is_readonly", fnType);
}

static LLVMValueRef getOrCreateTuaBytesCopy(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_bytes_copy");
    if (existing) return existing;
    LLVMTypeRef bytesTy = compilerGetBytesType(compiler);
    LLVMTypeRef i64 = LLVMInt64TypeInContext(compiler->context);
    LLVMTypeRef i32 = LLVMInt32TypeInContext(compiler->context);
    LLVMTypeRef params[5] = { bytesTy, i64, bytesTy, i64, i64 };
    LLVMTypeRef fnType = LLVMFunctionType(i32, params, 5, 0);
    return LLVMAddFunction(compiler->module, "tua_bytes_copy", fnType);
}

static LLVMValueRef getOrCreateTuaParseInt(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_parse_int");
    if (existing) return existing;
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef i32 = LLVMInt32TypeInContext(compiler->context);
    LLVMTypeRef i32ptr = LLVMPointerType(i32, 0);
    LLVMTypeRef params[2] = { i8ptr, i32ptr };
    LLVMTypeRef fnType = LLVMFunctionType(i32, params, 2, 0);
    return LLVMAddFunction(compiler->module, "tua_parse_int", fnType);
}

static LLVMValueRef getOrCreateTuaFree(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_free");
    if (existing) return existing;
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef params[1] = { i8ptr };
    LLVMTypeRef fnType = LLVMFunctionType(LLVMVoidTypeInContext(compiler->context), params, 1, 0);
    return LLVMAddFunction(compiler->module, "tua_free", fnType);
}

static LLVMValueRef getOrCreateTuaDeadlineAfterMs(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_deadline_after_ms");
    if (existing) return existing;
    LLVMTypeRef i64 = LLVMInt64TypeInContext(compiler->context);
    LLVMTypeRef params[1] = { i64 };
    LLVMTypeRef fnType = LLVMFunctionType(i64, params, 1, 0);
    return LLVMAddFunction(compiler->module, "tua_deadline_after_ms", fnType);
}

static LLVMValueRef getOrCreateTuaTimeMonoNs(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_time_mono_ns");
    if (existing) return existing;
    LLVMTypeRef i64 = LLVMInt64TypeInContext(compiler->context);
    LLVMTypeRef fnType = LLVMFunctionType(i64, NULL, 0, 0);
    return LLVMAddFunction(compiler->module, "tua_time_mono_ns", fnType);
}

static LLVMValueRef getOrCreateTuaTimeRealNs(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_time_real_ns");
    if (existing) return existing;
    LLVMTypeRef i64 = LLVMInt64TypeInContext(compiler->context);
    LLVMTypeRef fnType = LLVMFunctionType(i64, NULL, 0, 0);
    return LLVMAddFunction(compiler->module, "tua_time_real_ns", fnType);
}

static LLVMValueRef getOrCreateTuaSleepNs(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_sleep_ns");
    if (existing) return existing;
    LLVMTypeRef i64 = LLVMInt64TypeInContext(compiler->context);
    LLVMTypeRef params[1] = { i64 };
    LLVMTypeRef fnType = LLVMFunctionType(LLVMVoidTypeInContext(compiler->context), params, 1, 0);
    return LLVMAddFunction(compiler->module, "tua_sleep_ns", fnType);
}

static LLVMValueRef getOrCreateTuaTimerAfterMsCl(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_timer_after_ms_cl");
    if (existing) return existing;
    LLVMTypeRef i32 = LLVMInt32TypeInContext(compiler->context);
    LLVMTypeRef i64 = LLVMInt64TypeInContext(compiler->context);
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef closure = compilerGetClosureType(compiler);
    LLVMTypeRef params[3] = { i8ptr, i64, closure };
    LLVMTypeRef fnType = LLVMFunctionType(i32, params, 3, 0);
    return LLVMAddFunction(compiler->module, "tua_timer_after_ms_cl", fnType);
}

static LLVMValueRef getOrCreateTuaTimerEveryMsCl(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_timer_every_ms_cl");
    if (existing) return existing;
    LLVMTypeRef i32 = LLVMInt32TypeInContext(compiler->context);
    LLVMTypeRef i64 = LLVMInt64TypeInContext(compiler->context);
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef i8ptrptr = LLVMPointerType(i8ptr, 0);
    LLVMTypeRef closure = compilerGetClosureType(compiler);
    LLVMTypeRef params[4] = { i8ptr, i64, closure, i8ptrptr };
    LLVMTypeRef fnType = LLVMFunctionType(i32, params, 4, 0);
    return LLVMAddFunction(compiler->module, "tua_timer_every_ms_cl", fnType);
}

static LLVMValueRef getOrCreateTuaTimerEveryCancelCl(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_timer_every_cancel_cl");
    if (existing) return existing;
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef params[1] = { i8ptr };
    LLVMTypeRef fnType = LLVMFunctionType(LLVMVoidTypeInContext(compiler->context), params, 1, 0);
    return LLVMAddFunction(compiler->module, "tua_timer_every_cancel_cl", fnType);
}

static LLVMValueRef getOrCreateTuaLoopCreate(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_loop_create");
    if (existing) return existing;
    LLVMTypeRef i32 = LLVMInt32TypeInContext(compiler->context);
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef i8ptrptr = LLVMPointerType(i8ptr, 0);
    LLVMTypeRef params[1] = { i8ptrptr };
    LLVMTypeRef fnType = LLVMFunctionType(i32, params, 1, 0);
    return LLVMAddFunction(compiler->module, "tua_loop_create", fnType);
}

static LLVMValueRef getOrCreateTuaLoopFree(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_loop_free");
    if (existing) return existing;
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef params[1] = { i8ptr };
    LLVMTypeRef fnType = LLVMFunctionType(LLVMVoidTypeInContext(compiler->context), params, 1, 0);
    return LLVMAddFunction(compiler->module, "tua_loop_free", fnType);
}

static LLVMValueRef getOrCreateTuaLoopRun(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_loop_run");
    if (existing) return existing;
    LLVMTypeRef i32 = LLVMInt32TypeInContext(compiler->context);
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef params[1] = { i8ptr };
    LLVMTypeRef fnType = LLVMFunctionType(i32, params, 1, 0);
    return LLVMAddFunction(compiler->module, "tua_loop_run", fnType);
}

static LLVMValueRef getOrCreateTuaLoopStop(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_loop_stop");
    if (existing) return existing;
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef params[1] = { i8ptr };
    LLVMTypeRef fnType = LLVMFunctionType(LLVMVoidTypeInContext(compiler->context), params, 1, 0);
    return LLVMAddFunction(compiler->module, "tua_loop_stop", fnType);
}

static LLVMValueRef getOrCreateTuaLoopPostCl(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_loop_post_cl");
    if (existing) return existing;
    LLVMTypeRef i32 = LLVMInt32TypeInContext(compiler->context);
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef closure = compilerGetClosureType(compiler);
    LLVMTypeRef params[2] = { i8ptr, closure };
    LLVMTypeRef fnType = LLVMFunctionType(i32, params, 2, 0);
    return LLVMAddFunction(compiler->module, "tua_loop_post_cl", fnType);
}

static LLVMValueRef getOrCreateTuaWorkqueueCreate(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_workqueue_create");
    if (existing) return existing;
    LLVMTypeRef i32 = LLVMInt32TypeInContext(compiler->context);
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef i8ptrptr = LLVMPointerType(i8ptr, 0);
    LLVMTypeRef params[2] = { i8ptrptr, i32 };
    LLVMTypeRef fnType = LLVMFunctionType(i32, params, 2, 0);
    return LLVMAddFunction(compiler->module, "tua_workqueue_create", fnType);
}

static LLVMValueRef getOrCreateTuaWorkqueueFree(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_workqueue_free");
    if (existing) return existing;
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef params[1] = { i8ptr };
    LLVMTypeRef fnType = LLVMFunctionType(LLVMVoidTypeInContext(compiler->context), params, 1, 0);
    return LLVMAddFunction(compiler->module, "tua_workqueue_free", fnType);
}

static LLVMValueRef getOrCreateTuaTcpListen(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_tcp_listen");
    if (existing) return existing;
    LLVMTypeRef i32 = LLVMInt32TypeInContext(compiler->context);
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef i8ptrptr = LLVMPointerType(i8ptr, 0);
    LLVMTypeRef params[4] = { i8ptr, i8ptr, i32, i8ptrptr };
    LLVMTypeRef fnType = LLVMFunctionType(i32, params, 4, 0);
    return LLVMAddFunction(compiler->module, "tua_tcp_listen", fnType);
}

static LLVMValueRef getOrCreateTuaTcpListenerLocalPort(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_tcp_listener_local_port");
    if (existing) return existing;
    LLVMTypeRef i32 = LLVMInt32TypeInContext(compiler->context);
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef params[1] = { i8ptr };
    LLVMTypeRef fnType = LLVMFunctionType(i32, params, 1, 0);
    return LLVMAddFunction(compiler->module, "tua_tcp_listener_local_port", fnType);
}

static LLVMValueRef getOrCreateTuaTcpListenerClose(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_tcp_listener_close");
    if (existing) return existing;
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef params[1] = { i8ptr };
    LLVMTypeRef fnType = LLVMFunctionType(LLVMVoidTypeInContext(compiler->context), params, 1, 0);
    return LLVMAddFunction(compiler->module, "tua_tcp_listener_close", fnType);
}

static LLVMValueRef getOrCreateTuaTcpSocketClose(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_tcp_socket_close");
    if (existing) return existing;
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef params[1] = { i8ptr };
    LLVMTypeRef fnType = LLVMFunctionType(LLVMVoidTypeInContext(compiler->context), params, 1, 0);
    return LLVMAddFunction(compiler->module, "tua_tcp_socket_close", fnType);
}

static LLVMValueRef getOrCreateTuaTcpConnectAsyncCl(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_tcp_connect_async_cl");
    if (existing) return existing;
    LLVMTypeRef i32 = LLVMInt32TypeInContext(compiler->context);
    LLVMTypeRef i64 = LLVMInt64TypeInContext(compiler->context);
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef closure = compilerGetClosureType(compiler);
    LLVMTypeRef params[6] = { i8ptr, i8ptr, i8ptr, i8ptr, i64, closure };
    LLVMTypeRef fnType = LLVMFunctionType(i32, params, 6, 0);
    return LLVMAddFunction(compiler->module, "tua_tcp_connect_async_cl", fnType);
}

static LLVMValueRef getOrCreateTuaTcpConnectPortAsyncCl(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_tcp_connect_port_async_cl");
    if (existing) return existing;
    LLVMTypeRef i32 = LLVMInt32TypeInContext(compiler->context);
    LLVMTypeRef i64 = LLVMInt64TypeInContext(compiler->context);
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef closure = compilerGetClosureType(compiler);
    LLVMTypeRef params[6] = { i8ptr, i8ptr, i8ptr, i32, i64, closure };
    LLVMTypeRef fnType = LLVMFunctionType(i32, params, 6, 0);
    return LLVMAddFunction(compiler->module, "tua_tcp_connect_port_async_cl", fnType);
}

static LLVMValueRef getOrCreateTuaTcpAcceptStartCl(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_tcp_accept_start_cl");
    if (existing) return existing;
    LLVMTypeRef i32 = LLVMInt32TypeInContext(compiler->context);
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef i8ptrptr = LLVMPointerType(i8ptr, 0);
    LLVMTypeRef closure = compilerGetClosureType(compiler);
    LLVMTypeRef params[4] = { i8ptr, i8ptr, closure, i8ptrptr };
    LLVMTypeRef fnType = LLVMFunctionType(i32, params, 4, 0);
    return LLVMAddFunction(compiler->module, "tua_tcp_accept_start_cl", fnType);
}

static LLVMValueRef getOrCreateTuaTcpAcceptCancelCl(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_tcp_accept_cancel_cl");
    if (existing) return existing;
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef params[1] = { i8ptr };
    LLVMTypeRef fnType = LLVMFunctionType(LLVMVoidTypeInContext(compiler->context), params, 1, 0);
    return LLVMAddFunction(compiler->module, "tua_tcp_accept_cancel_cl", fnType);
}

static LLVMValueRef getOrCreateTuaTcpReadAllocAsyncCl(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_tcp_read_alloc_async_cl");
    if (existing) return existing;
    LLVMTypeRef i32 = LLVMInt32TypeInContext(compiler->context);
    LLVMTypeRef i64 = LLVMInt64TypeInContext(compiler->context);
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef closure = compilerGetClosureType(compiler);
    LLVMTypeRef params[5] = { i8ptr, i8ptr, i32, i64, closure };
    LLVMTypeRef fnType = LLVMFunctionType(i32, params, 5, 0);
    return LLVMAddFunction(compiler->module, "tua_tcp_read_alloc_async_cl", fnType);
}

static LLVMValueRef getOrCreateTuaTcpWriteStrAsyncCl(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_tcp_write_str_async_cl");
    if (existing) return existing;
    LLVMTypeRef i32 = LLVMInt32TypeInContext(compiler->context);
    LLVMTypeRef i64 = LLVMInt64TypeInContext(compiler->context);
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef closure = compilerGetClosureType(compiler);
    LLVMTypeRef params[5] = { i8ptr, i8ptr, i8ptr, i64, closure };
    LLVMTypeRef fnType = LLVMFunctionType(i32, params, 5, 0);
    return LLVMAddFunction(compiler->module, "tua_tcp_write_str_async_cl", fnType);
}

static LLVMValueRef getOrCreateTuaFsReadfileAllocAsyncCl(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_fs_readfile_alloc_async_cl");
    if (existing) return existing;
    LLVMTypeRef i32 = LLVMInt32TypeInContext(compiler->context);
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef closure = compilerGetClosureType(compiler);
    LLVMTypeRef params[4] = { i8ptr, i8ptr, i8ptr, closure };
    LLVMTypeRef fnType = LLVMFunctionType(i32, params, 4, 0);
    return LLVMAddFunction(compiler->module, "tua_fs_readfile_alloc_async_cl", fnType);
}

static LLVMValueRef getOrCreateTuaFsReadfileAlloc(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_fs_readfile_alloc");
    if (existing) return existing;
    LLVMTypeRef i32 = LLVMInt32TypeInContext(compiler->context);
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef i8ptrptr = LLVMPointerType(i8ptr, 0);
    LLVMTypeRef i32ptr = LLVMPointerType(i32, 0);
    LLVMTypeRef params[3] = { i8ptr, i8ptrptr, i32ptr };
    LLVMTypeRef fnType = LLVMFunctionType(i32, params, 3, 0);
    return LLVMAddFunction(compiler->module, "tua_fs_readfile_alloc", fnType);
}

static LLVMValueRef getOrCreateTuaFsWritefileStr(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_fs_writefile_str");
    if (existing) return existing;
    LLVMTypeRef i32 = LLVMInt32TypeInContext(compiler->context);
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef params[2] = { i8ptr, i8ptr };
    LLVMTypeRef fnType = LLVMFunctionType(i32, params, 2, 0);
    return LLVMAddFunction(compiler->module, "tua_fs_writefile_str", fnType);
}

static LLVMValueRef getOrCreateTuaFsStatSimple(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_fs_stat_simple");
    if (existing) return existing;
    LLVMTypeRef i32 = LLVMInt32TypeInContext(compiler->context);
    LLVMTypeRef i64 = LLVMInt64TypeInContext(compiler->context);
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef i32ptr = LLVMPointerType(i32, 0);
    LLVMTypeRef i64ptr = LLVMPointerType(i64, 0);
    LLVMTypeRef params[5] = { i8ptr, i32ptr, i64ptr, i64ptr, i32ptr };
    LLVMTypeRef fnType = LLVMFunctionType(i32, params, 5, 0);
    return LLVMAddFunction(compiler->module, "tua_fs_stat_simple", fnType);
}

static LLVMValueRef getOrCreateTuaFsMkdir(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_fs_mkdir");
    if (existing) return existing;
    LLVMTypeRef i32 = LLVMInt32TypeInContext(compiler->context);
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef params[2] = { i8ptr, i32 };
    LLVMTypeRef fnType = LLVMFunctionType(i32, params, 2, 0);
    return LLVMAddFunction(compiler->module, "tua_fs_mkdir", fnType);
}

static LLVMValueRef getOrCreateTuaFsRealpathAlloc(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_fs_realpath_alloc");
    if (existing) return existing;
    LLVMTypeRef i32 = LLVMInt32TypeInContext(compiler->context);
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef i8ptrptr = LLVMPointerType(i8ptr, 0);
    LLVMTypeRef params[2] = { i8ptr, i8ptrptr };
    LLVMTypeRef fnType = LLVMFunctionType(i32, params, 2, 0);
    return LLVMAddFunction(compiler->module, "tua_fs_realpath_alloc", fnType);
}

static LLVMValueRef getOrCreateTuaFsReaddirArr(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_fs_readdir_arr");
    if (existing) return existing;
    LLVMTypeRef i32 = LLVMInt32TypeInContext(compiler->context);
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef i32ptr = LLVMPointerType(i32, 0);
    LLVMTypeRef arrType = compilerGetArrayType(compiler);
    LLVMTypeRef params[2] = { i8ptr, i32ptr };
    LLVMTypeRef fnType = LLVMFunctionType(arrType, params, 2, 0);
    return LLVMAddFunction(compiler->module, "tua_fs_readdir_arr", fnType);
}

static LLVMValueRef getOrCreateTuaFsWritefileStrAsyncCl(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_fs_writefile_str_async_cl");
    if (existing) return existing;
    LLVMTypeRef i32 = LLVMInt32TypeInContext(compiler->context);
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef closure = compilerGetClosureType(compiler);
    LLVMTypeRef params[5] = { i8ptr, i8ptr, i8ptr, i8ptr, closure };
    LLVMTypeRef fnType = LLVMFunctionType(i32, params, 5, 0);
    return LLVMAddFunction(compiler->module, "tua_fs_writefile_str_async_cl", fnType);
}

static LLVMValueRef getOrCreateTuaFsStatAsyncCl(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_fs_stat_async_cl");
    if (existing) return existing;
    LLVMTypeRef i32 = LLVMInt32TypeInContext(compiler->context);
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef closure = compilerGetClosureType(compiler);
    LLVMTypeRef params[4] = { i8ptr, i8ptr, i8ptr, closure };
    LLVMTypeRef fnType = LLVMFunctionType(i32, params, 4, 0);
    return LLVMAddFunction(compiler->module, "tua_fs_stat_async_cl", fnType);
}

static LLVMValueRef getOrCreateTuaFsReaddirAsyncCl(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_fs_readdir_async_cl");
    if (existing) return existing;
    LLVMTypeRef i32 = LLVMInt32TypeInContext(compiler->context);
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef closure = compilerGetClosureType(compiler);
    LLVMTypeRef params[4] = { i8ptr, i8ptr, i8ptr, closure };
    LLVMTypeRef fnType = LLVMFunctionType(i32, params, 4, 0);
    return LLVMAddFunction(compiler->module, "tua_fs_readdir_async_cl", fnType);
}

static LLVMValueRef getOrCreateTuaFsStringArrayFree(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_fs_string_array_free");
    if (existing) return existing;
    LLVMTypeRef arrType = compilerGetArrayType(compiler);
    LLVMTypeRef params[1] = { arrType };
    LLVMTypeRef fnType = LLVMFunctionType(LLVMVoidTypeInContext(compiler->context), params, 1, 0);
    return LLVMAddFunction(compiler->module, "tua_fs_string_array_free", fnType);
}

enum {
    TUA_VAL_NIL = 0,
    TUA_VAL_INT = 1,
    TUA_VAL_LONG = 2,
    TUA_VAL_DOUBLE = 3,
    TUA_VAL_BOOL = 4,
    TUA_VAL_STRING = 5,
    TUA_VAL_PTR = 6
};

static LLVMValueRef tuaValueMake(Compiler* compiler, int tag, LLVMValueRef payloadI64) {
    LLVMTypeRef vt = compilerGetTuaValueType(compiler);
    LLVMValueRef v = LLVMGetUndef(vt);
    LLVMValueRef tagV = LLVMConstInt(LLVMInt32TypeInContext(compiler->context), (unsigned)tag, 0);
    LLVMValueRef payload = payloadI64 ? payloadI64 : LLVMConstInt(LLVMInt64TypeInContext(compiler->context), 0, 0);
    v = LLVMBuildInsertValue(compiler->builder, v, tagV, 0, "t_tag");
    v = LLVMBuildInsertValue(compiler->builder, v, payload, 1, "t_payload");
    return v;
}

static LLVMValueRef tuaValueFromKey(Compiler* compiler, LLVMValueRef key) {
    LLVMTypeRef t = LLVMTypeOf(key);
    LLVMTypeKind k = LLVMGetTypeKind(t);
    LLVMTypeRef i64 = LLVMInt64TypeInContext(compiler->context);

    if (k == LLVMIntegerTypeKind) {
        unsigned bits = LLVMGetIntTypeWidth(t);
        LLVMValueRef k64 = key;
        if (bits < 64) k64 = LLVMBuildSExt(compiler->builder, key, i64, "k_sext");
        else if (bits > 64) k64 = LLVMBuildTrunc(compiler->builder, key, i64, "k_trunc");
        return tuaValueMake(compiler, TUA_VAL_LONG, k64);
    }

    if (k == LLVMPointerTypeKind) {
        LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
        if (t != i8ptr) {
            emitDebug("Map key pointer type must be string (i8*)\n");
            return NULL;
        }
        LLVMValueRef p64 = LLVMBuildPtrToInt(compiler->builder, key, i64, "k_ptr");
        return tuaValueMake(compiler, TUA_VAL_STRING, p64);
    }

    emitDebug("Map key must be int/long/string\n");
    return NULL;
}

static int isOptionLLVMType(LLVMTypeRef t) {
    if (!t) return 0;
    if (LLVMGetTypeKind(t) != LLVMStructTypeKind) return 0;
    if (LLVMCountStructElementTypes(t) != 2) return 0;
    LLVMTypeRef f0 = LLVMStructGetTypeAtIndex(t, 0);
    if (LLVMGetTypeKind(f0) != LLVMIntegerTypeKind) return 0;
    return LLVMGetIntTypeWidth(f0) == 1;
}

static int isSliceLLVMType(LLVMTypeRef t) {
    if (!t) return 0;
    if (LLVMGetTypeKind(t) != LLVMStructTypeKind) return 0;
    const char* n = LLVMGetStructName(t);
    return n && strncmp(n, "tua_slice$", 10) == 0;
}

static int isTuaValueLLVMType(Compiler* compiler, LLVMTypeRef t) {
    if (!compiler || !t) return 0;
    if (LLVMGetTypeKind(t) != LLVMStructTypeKind) return 0;
    return t == compilerGetTuaValueType(compiler);
}

static int isStringLLVMType(Compiler* compiler, LLVMTypeRef t) {
    if (!compiler || !t) return 0;
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    return t == i8ptr;
}

static int isBoolLLVMType(LLVMTypeRef t) {
    if (!t) return 0;
    if (LLVMGetTypeKind(t) != LLVMIntegerTypeKind) return 0;
    return LLVMGetIntTypeWidth(t) == 1;
}

static int isNumericLLVMType(LLVMTypeRef t) {
    if (!t) return 0;
    LLVMTypeKind k = LLVMGetTypeKind(t);
    if (k != LLVMIntegerTypeKind) return 0;
    return LLVMGetIntTypeWidth(t) != 1;
}

static int isScalarValueLLVMType(Compiler* compiler, LLVMTypeRef t) {
    if (!compiler || !t) return 0;
    if (isStringLLVMType(compiler, t)) return 1;
    if (isBoolLLVMType(t)) return 1;
    if (LLVMGetTypeKind(t) == LLVMIntegerTypeKind && LLVMGetIntTypeWidth(t) != 1) return 1;
    if (LLVMGetTypeKind(t) == LLVMFloatTypeKind || LLVMGetTypeKind(t) == LLVMDoubleTypeKind) return 1;
    return 0;
}

// Under LLVM opaque pointers, all pointers share the same LLVM type (`ptr`), so LLVMTypeRef
// checks cannot distinguish `string` vs `map` vs `array`. For typed map semantics, prefer
// stored TypeKind metadata from the parser/analyzer.
static int typeKindIsScalarValueKind(TypeKind k) {
    switch (k) {
        case TYPE_BOOL:
        case TYPE_STRING:
        case TYPE_PTR:
        case TYPE_BYTE:
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
        case TYPE_F16:
        case TYPE_FLOAT:
        case TYPE_DOUBLE:
        case TYPE_BF16:
            return 1;
        default:
            return 0;
    }
}

static int implTypeNameForTypeKind(TypeKind k, const char** outName, int* outLen) {
    if (outName) *outName = NULL;
    if (outLen) *outLen = 0;
    const char* n = NULL;
    int l = 0;
    switch (k) {
        case TYPE_BOOL: n = "bool"; l = 4; break;
        case TYPE_STRING: n = "string"; l = 6; break;
        case TYPE_PTR: n = "ptr"; l = 3; break;
        case TYPE_BYTE: n = "byte"; l = 4; break;
        case TYPE_I8: n = "i8"; l = 2; break;
        case TYPE_I16: n = "i16"; l = 3; break;
        case TYPE_INT: n = "int"; l = 3; break;
        case TYPE_LONG: n = "long"; l = 4; break;
        case TYPE_ISIZE: n = "isize"; l = 5; break;
        case TYPE_U8: n = "u8"; l = 2; break;
        case TYPE_U16: n = "u16"; l = 3; break;
        case TYPE_U32: n = "u32"; l = 3; break;
        case TYPE_U64: n = "u64"; l = 3; break;
        case TYPE_USIZE: n = "usize"; l = 5; break;
        case TYPE_F8: n = "fp8"; l = 3; break;
        case TYPE_BF8: n = "bfp8"; l = 4; break;
        case TYPE_F16: n = "half"; l = 4; break;
        case TYPE_BF16: n = "bfloat"; l = 6; break;
        case TYPE_FLOAT: n = "float"; l = 5; break;
        case TYPE_DOUBLE: n = "double"; l = 6; break;
        case TYPE_ARRAY: n = "array"; l = 5; break;
        default:
            return 0;
    }
    if (outName) *outName = n;
    if (outLen) *outLen = l;
    return 1;
}

static int typedMapValueIsScalarMeta(const VariableRef* recvVar) {
    if (!recvVar || !recvVar->isTypedMap) return 0;
    if (typeKindIsScalarValueKind(recvVar->mapValueKind)) return 1;
    // Arrays, maps, and structs are all non-scalar for map.get()/getMut() element refs.
    return 0;
}

static int typedMapKeyCompatible(Compiler* compiler, LLVMTypeRef expectedKeyTy, LLVMValueRef keyVal) {
    if (!compiler || !expectedKeyTy || !keyVal) return 0;
    LLVMTypeRef actualTy = LLVMTypeOf(keyVal);
    if (isTuaValueLLVMType(compiler, actualTy)) return 0;
    if (isStringLLVMType(compiler, expectedKeyTy)) return isStringLLVMType(compiler, actualTy);
    return isNumericLLVMType(actualTy);
}

static LLVMTypeRef getPrintfType(Compiler* compiler) {
    LLVMTypeRef contextI8 = LLVMInt8TypeInContext(compiler->context);
    LLVMTypeRef printfParamTypes[] = { LLVMPointerType(contextI8, 0) };
    return LLVMFunctionType(
        LLVMInt32TypeInContext(compiler->context),
        printfParamTypes,
        1,
        1
    );
}

static const char* formatForValue(LLVMValueRef value, int addNewline) {
    if (!value) return NULL;

    LLVMTypeRef type = LLVMTypeOf(value);
    LLVMTypeKind kind = LLVMGetTypeKind(type);

    if (kind == LLVMIntegerTypeKind) {
        unsigned bits = LLVMGetIntTypeWidth(type);
        if (bits == 1) return addNewline ? "%d\n" : "%d";
        if (bits <= 32) return addNewline ? "%d\n" : "%d";
        return addNewline ? "%lld\n" : "%lld";
    }

    if (kind == LLVMFloatTypeKind || kind == LLVMDoubleTypeKind) {
        return addNewline ? "%f\n" : "%f";
    }

    if (kind == LLVMPointerTypeKind) {
        return addNewline ? "%s\n" : "%s";
    }

    return NULL;
}

LLVMTypeRef buildMethodType() {
    //TODO
    return NULL;
}

// LLVMValueRef emitCallExpr2(Compiler* compiler, CallExpr* expr) {
//     // Get object type and method
//     VariableRef obj = findVariableExpr(compiler, expr->caller);
//     char* methodName = getMethodName(expr->callee);
    
//     // Create struct type with vtable
//     LLVMTypeRef vtableType = LLVMStructCreateNamed(compiler->context, "vtable");
//     LLVMTypeRef objType = LLVMStructCreateNamed(compiler->context, "object");
    
//     // Add vtable pointer to object struct
//     LLVMTypeRef elementTypes[] = {
//         LLVMPointerType(vtableType, 0), // vtable ptr
//         // ... other fields
//     };
//     LLVMStructSetBody(objType, elementTypes, 1, false);
    
//     // Get method from vtable
//     LLVMValueRef vtablePtr = LLVMBuildStructGEP2(
//         compiler->builder, 
//         objType,
//         obj.value, 
//         0, 
//         "vtable"
//     );
    
//     // Build method call with this pointer
//     LLVMValueRef method = LLVMBuildLoad2(
//         compiler->builder,
//         LLVMPointerType(
//             buildMethodType(),
//             // LLVMFunctionType(
//             // LLVMVoidType(compiler->context), 
//             // NULL, 
//             // 0, 
//             // false
//             // ), 
//         0),
//         vtablePtr,
//         "method"
//     );
    
//     // Add this as first argument
//     LLVMValueRef args[expr->arguments->length + 1];
//     args[0] = obj.value; // this pointer
    
//     // Add other arguments
//     ListNode* arg = expr->arguments->head;
//     int i = 1;
//     while (arg != NULL) {
//         args[i++] = compileExpr(compiler, (Expr*)arg->data);
//         arg = arg->next; 
//     }
    
//     // Call method
//     return LLVMBuildCall2(
//         compiler->builder,
//         LLVMTypeOf(method),
//         method,
//         args,
//         expr->arguments->length + 1,
//         "call"
//     );
// }



static LLVMValueRef collapseMultiReturnIfNeeded(Compiler* compiler, LLVMValueRef func, LLVMValueRef call) {
    if (!compiler || !func || !call) return call;
    if (compiler->wantMultiValue) return call;
    const char* name = LLVMGetValueName(func);
    int len = name ? (int)strlen(name) : 0;
    int cnt = compilerMultiReturnCount(compiler, name, len);
    if (cnt > 1) {
        return LLVMBuildExtractValue(compiler->builder, call, 0, "mv0");
    }
    return call;
}

static LLVMValueRef collapseMultiReturnByTypeIfNeeded(Compiler* compiler, LLVMValueRef call, LLVMTypeRef retType) {
    if (!compiler || !call || !retType) return call;
    if (compiler->wantMultiValue) return call;
    if (LLVMGetTypeKind(retType) == LLVMStructTypeKind) {
        // Avoid collapsing real struct returns (including closures and user-defined structs).
        // Multi-return tuples are lowered as anonymous structs; those use the default "first value" rule.
        LLVMTypeRef closureTy = compilerGetClosureType(compiler);
        if (retType == closureTy) return call;

        // Named structs are user-defined (or named runtime types) and must not be collapsed.
        const char* structName = LLVMGetStructName(retType);
        if (structName && structName[0] != '\0') return call;

        // Option<T> is also a 2-field anonymous struct; do not collapse it.
        if (LLVMCountStructElementTypes(retType) == 2) {
            LLVMTypeRef tagTy = LLVMStructGetTypeAtIndex(retType, 0);
            if (LLVMGetTypeKind(tagTy) == LLVMIntegerTypeKind && LLVMGetIntTypeWidth(tagTy) == 1) {
                return call;
            }
        }

        return LLVMBuildExtractValue(compiler->builder, call, 0, "mv0");
    }
    return call;
}

static LLVMValueRef loadLocalValue(Compiler* compiler, VariableRef var) {
    if (!compiler || !var.value || !var.type) return NULL;
    if (var.isGlobal) return var.value;
    if (var.isBoxed) {
        if (!var.boxPtrType) return NULL;
        LLVMValueRef cellPtr = LLVMBuildLoad2(compiler->builder, var.boxPtrType, var.value, "cellptr");
        return LLVMBuildLoad2(compiler->builder, var.type, cellPtr, "load");
    }
    return LLVMBuildLoad2(compiler->builder, var.type, var.value, "load");
}

LLVMValueRef emitCallExpr(Compiler* compiler, CallExpr* expr) {
    emitDebug("emitCallExpr\n");

    if (!expr || !expr->callee) return NULL;
    int hasTypeArgs = expr->typeArgs && expr->typeArgs->length > 0;

    // Multi-return selection is only for the current call's return value.
    // Nested calls (arguments) should keep the default "first value" rule.
    int wantMultiForThisCall = compiler ? compiler->wantMultiValue : 0;
    if (compiler) compiler->wantMultiValue = 0;

    // Immediate lambda call: (fn(...) { ... })(args)
    if (expr->callee->type == EXPR_LAMBDA) {
        if (hasTypeArgs) {
            compilerErrorAtToken(compiler, &expr->base.token, "generic type arguments are not supported on lambda calls");
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        LLVMValueRef closureVal = compileExpr(compiler, expr->callee);
        LLVMTypeRef closureType = compilerGetClosureType(compiler);
        if (!closureVal || LLVMTypeOf(closureVal) != closureType) {
            emitDebug("Failed to compile lambda callee\n");
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }

        LLVMValueRef fnPtr = LLVMBuildExtractValue(compiler->builder, closureVal, 0, "fnptr");
        LLVMValueRef envPtr = LLVMBuildExtractValue(compiler->builder, closureVal, 1, "envptr");

        LLVMTypeRef fnType = compiler->lastLambdaFuncType;
        if (!fnType) {
            emitDebug("Missing lambda signature\n");
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }

        unsigned expected = LLVMCountParamTypes(fnType);
        unsigned got = expr->arguments ? (unsigned)expr->arguments->length : 0;
        if (expected != got + 1) {
            emitDebug("Lambda argument count mismatch\n");
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }

        LLVMTypeRef* paramTypes = NULL;
        if (expected > 0) {
            paramTypes = malloc(sizeof(LLVMTypeRef) * (size_t)expected);
            LLVMGetParamTypes(fnType, paramTypes);
        }

        LLVMValueRef* args = NULL;
        if (expected > 0) {
            args = malloc(sizeof(LLVMValueRef) * (size_t)expected);
            args[0] = castValueToType(compiler, envPtr, paramTypes[0]);
            ListNode* node = expr->arguments ? expr->arguments->head : NULL;
            for (unsigned i = 1; i < expected; i++) {
                LLVMValueRef av = compileCallArgForParam(compiler, (Expr*)node->data, paramTypes[i]);
                if (!av) {
                    if (paramTypes) free(paramTypes);
                    if (args) free(args);
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return NULL;
                }
                args[i] = av;
                node = node->next;
            }
        }

        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
        LLVMValueRef call = LLVMBuildCall2(compiler->builder, fnType, fnPtr, args, expected, callInstNameForFnType(fnType));
        if (expr->arguments && expected > 1) {
            dropTemporaryClosureArgs(compiler, expr->arguments, args, 1);
        }
        LLVMTypeRef retType = LLVMGetReturnType(fnType);
        LLVMValueRef out = collapseMultiReturnByTypeIfNeeded(compiler, call, retType);
        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;

        if (paramTypes) free(paramTypes);
        if (args) free(args);
        return out;
    }

    // Call a closure value returned by a call-chain:
    // - `makeAdder(1)(2)`
    // - `bar(1)(2)(3)` (nested closure returns)
    // Requires that the base function has an explicit (possibly nested) closure return type.
    Expr* calleeExpr = unwrapGroupingExpr(expr->callee);
    if (calleeExpr && calleeExpr->type == EXPR_CALL) {
        if (hasTypeArgs) {
            compilerErrorAtToken(compiler, &expr->base.token, "generic type arguments are not supported on closure calls");
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        int depth = 0;
        CallExpr* baseCall = findInnermostCallInChain(calleeExpr, &depth);
        int level = depth > 0 ? (depth - 1) : 0;
        LLVMTypeRef fnType = baseCall ? resolveClosureReturnSigForSimpleCallAt(compiler, baseCall, level) : NULL;
        if (fnType) {
            LLVMValueRef closureVal = compileExpr(compiler, calleeExpr);
            LLVMTypeRef closureType = compilerGetClosureType(compiler);
            if (!closureVal || LLVMTypeOf(closureVal) != closureType) {
                emitDebug("Expected closure return value\n");
                if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                return NULL;
            }

            LLVMValueRef fnPtr = LLVMBuildExtractValue(compiler->builder, closureVal, 0, "fnptr");
            LLVMValueRef envPtr = LLVMBuildExtractValue(compiler->builder, closureVal, 1, "envptr");

            unsigned expected = LLVMCountParamTypes(fnType);
            unsigned got = expr->arguments ? (unsigned)expr->arguments->length : 0;
            if (expected != got + 1) {
                compilerErrorAtToken(
                    compiler,
                    &expr->base.token,
                    "argument count mismatch for closure call: expected %u, got %u",
                    expected > 0 ? (unsigned)(expected - 1) : 0u,
                    got
                );
                if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                return NULL;
            }

            LLVMTypeRef* paramTypes = NULL;
            if (expected > 0) {
                paramTypes = malloc(sizeof(LLVMTypeRef) * (size_t)expected);
                LLVMGetParamTypes(fnType, paramTypes);
            }

            LLVMValueRef* args = NULL;
            if (expected > 0) {
                args = malloc(sizeof(LLVMValueRef) * (size_t)expected);
                args[0] = castValueToType(compiler, envPtr, paramTypes[0]);
                ListNode* node = expr->arguments ? expr->arguments->head : NULL;
                for (unsigned i = 1; i < expected; i++) {
                    LLVMValueRef av = compileCallArgForParam(compiler, (Expr*)node->data, paramTypes[i]);
                    if (!av) {
                        if (paramTypes) free(paramTypes);
                        if (args) free(args);
                        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                        return NULL;
                    }
                    args[i] = av;
                    node = node->next;
                }
            }

            LLVMValueRef call = LLVMBuildCall2(compiler->builder, fnType, fnPtr, args, expected, callInstNameForFnType(fnType));
            if (expr->arguments && expected > 1) {
                dropTemporaryClosureArgs(compiler, expr->arguments, args, 1);
            }
            if (paramTypes) free(paramTypes);
            if (args) free(args);
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            LLVMValueRef out = collapseMultiReturnByTypeIfNeeded(compiler, call, LLVMGetReturnType(fnType));
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return out;
        }
    }

    // Member call:
    // - Instance: p.method(...) -> Struct__method(p, ...)
    // - Object:   Obj.method(...) -> Obj__method(...)
    if (expr->callee->type == EXPR_GET) {
        GetExpr* get = (GetExpr*)expr->callee;
        if (!get->object) {
            emitDebug("Unsupported member call receiver\n");
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }

        // Namespace-qualified calls:
        // - `ns.Name(...)` where `ns` is imported via `import "path" as ns`
        // - `ns.Type.method(...)` for imported object/enum/struct static methods
        if (get->object->type == EXPR_VARIABLE) {
            VariableExpr* ns = (VariableExpr*)get->object;
            SymbolAlias* a = compilerFindAlias(compiler, ns->name.start, ns->name.length);
            if (a && a->kind == ALIAS_MODULE) {
                int ql = 0;
                char* q = mangleRawAndToken(a->qualified, a->qualifiedLen, &get->name, &ql);
                if (hasTypeArgs) {
                    LLVMValueRef gen = compilerInstantiateGenericFunc(compiler, q, ql, expr->typeArgs, &get->name);
                    if (!gen) {
                        free(q);
                        compilerErrorAtToken(compiler, &get->name, "undefined generic function '%.*s' in namespace '%.*s'",
                            get->name.length, get->name.start, ns->name.length, ns->name.start);
                        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                        return NULL;
                    }
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    LLVMValueRef out = emitDirectFuncCall(compiler, gen, expr, get->name.line);
                    free(q);
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return out;
                }

                LLVMValueRef func = LLVMGetNamedFunction(compiler->module, q);
                if (func) {
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    LLVMValueRef out = emitDirectFuncCall(compiler, func, expr, get->name.line);
                    free(q);
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return out;
                }
                StructInfo* info = compilerFindStruct(compiler, q, ql);
                free(q);
                if (info) {
                    LLVMValueRef out = emitStructConstructor(compiler, info, expr);
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return out;
                }
                compilerErrorAt(compiler, get->name.line, "unknown import '%.*s' in namespace '%.*s'",
                    get->name.length, get->name.start, ns->name.length, ns->name.start);
                if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                return NULL;
            }
        }
        if (get->object->type == EXPR_GET) {
            GetExpr* inner = (GetExpr*)get->object;
            if (inner->object && inner->object->type == EXPR_VARIABLE) {
                VariableExpr* ns = (VariableExpr*)inner->object;
                SymbolAlias* a = compilerFindAlias(compiler, ns->name.start, ns->name.length);
                if (a && a->kind == ALIAS_MODULE) {
                    // Fast path: in `unsafe { ... }` (or `--unchecked-index`), lower `std/bytes.Bytes.getU8(b, i)`
                    // into an inline load returning `(value, 0)` (UB on null/oob).
                    if (!hasTypeArgs &&
                        compilerUncheckedIndex(compiler) &&
                        aliasIsStdBytesModule(a) &&
                        inner->name.length == 5 && memcmp(inner->name.start, "Bytes", 5) == 0 &&
                        get->name.length == 5 && memcmp(get->name.start, "getU8", 5) == 0) {
                        unsigned got = expr->arguments ? (unsigned)expr->arguments->length : 0;
                        if (got == 2) {
                            LLVMContextRef ctx = compiler->context;
                            LLVMTypeRef bytesTy = compilerGetBytesType(compiler);
                            LLVMTypeRef bytesStruct = LLVMGetTypeByName2(ctx, "tua_bytes");
                            if (!bytesStruct) bytesStruct = LLVMGetElementType(bytesTy);
                            LLVMTypeRef i64 = LLVMInt64TypeInContext(ctx);
                            LLVMTypeRef i32 = LLVMInt32TypeInContext(ctx);
                            LLVMTypeRef i8 = LLVMInt8TypeInContext(ctx);
                            LLVMTypeRef i8ptr = LLVMPointerType(i8, 0);

                            LLVMValueRef bArg = compileExpr(compiler, (Expr*)expr->arguments->head->data);
                            LLVMValueRef iArg = compileExpr(compiler, (Expr*)expr->arguments->head->next->data);
                            if (bArg && iArg) {
                                LLVMValueRef bPtr = castValueToType(compiler, bArg, bytesTy);
                                LLVMValueRef idx = castValueToType(compiler, iArg, i64);

                                LLVMValueRef dataPtrPtr = LLVMBuildStructGEP2(compiler->builder, bytesStruct, bPtr, 2, "bdatap");
                                LLVMValueRef data = LLVMBuildLoad2(compiler->builder, i8ptr, dataPtrPtr, "bdata");
                                LLVMValueRef ep = LLVMBuildGEP2(compiler->builder, i8, data, &idx, 1, "ep");
                                LLVMValueRef v8 = LLVMBuildLoad2(compiler->builder, i8, ep, "bv");
                                LLVMValueRef v32 = LLVMBuildZExt(compiler->builder, v8, i32, "bv32");

                                LLVMValueRef err0 = LLVMConstInt(i32, 0, 0);
                                if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                                if (compiler && compiler->wantMultiValue) {
                                    LLVMTypeRef fields[2] = { i32, i32 };
                                    LLVMTypeRef retTy = LLVMStructTypeInContext(ctx, fields, 2, 0);
                                    LLVMValueRef out = LLVMGetUndef(retTy);
                                    out = LLVMBuildInsertValue(compiler->builder, out, v32, 0, "mv0");
                                    out = LLVMBuildInsertValue(compiler->builder, out, err0, 1, "mv1");
                                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                                    return out;
                                }
                                if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                                return v32;
                            }
                        }
                    }

                    int ql1 = 0;
                    char* q1 = mangleRawAndToken(a->qualified, a->qualifiedLen, &inner->name, &ql1);
                    int ql2 = 0;
                    char* q2 = mangleRawAndToken(q1, ql1, &get->name, &ql2);
                    free(q1);

                    LLVMValueRef func = LLVMGetNamedFunction(compiler->module, q2);
                    if (!func) {
                        compilerErrorAt(compiler, get->name.line, "undefined method '%.*s' on '%.*s.%.*s'",
                            get->name.length, get->name.start,
                            ns->name.length, ns->name.start,
                            inner->name.length, inner->name.start);
                        free(q2);
                        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                        return NULL;
                    }
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    LLVMValueRef out = emitDirectFuncCall(compiler, func, expr, get->name.line);
                    free(q2);
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return out;
                }
            }
        }

        // Built-in member calls can accept non-variable receivers.
	        if (get->object->type != EXPR_VARIABLE) {
	            LLVMValueRef recvVal = compileExpr(compiler, get->object);
	            if (!recvVal) {
	                if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
	                return NULL;
	            }
	            LLVMTypeRef recvType = LLVMTypeOf(recvVal);
	            unsigned got = expr->arguments ? (unsigned)expr->arguments->length : 0;

	            // `impl <scalar/array> { ... }` methods can also be called on non-variable receivers
	            // when the receiver has a known inferred kind (or explicit type args are provided).
	            {
	                TypeKind k = get->object->inferredType;
	                Type* it = inferTypeFromValueExpr(compiler, get->object);
	                if (it) {
	                    k = it->kind;
	                    freeTypeTreeDeep(it);
	                }
	                const char* tn = NULL;
	                int tnLen = 0;
	                if (implTypeNameForTypeKind(k, &tn, &tnLen)) {
	                    int saw = 0;
	                    LLVMValueRef implOut = tryEmitBuiltinValueImplMethodCall(
	                        compiler,
	                        tn,
	                        tnLen,
	                        get->object,
	                        recvVal,
	                        get,
	                        expr,
	                        wantMultiForThisCall,
	                        &saw
	                    );
	                    if (implOut) return implOut;
	                }
	            }

	            // Map built-in methods on non-variable receivers are intentionally disabled for now:
	            // LLVM opaque pointers make it impossible to reliably distinguish `%tua_map*` from other pointers here.
	            if (0 && recvType == compilerGetMapType(compiler)) {
	                LLVMValueRef mapPtr = recvVal;

                if (tokenEquals(&get->name, "len")) {
                    if (got != 0) {
                        emitDebug("map.len expects 0 arguments\n");
                        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                        return NULL;
                    }
                    LLVMValueRef fn = getOrCreateTuaMapLen(compiler);
                    LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
                    LLVMValueRef args1[1] = { mapPtr };
                    LLVMValueRef out = LLVMBuildCall2(compiler->builder, fnType, fn, args1, 1, "mlen");
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return out;
                }

                if (tokenEquals(&get->name, "hasKey")) {
                    if (got != 1) {
                        emitDebug("map.hasKey expects 1 argument\n");
                        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                        return NULL;
                    }
                    LLVMValueRef keyExpr = compileExpr(compiler, (Expr*)expr->arguments->head->data);
                    LLVMValueRef key = tuaValueFromKey(compiler, keyExpr);
                    if (!key) {
                        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                        return NULL;
                    }
                    LLVMValueRef fn = getOrCreateTuaMapHas(compiler);
                    LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
                    LLVMValueRef args2[2] = { mapPtr, key };
                    LLVMValueRef ok32 = LLVMBuildCall2(compiler->builder, fnType, fn, args2, 2, "mhas");
                    LLVMValueRef ok = LLVMBuildTrunc(compiler->builder, ok32, LLVMInt1TypeInContext(compiler->context), "ok");
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return ok;
                }

                if (tokenEquals(&get->name, "delete")) {
                    if (got != 1) {
                        emitDebug("map.delete expects 1 argument\n");
                        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                        return NULL;
                    }
                    LLVMValueRef keyExpr = compileExpr(compiler, (Expr*)expr->arguments->head->data);
                    LLVMValueRef key = tuaValueFromKey(compiler, keyExpr);
                    if (!key) {
                        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                        return NULL;
                    }
                    LLVMValueRef fn = getOrCreateTuaMapDelete(compiler);
                    LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
                    LLVMValueRef args2[2] = { mapPtr, key };
                    LLVMValueRef ok32 = LLVMBuildCall2(compiler->builder, fnType, fn, args2, 2, "mdel32");
                    LLVMValueRef ok = LLVMBuildTrunc(compiler->builder, ok32, LLVMInt1TypeInContext(compiler->context), "mdel");
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return ok;
                }

                if (tokenEquals(&get->name, "clear")) {
                    if (got != 0) {
                        emitDebug("map.clear expects 0 arguments\n");
                        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                        return NULL;
                    }
                    LLVMValueRef fn = getOrCreateTuaMapClear(compiler);
                    LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
                    LLVMValueRef args1[1] = { mapPtr };
                    LLVMBuildCall2(compiler->builder, fnType, fn, args1, 1, "");
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return LLVMConstInt(LLVMInt32TypeInContext(compiler->context), 0, 0);
                }

                if (tokenEquals(&get->name, "get")) {
                    if (got != 1) {
                        emitDebug("map.get expects 1 argument\n");
                        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                        return NULL;
                    }
                    LLVMValueRef keyExpr = compileExpr(compiler, (Expr*)expr->arguments->head->data);
                    LLVMValueRef key = tuaValueFromKey(compiler, keyExpr);
                    if (!key) {
                        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                        return NULL;
                    }

                    LLVMTypeRef vt = compilerGetTuaValueType(compiler);
                    LLVMValueRef okPtr = LLVMBuildAlloca(compiler->builder, LLVMInt32TypeInContext(compiler->context), "mokptr");
                    LLVMValueRef gfn = getOrCreateTuaMapGetWithOk(compiler);
                    LLVMTypeRef gtype = LLVMGlobalGetValueType(gfn);
                    LLVMValueRef args3[3] = { mapPtr, key, okPtr };
                    LLVMValueRef tv = LLVMBuildCall2(compiler->builder, gtype, gfn, args3, 3, "mget");
                    LLVMValueRef ok32 = LLVMBuildLoad2(compiler->builder, LLVMInt32TypeInContext(compiler->context), okPtr, "mok32");
                    LLVMValueRef ok = LLVMBuildTrunc(compiler->builder, ok32, LLVMInt1TypeInContext(compiler->context), "mok");

                    LLVMTypeRef optType = compilerGetOptionType(compiler, vt);
                    LLVMValueRef opt = LLVMGetUndef(optType);
                    opt = LLVMBuildInsertValue(compiler->builder, opt, ok, 0, "o0");
                    opt = LLVMBuildInsertValue(compiler->builder, opt, tv, 1, "o1");
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return opt;
                }
            }

            // Array built-in methods on non-variable receivers are intentionally disabled for now
            // for the same reason as map methods (LLVM opaque pointers).
            if (0 && recvType == compilerGetArrayType(compiler)) {
                LLVMTypeRef arrType = compilerGetArrayType(compiler);
                LLVMTypeRef arrStruct = LLVMGetElementType(arrType);
                LLVMTypeRef i64 = LLVMInt64TypeInContext(compiler->context);
                LLVMTypeRef i32 = LLVMInt32TypeInContext(compiler->context);

                if (tokenEquals(&get->name, "len")) {
                    if (got != 0) {
                        emitDebug("array.len expects 0 arguments\n");
                        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                        return NULL;
                    }
                    LLVMValueRef lenPtr = LLVMBuildStructGEP2(compiler->builder, arrStruct, recvVal, 0, "alenp");
                    LLVMValueRef len64 = LLVMBuildLoad2(compiler->builder, i64, lenPtr, "alen64");
                    LLVMValueRef out = LLVMBuildTrunc(compiler->builder, len64, i32, "alen");
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return out;
                }

                if (tokenEquals(&get->name, "clone")) {
                    if (got != 0) {
                        emitDebug("array.clone expects 0 arguments\n");
                        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                        return NULL;
                    }
                    LLVMValueRef fn = getOrCreateTuaArrayClone(compiler);
                    LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
                    LLVMValueRef args1[1] = { recvVal };
                    LLVMValueRef out = LLVMBuildCall2(compiler->builder, fnType, fn, args1, 1, "aclone");
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return out;
                }
            }

            // Slice<T> built-in methods (non-variable receiver):
            // - `s.len() -> long`
            // - `s.get(i: long) -> T` (panics on invalid)
            if (isSliceLLVMType(recvType)) {
                int saw = 0;
                LLVMValueRef implOut = tryEmitBuiltinValueImplMethodCall(
                    compiler,
                    "Slice",
                    5,
                    get->object,
                    recvVal,
                    get,
                    expr,
                    wantMultiForThisCall,
                    &saw
                );
                if (implOut) return implOut;

                LLVMTypeRef elemTy = sliceElemTypeFromSliceStruct(compiler, recvType);
                if (!elemTy) {
                    elemTy = LLVMInt8TypeInContext(compiler->context);
                }

                LLVMValueRef data = LLVMBuildExtractValue(compiler->builder, recvVal, 0, "sdata");
                LLVMValueRef len = LLVMBuildExtractValue(compiler->builder, recvVal, 1, "slen");
                LLVMTypeRef i64 = LLVMInt64TypeInContext(compiler->context);

                if (tokenEquals(&get->name, "len")) {
                    if (got != 0) {
                        emitDebug("Slice.len expects 0 arguments\n");
                        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                        return NULL;
                    }
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return len;
                }

                if (tokenEquals(&get->name, "get")) {
                    if (got != 1) {
                        emitDebug("Slice.get expects 1 argument\n");
                        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                        return NULL;
                    }
                    LLVMValueRef idx = compileExpr(compiler, (Expr*)expr->arguments->head->data);
                    if (!idx) {
                        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                        return NULL;
                    }
                    idx = castValueToType(compiler, idx, i64);
                    LLVMValueRef zero = LLVMConstInt(i64, 0, 0);
                    LLVMValueRef neg = LLVMBuildICmp(compiler->builder, LLVMIntSLT, idx, zero, "idxneg");
                    LLVMValueRef ge = LLVMBuildICmp(compiler->builder, LLVMIntSGE, idx, len, "idxge");
                    LLVMValueRef bad = LLVMBuildOr(compiler->builder, neg, ge, "bad");

                    LLVMValueRef fn = compiler->current->func;
                    LLVMBasicBlockRef okB = LLVMAppendBasicBlock(fn, "sg_ok");
                    LLVMBasicBlockRef badB = LLVMAppendBasicBlock(fn, "sg_bad");
                    LLVMBasicBlockRef contB = LLVMAppendBasicBlock(fn, "sg_cont");
                    LLVMBuildCondBr(compiler->builder, bad, badB, okB);

                    LLVMPositionBuilderAtEnd(compiler->builder, badB);
                    LLVMValueRef panicFn = getOrCreateTuaPanic(compiler);
                    LLVMTypeRef pty = LLVMGlobalGetValueType(panicFn);
                    LLVMValueRef msg = LLVMBuildGlobalStringPtr(compiler->builder, "Slice.get out of bounds", "sgmsg");
                    LLVMBuildCall2(compiler->builder, pty, panicFn, &msg, 1, "");
                    LLVMBuildUnreachable(compiler->builder);

                    LLVMPositionBuilderAtEnd(compiler->builder, okB);
                    LLVMTypeRef elemPtrTy = LLVMPointerType(elemTy, 0);
                    LLVMValueRef typed = castValueToType(compiler, data, elemPtrTy);
                    LLVMValueRef ptr = LLVMBuildGEP2(compiler->builder, elemTy, typed, &idx, 1, "ep");
                    LLVMValueRef val = LLVMBuildLoad2(compiler->builder, elemTy, ptr, "sv");
                    LLVMBuildBr(compiler->builder, contB);

                    LLVMPositionBuilderAtEnd(compiler->builder, contB);
                    LLVMValueRef phi = LLVMBuildPhi(compiler->builder, elemTy, "sget");
                    LLVMAddIncoming(phi, &val, &okB, 1);
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return phi;
                }
            }

            // Option built-in methods: `opt.isSome()`, `opt.unwrap()`, ...
            if (isOptionLLVMType(recvType)) {
                int saw = 0;
                LLVMValueRef implOut = tryEmitBuiltinValueImplMethodCall(
                    compiler,
                    "Option",
                    6,
                    get->object,
                    recvVal,
                    get,
                    expr,
                    wantMultiForThisCall,
                    &saw
                );
                if (implOut) return implOut;

                LLVMTypeRef innerType = LLVMStructGetTypeAtIndex(recvType, 1);
                LLVMValueRef ok = LLVMBuildExtractValue(compiler->builder, recvVal, 0, "opt_ok");
                LLVMValueRef payload = LLVMBuildExtractValue(compiler->builder, recvVal, 1, "opt_v");

                if (tokenEquals(&get->name, "isSome")) {
                    if (got != 0) {
                        emitDebug("Option.isSome expects 0 arguments\n");
                        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                        return NULL;
                    }
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return ok;
                }

                if (tokenEquals(&get->name, "isNone")) {
                    if (got != 0) {
                        emitDebug("Option.isNone expects 0 arguments\n");
                        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                        return NULL;
                    }
                    LLVMValueRef out = LLVMBuildNot(compiler->builder, ok, "opt_not");
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return out;
                }

                if (tokenEquals(&get->name, "unwrap")) {
                    if (got != 0) {
                        emitDebug("Option.unwrap expects 0 arguments\n");
                        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                        return NULL;
                    }
                    LLVMValueRef fn = compiler->current->func;
                    LLVMBasicBlockRef someBB = LLVMAppendBasicBlock(fn, "opt.unwrap.some");
                    LLVMBasicBlockRef noneBB = LLVMAppendBasicBlock(fn, "opt.unwrap.none");
                    LLVMBasicBlockRef contBB = LLVMAppendBasicBlock(fn, "opt.unwrap.cont");
                    LLVMBuildCondBr(compiler->builder, ok, someBB, noneBB);

                    LLVMPositionBuilderAtEnd(compiler->builder, noneBB);
                    LLVMValueRef panicFn = getOrCreateTuaPanic(compiler);
                    LLVMTypeRef panicType = LLVMGlobalGetValueType(panicFn);
                    LLVMValueRef msg = LLVMBuildGlobalStringPtr(compiler->builder, "unwrap on None", "panicmsg");
                    LLVMBuildCall2(compiler->builder, panicType, panicFn, &msg, 1, "");
                    LLVMBuildUnreachable(compiler->builder);

                    LLVMPositionBuilderAtEnd(compiler->builder, someBB);
                    LLVMBuildBr(compiler->builder, contBB);

                    LLVMPositionBuilderAtEnd(compiler->builder, contBB);
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return payload;
                }

                if (tokenEquals(&get->name, "unwrapOr")) {
                    if (got != 1) {
                        emitDebug("Option.unwrapOr expects 1 argument\n");
                        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                        return NULL;
                    }
                    LLVMValueRef defRaw = compileExpr(compiler, (Expr*)expr->arguments->head->data);
                    defRaw = castValueToType(compiler, defRaw, innerType);

                    LLVMValueRef fn = compiler->current->func;
                    LLVMBasicBlockRef someBB = LLVMAppendBasicBlock(fn, "opt.uor.some");
                    LLVMBasicBlockRef noneBB = LLVMAppendBasicBlock(fn, "opt.uor.none");
                    LLVMBasicBlockRef contBB = LLVMAppendBasicBlock(fn, "opt.uor.cont");
                    LLVMBuildCondBr(compiler->builder, ok, someBB, noneBB);

                    LLVMPositionBuilderAtEnd(compiler->builder, someBB);
                    LLVMBuildBr(compiler->builder, contBB);
                    LLVMBasicBlockRef someEnd = LLVMGetInsertBlock(compiler->builder);

                    LLVMPositionBuilderAtEnd(compiler->builder, noneBB);
                    LLVMBuildBr(compiler->builder, contBB);
                    LLVMBasicBlockRef noneEnd = LLVMGetInsertBlock(compiler->builder);

                    LLVMPositionBuilderAtEnd(compiler->builder, contBB);
                    LLVMValueRef phi = LLVMBuildPhi(compiler->builder, innerType, "uor");
                    LLVMAddIncoming(phi, &payload, &someEnd, 1);
                    LLVMAddIncoming(phi, &defRaw, &noneEnd, 1);
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return phi;
                }
            }

            emitDebug("Unsupported member call receiver\n");
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }

        VariableExpr* recvNameExpr = (VariableExpr*)get->object;

        // If receiver resolves to a local and has a struct type, treat as instance method call.
        VariableRef recvVar = findVariableExpr(compiler, get->object);

	        // Ref.get(): load through a reference variable (replaces `*r` syntax).
	        if (recvVar.value && recvVar.pointeeType && tokenEquals(&get->name, "get")) {
	            unsigned got = expr->arguments ? (unsigned)expr->arguments->length : 0;
	            if (got != 0) {
                compilerErrorAt(compiler, get->name.line, "Ref.get expects 0 arguments");
                if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                return NULL;
            }
            LLVMValueRef refPtr = NULL;
            if (recvVar.isBoxed) {
                if (!recvVar.boxPtrType) {
                    compilerErrorAt(compiler, get->name.line, "missing boxed pointer type metadata");
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return NULL;
                }
                LLVMValueRef cell = LLVMBuildLoad2(compiler->builder, recvVar.boxPtrType, recvVar.value, "cell");
                refPtr = LLVMBuildLoad2(compiler->builder, recvVar.type, cell, "refp");
            } else {
                refPtr = LLVMBuildLoad2(compiler->builder, recvVar.type, recvVar.value, "refp");
            }
            LLVMValueRef out = LLVMBuildLoad2(compiler->builder, recvVar.pointeeType, refPtr, "rget");
	            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
	            return out;
	        }

	        // Trait object method call (dynamic dispatch) through a reference:
	        // `r: Ref<Trait>` allows `r.m(...)` without extracting an owned trait object value.
	        if (recvVar.value && recvVar.pointeeType) {
	            TraitInfo* trait = findTraitByObjType(compiler, recvVar.pointeeType);
	            if (trait) {
	                int midx = traitMethodIndex(trait, &get->name);
	                if (midx < 0) {
	                    compilerErrorAtToken(
	                        compiler,
	                        &get->name,
	                        "method '%.*s' is not in trait '%.*s'",
	                        get->name.length,
	                        get->name.start,
	                        trait->nameLength,
	                        trait->name
	                    );
	                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
	                    return NULL;
	                }
	                TraitMethodDecl* m = traitMethodDeclAt(trait, midx);
	                if (!m) {
	                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
	                    return NULL;
	                }

	                LLVMValueRef objPtr = loadLocalValue(compiler, recvVar); // Trait__obj*
	                if (!objPtr) {
	                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
	                    return NULL;
	                }
	                LLVMValueRef obj = LLVMBuildLoad2(compiler->builder, recvVar.pointeeType, objPtr, "to_ref");
	                LLVMTypeRef objTy = LLVMTypeOf(obj);
	                if (LLVMGetTypeKind(objTy) != LLVMStructTypeKind || LLVMCountStructElementTypes(objTy) != 2) {
	                    compilerErrorAtToken(compiler, &get->name, "invalid trait object representation");
	                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
	                    return NULL;
	                }

	                LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
	                LLVMValueRef data = LLVMBuildExtractValue(compiler->builder, obj, 0, "to_data");
	                LLVMValueRef vtp = LLVMBuildExtractValue(compiler->builder, obj, 1, "to_vt");

	                LLVMTypeRef vtTy = compilerGetTraitVtableType(compiler, trait);
	                LLVMValueRef vtptr = LLVMBuildBitCast(compiler->builder, vtp, LLVMPointerType(vtTy, 0), "vtptr");
	                LLVMValueRef slotPtr = LLVMBuildStructGEP2(compiler->builder, vtTy, vtptr, (unsigned)(1 + midx), "vt_m_p");
	                LLVMValueRef fnRaw = LLVMBuildLoad2(compiler->builder, i8ptr, slotPtr, "vt_m");

	                int argc = m->params ? m->params->length : 0;
	                unsigned got = expr->arguments ? (unsigned)expr->arguments->length : 0;
	                if (got != (unsigned)argc) {
	                    compilerErrorAtToken(
	                        compiler,
	                        &get->name,
	                        "argument count mismatch for call '%.*s': expected %d, got %u",
	                        get->name.length,
	                        get->name.start,
	                        argc,
	                        got
	                    );
	                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
	                    return NULL;
	                }

	                LLVMTypeRef retTy = llvmReturnTypeForTraitMethod(compiler, m);
	                LLVMTypeRef* pts = malloc(sizeof(LLVMTypeRef) * (size_t)(1 + argc));
	                pts[0] = i8ptr;
	                for (int i = 0; i < argc; i++) {
	                    Parameter* p = (Parameter*)listGet(m->params, i);
	                    pts[1 + i] = llvmParamTypeForTraitMethod(compiler, p);
	                }
	                LLVMTypeRef fnTy = LLVMFunctionType(retTy, pts, (unsigned)(1 + argc), 0);

	                LLVMValueRef fn = LLVMBuildBitCast(compiler->builder, fnRaw, LLVMPointerType(fnTy, 0), "vt_fn");

	                LLVMValueRef* args = malloc(sizeof(LLVMValueRef) * (size_t)(1 + argc));
	                args[0] = data;
	                for (int i = 0; i < argc; i++) {
	                    Expr* argAst = (Expr*)listGet(expr->arguments, i);
	                    LLVMValueRef av = compileCallArgForParam(compiler, argAst, pts[1 + i]);
	                    if (!av) {
	                        free(pts);
	                        free(args);
	                        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
	                        return NULL;
	                    }
	                    args[1 + i] = av;
	                }
	                free(pts);

	                LLVMValueRef call = LLVMBuildCall2(compiler->builder, fnTy, fn, args, (unsigned)(1 + argc), callInstNameForFnType(fnTy));
	                free(args);
	                if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
	                LLVMValueRef out = collapseMultiReturnByTypeIfNeeded(compiler, call, retTy);
	                if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
	                return out;
	            }
	        }

		        // Built-in methods on `Ref<map>` / `Ref<T[]>`:
		        // When receiver is a reference to a map/array handle, load the handle and call the same runtime APIs.
		        if (recvVar.value && recvVar.pointeeType == compilerGetMapType(compiler)) {
	            LLVMTypeRef mapType = compilerGetMapType(compiler);
            LLVMValueRef refPtr = loadLocalValue(compiler, recvVar); // tua_map**
            if (!refPtr) {
                if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                return NULL;
            }
            LLVMValueRef mapPtr = LLVMBuildLoad2(compiler->builder, mapType, refPtr, "mref");
            unsigned got = expr->arguments ? (unsigned)expr->arguments->length : 0;

            switch (mapMethodId(&get->name)) {
                case MAP_M_LEN: {
                    if (got != 0) {
                        emitDebug("map.len expects 0 arguments\n");
                        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                        return NULL;
                    }
                    LLVMValueRef fn = getOrCreateTuaMapLen(compiler);
                    LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
                    LLVMValueRef args1[1] = { mapPtr };
                    LLVMValueRef out = LLVMBuildCall2(compiler->builder, fnType, fn, args1, 1, "mlen");
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return out;
                }
                case MAP_M_HASKEY: {
                    if (got != 1) {
                        emitDebug("map.hasKey expects 1 argument\n");
                        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                        return NULL;
                    }
                    Expr* keyAst = (Expr*)expr->arguments->head->data;
                    LLVMValueRef keyExpr = compileExpr(compiler, keyAst);
                    LLVMValueRef key = tuaValueFromKey(compiler, keyExpr);
                    if (!key) {
                        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                        return NULL;
                    }
                    LLVMValueRef fn = getOrCreateTuaMapHas(compiler);
                    LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
                    LLVMValueRef args2[2] = { mapPtr, key };
                    LLVMValueRef ok32 = LLVMBuildCall2(compiler->builder, fnType, fn, args2, 2, "mhas");
                    LLVMValueRef ok = LLVMBuildTrunc(compiler->builder, ok32, LLVMInt1TypeInContext(compiler->context), "ok");
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return ok;
                }
                case MAP_M_DELETE: {
                    if (got != 1) {
                        emitDebug("map.delete expects 1 argument\n");
                        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                        return NULL;
                    }
                    Expr* keyAst = (Expr*)expr->arguments->head->data;
                    LLVMValueRef keyExpr = compileExpr(compiler, keyAst);
                    LLVMValueRef key = tuaValueFromKey(compiler, keyExpr);
                    if (!key) {
                        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                        return NULL;
                    }
                    LLVMValueRef fn = getOrCreateTuaMapDelete(compiler);
                    LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
                    LLVMValueRef args2[2] = { mapPtr, key };
                    LLVMValueRef ok32 = LLVMBuildCall2(compiler->builder, fnType, fn, args2, 2, "mdel32");
                    LLVMValueRef ok = LLVMBuildTrunc(compiler->builder, ok32, LLVMInt1TypeInContext(compiler->context), "mdel");
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return ok;
                }
                case MAP_M_CLEAR: {
                    if (got != 0) {
                        emitDebug("map.clear expects 0 arguments\n");
                        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                        return NULL;
                    }
                    LLVMValueRef fn = getOrCreateTuaMapClear(compiler);
                    LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
                    LLVMValueRef args1[1] = { mapPtr };
                    LLVMBuildCall2(compiler->builder, fnType, fn, args1, 1, "");
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return LLVMConstInt(LLVMInt32TypeInContext(compiler->context), 0, 0);
                }
                default:
                    break;
            }
        }

        if (recvVar.value && recvVar.pointeeType == compilerGetArrayType(compiler)) {
            LLVMTypeRef arrType = compilerGetArrayType(compiler);
            LLVMValueRef refPtr = loadLocalValue(compiler, recvVar); // tua_array**
            if (!refPtr) {
                if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                return NULL;
            }
            LLVMValueRef arrPtr = LLVMBuildLoad2(compiler->builder, arrType, refPtr, "aref");
            unsigned got = expr->arguments ? (unsigned)expr->arguments->length : 0;
            if (arrayMethodId(&get->name) == ARRAY_M_LEN) {
                if (got != 0) {
                    emitDebug("array.len expects 0 arguments\n");
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return NULL;
                }
                LLVMContextRef context = compiler->context;
                LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
                LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
                LLVMTypeRef arrStruct = LLVMGetTypeByName2(context, "tua_array");
                if (!arrStruct) {
                    compilerErrorAt(compiler, get->name.line, "missing tua_array type for array.len");
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return NULL;
                }
                LLVMValueRef lenPtr = LLVMBuildStructGEP2(compiler->builder, arrStruct, arrPtr, 0, "alenp");
                LLVMValueRef len64 = LLVMBuildLoad2(compiler->builder, i64, lenPtr, "alen64");
                LLVMValueRef out = LLVMBuildTrunc(compiler->builder, len64, i32, "alen");
                if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                return out;
            }
        }

	        // Array built-in methods: `a.len()`, `a.clone()`, `a.push(v)`, `a.slice(off,n)`
	        if (recvVar.value && recvVar.isArray) {
	            LLVMTypeRef arrType = compilerGetArrayType(compiler);
	            LLVMTypeRef arrStruct = LLVMGetTypeByName2(compiler->context, "tua_array");
	            LLVMValueRef arrPtr = NULL;
            if (recvVar.isBoxed) {
                if (!recvVar.boxPtrType) {
                    emitDebug("Missing boxed pointer type for array receiver\n");
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return NULL;
                }
                LLVMValueRef cell = LLVMBuildLoad2(compiler->builder, recvVar.boxPtrType, recvVar.value, "cell");
                arrPtr = LLVMBuildLoad2(compiler->builder, arrType, cell, "aval");
	            } else {
	                arrPtr = LLVMBuildLoad2(compiler->builder, arrType, recvVar.value, "aval");
	            }

	            int saw = 0;
	            LLVMValueRef implOut = tryEmitBuiltinValueImplMethodCall(
	                compiler,
	                "array",
	                5,
	                get->object,
	                arrPtr,
	                get,
	                expr,
	                wantMultiForThisCall,
	                &saw
	            );
	            if (implOut) return implOut;

	            unsigned got = expr->arguments ? (unsigned)expr->arguments->length : 0;
	            LLVMTypeRef i64 = LLVMInt64TypeInContext(compiler->context);
	            LLVMTypeRef i32 = LLVMInt32TypeInContext(compiler->context);

            ArrayMethodId mid = arrayMethodId(&get->name);
            if (mid == ARRAY_M_LEN) {
                if (got != 0) {
                    emitDebug("array.len expects 0 arguments\n");
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return NULL;
                }
                if (recvVar.isStackArray && recvVar.arrayFixedLen >= 0) {
                    LLVMValueRef out = LLVMConstInt(i32, (uint64_t)recvVar.arrayFixedLen, 0);
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return out;
                }
                LLVMValueRef lenPtr = LLVMBuildStructGEP2(compiler->builder, arrStruct, arrPtr, 0, "alenp");
                LLVMValueRef len64 = LLVMBuildLoad2(compiler->builder, i64, lenPtr, "alen64");
                LLVMValueRef out = LLVMBuildTrunc(compiler->builder, len64, i32, "alen");
                if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                return out;
            }

            if (mid == ARRAY_M_CLONE) {
                if (got != 0) {
                    emitDebug("array.clone expects 0 arguments\n");
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return NULL;
                }
                LLVMValueRef fn = getOrCreateTuaArrayClone(compiler);
                LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
                LLVMValueRef args1[1] = { arrPtr };
                LLVMValueRef out = LLVMBuildCall2(compiler->builder, fnType, fn, args1, 1, "aclone");
                if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                return out;
            }

            if (mid == ARRAY_M_PUSH) {
                if (got != 1) {
                    compilerErrorAt(compiler, get->name.line, "array.push expects 1 argument");
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return NULL;
                }
                if (recvVar.arrayFixedLen >= 0) {
                    compilerErrorAt(compiler, get->name.line, "cannot push to fixed-length array");
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return NULL;
                }
                LLVMTypeRef elemTy = recvVar.arrayElemType;
                if (!elemTy) {
                    compilerErrorAt(compiler, get->name.line, "missing array element type metadata");
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return NULL;
                }
                LLVMValueRef arg0 = compileExpr(compiler, (Expr*)expr->arguments->head->data);
                arg0 = castValueToType(compiler, arg0, elemTy);
                LLVMValueRef tmp = LLVMBuildAlloca(compiler->builder, elemTy, "push_tmp");
                LLVMBuildStore(compiler->builder, arg0, tmp);
                LLVMValueRef p = LLVMBuildBitCast(compiler->builder, tmp, LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0), "push_p");

                LLVMValueRef fn = getOrCreateTuaArrayPush(compiler);
                LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
                LLVMValueRef args2[2] = { arrPtr, p };
                LLVMValueRef len64 = LLVMBuildCall2(compiler->builder, fnType, fn, args2, 2, "plen64");
                LLVMValueRef out = LLVMBuildTrunc(compiler->builder, len64, i32, "plen");
                if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                return out;
            }

            if (mid == ARRAY_M_SLICE) {
                if (got != 2) {
                    compilerErrorAt(compiler, get->name.line, "array.slice expects 2 arguments");
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return NULL;
                }
                LLVMTypeRef elemTy = recvVar.arrayElemType;
                if (!elemTy) {
                    compilerErrorAt(compiler, get->name.line, "missing array element type metadata");
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return NULL;
                }
                LLVMContextRef context = compiler->context;
                LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
                LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(context), 0);

                LLVMValueRef off = compileExpr(compiler, (Expr*)expr->arguments->head->data);
                LLVMValueRef n = compileExpr(compiler, (Expr*)expr->arguments->head->next->data);
                if (!off || !n) {
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return NULL;
                }
                off = castValueToType(compiler, off, i64);
                n = castValueToType(compiler, n, i64);

                // Determine base pointer and length.
                LLVMValueRef len64 = NULL;
                LLVMValueRef basePtr = NULL; // elemTy*
                if (compiler->stackFixedArrays && recvVar.isStackArray && recvVar.stackArrayData && recvVar.arrayFixedLen >= 0) {
                    len64 = LLVMConstInt(i64, (uint64_t)recvVar.arrayFixedLen, 1);
                    basePtr = recvVar.stackArrayData;
                } else {
                    // null check
                    LLVMValueRef isNull = LLVMBuildICmp(compiler->builder, LLVMIntEQ, arrPtr, LLVMConstNull(arrType), "asnull");
                    LLVMValueRef fn = compiler->current->func;
                    LLVMBasicBlockRef okBB = LLVMAppendBasicBlock(fn, "as.ok");
                    LLVMBasicBlockRef badBB = LLVMAppendBasicBlock(fn, "as.null");
                    LLVMBuildCondBr(compiler->builder, isNull, badBB, okBB);

                    LLVMPositionBuilderAtEnd(compiler->builder, badBB);
                    LLVMValueRef panicFn = getOrCreateTuaPanic(compiler);
                    LLVMTypeRef pty = LLVMGlobalGetValueType(panicFn);
                    LLVMValueRef msg = LLVMBuildGlobalStringPtr(compiler->builder, "null array", "asmsg");
                    LLVMBuildCall2(compiler->builder, pty, panicFn, &msg, 1, "");
                    LLVMBuildUnreachable(compiler->builder);

                    LLVMPositionBuilderAtEnd(compiler->builder, okBB);
                    LLVMValueRef lenPtr = LLVMBuildStructGEP2(compiler->builder, arrStruct, arrPtr, 0, "aslenp");
                    len64 = LLVMBuildLoad2(compiler->builder, i64, lenPtr, "aslen");
                    LLVMValueRef dataPtrPtr = LLVMBuildStructGEP2(compiler->builder, arrStruct, arrPtr, 2, "asdatap");
                    LLVMValueRef dataI8 = LLVMBuildLoad2(compiler->builder, i8ptr, dataPtrPtr, "asdata");
                    basePtr = LLVMBuildBitCast(compiler->builder, dataI8, LLVMPointerType(elemTy, 0), "asbase");
                }

                // Bounds: off>=0, n>=0, off+n <= len
                LLVMValueRef zero = LLVMConstInt(i64, 0, 0);
                LLVMValueRef offNeg = LLVMBuildICmp(compiler->builder, LLVMIntSLT, off, zero, "asoffneg");
                LLVMValueRef nNeg = LLVMBuildICmp(compiler->builder, LLVMIntSLT, n, zero, "asnneg");
                LLVMValueRef sum = LLVMBuildAdd(compiler->builder, off, n, "assum");
                LLVMValueRef over = LLVMBuildICmp(compiler->builder, LLVMIntSGT, sum, len64, "asover");
                LLVMValueRef bad0 = LLVMBuildOr(compiler->builder, offNeg, nNeg, "asbad0");
                LLVMValueRef bad = LLVMBuildOr(compiler->builder, bad0, over, "asbad");

                LLVMValueRef fn2 = compiler->current->func;
                LLVMBasicBlockRef inBB = LLVMAppendBasicBlock(fn2, "as.in");
                LLVMBasicBlockRef oobBB = LLVMAppendBasicBlock(fn2, "as.oob");
                LLVMBuildCondBr(compiler->builder, bad, oobBB, inBB);

                LLVMPositionBuilderAtEnd(compiler->builder, oobBB);
                LLVMValueRef panicFn2 = getOrCreateTuaPanic(compiler);
                LLVMTypeRef pty2 = LLVMGlobalGetValueType(panicFn2);
                LLVMValueRef msg2 = LLVMBuildGlobalStringPtr(compiler->builder, "array.slice out of bounds", "asmsg2");
                LLVMBuildCall2(compiler->builder, pty2, panicFn2, &msg2, 1, "");
                LLVMBuildUnreachable(compiler->builder);

                LLVMPositionBuilderAtEnd(compiler->builder, inBB);
                LLVMValueRef dataOff = LLVMBuildInBoundsGEP2(compiler->builder, elemTy, basePtr, &off, 1, "asdataoff");
                LLVMTypeRef sliceTy = compilerGetSliceType(compiler, elemTy);
                LLVMValueRef s = LLVMGetUndef(sliceTy);
                s = LLVMBuildInsertValue(compiler->builder, s, dataOff, 0, "s0");
                s = LLVMBuildInsertValue(compiler->builder, s, n, 1, "s1");
                if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                return s;
            }

            compilerErrorAt(compiler, get->name.line, "unknown array method: %.*s", get->name.length, get->name.start);
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }

        // Bytes built-in methods:
        // - `b.len() -> long`
        // - `b.get(i: long) -> byte` (panics on invalid)
        // - `b.getU8(i: long) -> byte` (alias of get)
        // - `b.set(i: long, v: int) -> int` (returns err code)
        // - `b.setU8(i: long, v: int) -> int` (alias of set)
        // - `b.isReadonly() -> int`
        // - `b.copy(dstOff: long, src: bytes, srcOff: long, n: long) -> int`
        // - `b.slice(off: long, n: long) -> Slice<byte>` (panics on invalid)
        if (recvVar.value && recvVar.isBytes) {
            LLVMValueRef implOut = tryEmitBuiltinHandleImplMethodCall(compiler, recvVar, get, expr, wantMultiForThisCall);
            if (implOut) return implOut;

            LLVMTypeRef bytesTy = compilerGetBytesType(compiler);
            LLVMValueRef bytesPtr = NULL;
            if (recvVar.isBoxed) {
                if (!recvVar.boxPtrType) {
                    emitDebug("Missing boxed pointer type for bytes receiver\n");
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return NULL;
                }
                LLVMValueRef cell = LLVMBuildLoad2(compiler->builder, recvVar.boxPtrType, recvVar.value, "cell");
                bytesPtr = LLVMBuildLoad2(compiler->builder, bytesTy, cell, "bval");
            } else {
                bytesPtr = LLVMBuildLoad2(compiler->builder, bytesTy, recvVar.value, "bval");
            }

            unsigned got = expr->arguments ? (unsigned)expr->arguments->length : 0;
            LLVMTypeRef i64 = LLVMInt64TypeInContext(compiler->context);
            LLVMTypeRef i32 = LLVMInt32TypeInContext(compiler->context);
            LLVMTypeRef i8 = LLVMInt8TypeInContext(compiler->context);

            BytesMethodId mid = bytesMethodId(&get->name);
            if (mid == BYTES_M_LEN) {
                if (got != 0) {
                    emitDebug("bytes.len expects 0 arguments\n");
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return NULL;
                }
                LLVMValueRef fn = getOrCreateTuaBytesLen(compiler);
                LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
                LLVMValueRef args1[1] = { bytesPtr };
                LLVMValueRef out = LLVMBuildCall2(compiler->builder, fnType, fn, args1, 1, "blen");
                if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                return out;
            }

            if (mid == BYTES_M_GET) {
                if (got != 1) {
                    emitDebug("bytes.get/getU8 expects 1 argument\n");
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return NULL;
                }
                LLVMValueRef idx = compileExpr(compiler, (Expr*)expr->arguments->head->data);
                if (!idx) {
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return NULL;
                }
                idx = castValueToType(compiler, idx, i64);

                // Unchecked mode: inline load (UB on null/oob).
                if (compilerUncheckedIndex(compiler)) {
                    LLVMTypeRef bytesStruct = LLVMGetTypeByName2(compiler->context, "tua_bytes");
                    if (!bytesStruct) bytesStruct = LLVMGetElementType(bytesTy);
                    LLVMTypeRef i8ptr = LLVMPointerType(i8, 0);
                    LLVMValueRef dataPtrPtr = LLVMBuildStructGEP2(compiler->builder, bytesStruct, bytesPtr, 2, "bdatap");
                    LLVMValueRef data = LLVMBuildLoad2(compiler->builder, i8ptr, dataPtrPtr, "bdata");
                    LLVMValueRef ep = LLVMBuildGEP2(compiler->builder, i8, data, &idx, 1, "ep");
                    LLVMValueRef v8 = LLVMBuildLoad2(compiler->builder, i8, ep, "bv");
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return v8;
                }

                LLVMValueRef outSlot = LLVMBuildAlloca(compiler->builder, i32, "bout");
                LLVMValueRef fn = getOrCreateTuaBytesGetU8(compiler);
                LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
                LLVMValueRef args3[3] = { bytesPtr, idx, outSlot };
                LLVMValueRef err = LLVMBuildCall2(compiler->builder, fnType, fn, args3, 3, "berr");

                LLVMValueRef ok = LLVMBuildICmp(compiler->builder, LLVMIntEQ, err, LLVMConstInt(i32, 0, 0), "bok");
                LLVMBasicBlockRef thenB = LLVMAppendBasicBlock(compiler->current->func, "bget_ok");
                LLVMBasicBlockRef elseB = LLVMAppendBasicBlock(compiler->current->func, "bget_bad");
                LLVMBasicBlockRef contB = LLVMAppendBasicBlock(compiler->current->func, "bget_cont");
                LLVMBuildCondBr(compiler->builder, ok, thenB, elseB);

                LLVMPositionBuilderAtEnd(compiler->builder, elseB);
                LLVMValueRef panicFn = getOrCreateTuaPanic(compiler);
                LLVMTypeRef pty = LLVMGlobalGetValueType(panicFn);
                LLVMValueRef msg = LLVMBuildGlobalStringPtr(compiler->builder, "bytes.get out of bounds or null", "bgetmsg");
                LLVMBuildCall2(compiler->builder, pty, panicFn, &msg, 1, "");
                LLVMBuildUnreachable(compiler->builder);

                LLVMPositionBuilderAtEnd(compiler->builder, thenB);
                LLVMValueRef out32 = LLVMBuildLoad2(compiler->builder, i32, outSlot, "out32");
                LLVMValueRef out8 = LLVMBuildTrunc(compiler->builder, out32, i8, "out8");
                LLVMBuildBr(compiler->builder, contB);

                LLVMPositionBuilderAtEnd(compiler->builder, contB);
                LLVMValueRef phi = LLVMBuildPhi(compiler->builder, i8, "bget");
                LLVMAddIncoming(phi, &out8, &thenB, 1);
                if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                return phi;
            }

            if (mid == BYTES_M_SET) {
                if (got != 2) {
                    emitDebug("bytes.set/setU8 expects 2 arguments\n");
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return NULL;
                }
                LLVMValueRef idx = compileExpr(compiler, (Expr*)expr->arguments->head->data);
                LLVMValueRef v0 = compileExpr(compiler, (Expr*)expr->arguments->head->next->data);
                if (!idx || !v0) {
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return NULL;
                }
                idx = castValueToType(compiler, idx, i64);
                v0 = castValueToType(compiler, v0, i32);
                LLVMValueRef fn = getOrCreateTuaBytesSetU8(compiler);
                LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
                LLVMValueRef args3[3] = { bytesPtr, idx, v0 };
                LLVMValueRef err = LLVMBuildCall2(compiler->builder, fnType, fn, args3, 3, "bset");
                if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                return err;
            }

            if (mid == BYTES_M_ISREADONLY) {
                if (got != 0) {
                    emitDebug("bytes.isReadonly expects 0 arguments\n");
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return NULL;
                }
                LLVMValueRef fn = getOrCreateTuaBytesIsReadonly(compiler);
                LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
                LLVMValueRef args1[1] = { bytesPtr };
                LLVMValueRef out = LLVMBuildCall2(compiler->builder, fnType, fn, args1, 1, "bro");
                if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                return out;
            }

            if (mid == BYTES_M_COPY) {
                if (got != 4) {
                    emitDebug("bytes.copy expects 4 arguments\n");
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return NULL;
                }
                LLVMValueRef dstOff = compileExpr(compiler, (Expr*)expr->arguments->head->data);
                LLVMValueRef src = compileExpr(compiler, (Expr*)expr->arguments->head->next->data);
                LLVMValueRef srcOff = compileExpr(compiler, (Expr*)expr->arguments->head->next->next->data);
                LLVMValueRef n = compileExpr(compiler, (Expr*)expr->arguments->head->next->next->next->data);
                if (!dstOff || !src || !srcOff || !n) {
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return NULL;
                }
                dstOff = castValueToType(compiler, dstOff, i64);
                src = castValueToType(compiler, src, bytesTy);
                srcOff = castValueToType(compiler, srcOff, i64);
                n = castValueToType(compiler, n, i64);
                LLVMValueRef fn = getOrCreateTuaBytesCopy(compiler);
                LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
                LLVMValueRef args5[5] = { bytesPtr, dstOff, src, srcOff, n };
                LLVMValueRef out = LLVMBuildCall2(compiler->builder, fnType, fn, args5, 5, "bcopy");
                if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                return out;
            }

            if (mid == BYTES_M_SLICE) {
                if (got != 2) {
                    emitDebug("bytes.slice expects 2 arguments\n");
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return NULL;
                }
                LLVMValueRef off = compileExpr(compiler, (Expr*)expr->arguments->head->data);
                LLVMValueRef n = compileExpr(compiler, (Expr*)expr->arguments->head->next->data);
                if (!off || !n) {
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return NULL;
                }
                off = castValueToType(compiler, off, i64);
                n = castValueToType(compiler, n, i64);

                LLVMValueRef lenFn = getOrCreateTuaBytesLen(compiler);
                LLVMTypeRef lenTy = LLVMGlobalGetValueType(lenFn);
                LLVMValueRef lenArgs[1] = { bytesPtr };
                LLVMValueRef blen = LLVMBuildCall2(compiler->builder, lenTy, lenFn, lenArgs, 1, "blen2");

                LLVMValueRef zero = LLVMConstInt(i64, 0, 0);
                LLVMValueRef offNeg = LLVMBuildICmp(compiler->builder, LLVMIntSLT, off, zero, "offneg");
                LLVMValueRef nNeg = LLVMBuildICmp(compiler->builder, LLVMIntSLT, n, zero, "nneg");
                LLVMValueRef sum = LLVMBuildAdd(compiler->builder, off, n, "sum");
                LLVMValueRef oob = LLVMBuildICmp(compiler->builder, LLVMIntSGT, sum, blen, "oob");
                LLVMValueRef bad1 = LLVMBuildOr(compiler->builder, offNeg, nNeg, "bad1");
                LLVMValueRef bad = LLVMBuildOr(compiler->builder, bad1, oob, "bad");

                LLVMBasicBlockRef okB = LLVMAppendBasicBlock(compiler->current->func, "bs_ok");
                LLVMBasicBlockRef badB = LLVMAppendBasicBlock(compiler->current->func, "bs_bad");
                LLVMBasicBlockRef contB = LLVMAppendBasicBlock(compiler->current->func, "bs_cont");
                LLVMBuildCondBr(compiler->builder, bad, badB, okB);

                LLVMPositionBuilderAtEnd(compiler->builder, badB);
                LLVMValueRef panicFn = getOrCreateTuaPanic(compiler);
                LLVMTypeRef pty = LLVMGlobalGetValueType(panicFn);
                LLVMValueRef msg = LLVMBuildGlobalStringPtr(compiler->builder, "bytes.slice out of bounds or null", "bsmsg");
                LLVMBuildCall2(compiler->builder, pty, panicFn, &msg, 1, "");
                LLVMBuildUnreachable(compiler->builder);

                LLVMPositionBuilderAtEnd(compiler->builder, okB);
                LLVMValueRef dataFn = getOrCreateTuaBytesData(compiler);
                LLVMTypeRef dataTy = LLVMGlobalGetValueType(dataFn);
                LLVMValueRef dataArgs[1] = { bytesPtr };
                LLVMValueRef data = LLVMBuildCall2(compiler->builder, dataTy, dataFn, dataArgs, 1, "bdata");
                LLVMValueRef dataOff = LLVMBuildGEP2(compiler->builder, i8, data, &off, 1, "bdata_off");

                LLVMTypeRef sliceTy = compilerGetSliceType(compiler, i8);
                LLVMValueRef s = LLVMGetUndef(sliceTy);
                s = LLVMBuildInsertValue(compiler->builder, s, dataOff, 0, "s0");
                s = LLVMBuildInsertValue(compiler->builder, s, n, 1, "s1");
                LLVMBuildBr(compiler->builder, contB);

                LLVMPositionBuilderAtEnd(compiler->builder, contB);
                LLVMValueRef phi = LLVMBuildPhi(compiler->builder, sliceTy, "slice");
                LLVMAddIncoming(phi, &s, &okB, 1);
                if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                return phi;
            }

            compilerErrorAt(compiler, get->name.line, "unknown bytes method: %.*s", get->name.length, get->name.start);
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }

        // Slice<T> built-in methods:
        // - `s.len() -> long`
        // - `s.get(i: long) -> T` (panics on invalid)
        // - `s.set(i: long, v: T) -> int` (requires writable binding; scalar elements only in v1)
        if (recvVar.value && recvVar.isSlice && recvVar.sliceElemType) {
            LLVMValueRef sliceVal = loadLocalValue(compiler, recvVar);
            if (!sliceVal) {
                if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                return NULL;
            }
            int saw = 0;
            LLVMValueRef implOut = tryEmitBuiltinValueImplMethodCall(
                compiler,
                "Slice",
                5,
                get->object,
                sliceVal,
                get,
                expr,
                wantMultiForThisCall,
                &saw
            );
            if (implOut) return implOut;
            unsigned got = expr->arguments ? (unsigned)expr->arguments->length : 0;
            LLVMTypeRef i64 = LLVMInt64TypeInContext(compiler->context);
            LLVMTypeRef elemTy = recvVar.sliceElemType;

            LLVMValueRef data = LLVMBuildExtractValue(compiler->builder, sliceVal, 0, "sdata");
            LLVMValueRef len = LLVMBuildExtractValue(compiler->builder, sliceVal, 1, "slen");

            if (tokenEquals(&get->name, "len")) {
                if (got != 0) {
                    emitDebug("Slice.len expects 0 arguments\n");
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return NULL;
                }
                if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                return len;
            }

            if (tokenEquals(&get->name, "get")) {
                if (got != 1) {
                    emitDebug("Slice.get expects 1 argument\n");
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return NULL;
                }
                LLVMValueRef idx = compileExpr(compiler, (Expr*)expr->arguments->head->data);
                if (!idx) {
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return NULL;
                }
                idx = castValueToType(compiler, idx, i64);

                // Unchecked mode: no bounds checks (UB on invalid access).
                if (compilerUncheckedIndex(compiler)) {
                    LLVMTypeRef elemPtrTy = LLVMPointerType(elemTy, 0);
                    LLVMValueRef typed = castValueToType(compiler, data, elemPtrTy);
                    LLVMValueRef ptr = LLVMBuildGEP2(compiler->builder, elemTy, typed, &idx, 1, "ep");
                    LLVMValueRef val = LLVMBuildLoad2(compiler->builder, elemTy, ptr, "sv");
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return val;
                }

                LLVMValueRef zero = LLVMConstInt(i64, 0, 0);
                LLVMValueRef neg = LLVMBuildICmp(compiler->builder, LLVMIntSLT, idx, zero, "idxneg");
                LLVMValueRef ge = LLVMBuildICmp(compiler->builder, LLVMIntSGE, idx, len, "idxge");
                LLVMValueRef bad = LLVMBuildOr(compiler->builder, neg, ge, "bad");

                LLVMBasicBlockRef okB = LLVMAppendBasicBlock(compiler->current->func, "sg_ok");
                LLVMBasicBlockRef badB = LLVMAppendBasicBlock(compiler->current->func, "sg_bad");
                LLVMBasicBlockRef contB = LLVMAppendBasicBlock(compiler->current->func, "sg_cont");
                LLVMBuildCondBr(compiler->builder, bad, badB, okB);

                LLVMPositionBuilderAtEnd(compiler->builder, badB);
                LLVMValueRef panicFn = getOrCreateTuaPanic(compiler);
                LLVMTypeRef pty = LLVMGlobalGetValueType(panicFn);
                LLVMValueRef msg = LLVMBuildGlobalStringPtr(compiler->builder, "Slice.get out of bounds", "sgmsg");
                LLVMBuildCall2(compiler->builder, pty, panicFn, &msg, 1, "");
                LLVMBuildUnreachable(compiler->builder);

                LLVMPositionBuilderAtEnd(compiler->builder, okB);
                LLVMTypeRef elemPtrTy = LLVMPointerType(elemTy, 0);
                LLVMValueRef typed = castValueToType(compiler, data, elemPtrTy);
                LLVMValueRef ptr = LLVMBuildGEP2(compiler->builder, elemTy, typed, &idx, 1, "ep");
                LLVMValueRef val = LLVMBuildLoad2(compiler->builder, elemTy, ptr, "sv");
                LLVMBuildBr(compiler->builder, contB);

                LLVMPositionBuilderAtEnd(compiler->builder, contB);
                LLVMValueRef phi = LLVMBuildPhi(compiler->builder, elemTy, "sget");
                LLVMAddIncoming(phi, &val, &okB, 1);
                if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                return phi;
            }

            if (tokenEquals(&get->name, "set")) {
                if (got != 2) {
                    compilerErrorAt(compiler, get->name.line, "Slice.set expects 2 arguments");
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return NULL;
                }
                if (recvVar.isConst) {
                    compilerErrorAt(compiler, get->name.line, "cannot call Slice.set on const binding");
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return NULL;
                }
                if (!typeKindIsScalarValueKind(recvVar.sliceElemKind)) {
                    compilerErrorAt(compiler, get->name.line, "Slice.set is only supported for scalar element types in v1");
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return NULL;
                }

                LLVMValueRef idx = compileExpr(compiler, (Expr*)expr->arguments->head->data);
                LLVMValueRef vv = compileExpr(compiler, (Expr*)expr->arguments->head->next->data);
                if (!idx || !vv) {
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return NULL;
                }
                idx = castValueToType(compiler, idx, i64);
                vv = castValueToType(compiler, vv, elemTy);

                LLVMValueRef zero = LLVMConstInt(i64, 0, 0);
                LLVMValueRef neg = LLVMBuildICmp(compiler->builder, LLVMIntSLT, idx, zero, "idxneg");
                LLVMValueRef ge = LLVMBuildICmp(compiler->builder, LLVMIntSGE, idx, len, "idxge");
                LLVMValueRef bad = LLVMBuildOr(compiler->builder, neg, ge, "bad");

                LLVMValueRef fn = compiler->current->func;
                LLVMBasicBlockRef okB = LLVMAppendBasicBlock(fn, "ss_ok");
                LLVMBasicBlockRef badB = LLVMAppendBasicBlock(fn, "ss_bad");
                LLVMBasicBlockRef contB = LLVMAppendBasicBlock(fn, "ss_cont");
                LLVMBuildCondBr(compiler->builder, bad, badB, okB);

                LLVMPositionBuilderAtEnd(compiler->builder, badB);
                LLVMValueRef panicFn = getOrCreateTuaPanic(compiler);
                LLVMTypeRef pty = LLVMGlobalGetValueType(panicFn);
                LLVMValueRef msg = LLVMBuildGlobalStringPtr(compiler->builder, "Slice.set out of bounds", "ssmsg");
                LLVMBuildCall2(compiler->builder, pty, panicFn, &msg, 1, "");
                LLVMBuildUnreachable(compiler->builder);

                LLVMPositionBuilderAtEnd(compiler->builder, okB);
                LLVMTypeRef i32 = LLVMInt32TypeInContext(compiler->context);
                LLVMValueRef err32 = LLVMConstInt(i32, 0, 0);

                if (recvVar.sliceOwnerIsBytes && recvVar.sliceOwnerName && recvVar.sliceOwnerNameLength > 0 &&
                    recvVar.sliceElemKind == TYPE_BYTE) {
                    VariableExpr ve;
                    memset(&ve, 0, sizeof(ve));
                    ve.base.type = EXPR_VARIABLE;
                    ve.name = (Token){TOKEN_IDENTIFIER, recvVar.sliceOwnerName, recvVar.sliceOwnerNameLength, get->name.line, get->name.col, 0};
                    VariableRef owner = findVariableExpr(compiler, (Expr*)&ve);
                    if (owner.value && owner.isBytes) {
                        LLVMTypeRef bytesTy = compilerGetBytesType(compiler);
                        LLVMValueRef bytesPtr = NULL;
                        if (owner.isBoxed) {
                            if (!owner.boxPtrType) {
                                compilerErrorAt(compiler, get->name.line, "missing boxed pointer type metadata");
                                if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                                return NULL;
                            }
                            LLVMValueRef cell = LLVMBuildLoad2(compiler->builder, owner.boxPtrType, owner.value, "cell");
                            bytesPtr = LLVMBuildLoad2(compiler->builder, bytesTy, cell, "bval");
                        } else {
                            bytesPtr = LLVMBuildLoad2(compiler->builder, bytesTy, owner.value, "bval");
                        }

                        LLVMValueRef dataFn = getOrCreateTuaBytesData(compiler);
                        LLVMTypeRef dataTy = LLVMGlobalGetValueType(dataFn);
                        LLVMValueRef dataArgs[1] = { bytesPtr };
                        LLVMValueRef baseI8 = LLVMBuildCall2(compiler->builder, dataTy, dataFn, dataArgs, 1, "bdata");

                        LLVMValueRef sliceI8 = LLVMBuildBitCast(compiler->builder, data, LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0), "sdata8");
                        LLVMValueRef baseI8i = LLVMBuildPtrToInt(compiler->builder, baseI8, i64, "bpi");
                        LLVMValueRef sliceI8i = LLVMBuildPtrToInt(compiler->builder, sliceI8, i64, "spi");
                        LLVMValueRef diff = LLVMBuildSub(compiler->builder, sliceI8i, baseI8i, "boff");
                        LLVMValueRef absIdx = LLVMBuildAdd(compiler->builder, diff, idx, "absidx");

                        LLVMValueRef setFn = getOrCreateTuaBytesSetU8(compiler);
                        LLVMTypeRef setTy = LLVMGlobalGetValueType(setFn);
                        LLVMValueRef vv32 = LLVMBuildZExt(compiler->builder, vv, i32, "vv32");
                        LLVMValueRef args3[3] = { bytesPtr, absIdx, vv32 };
                        err32 = LLVMBuildCall2(compiler->builder, setTy, setFn, args3, 3, "setu8");
                    }
                } else {
                    LLVMTypeRef elemPtrTy = LLVMPointerType(elemTy, 0);
                    LLVMValueRef typed = castValueToType(compiler, data, elemPtrTy);
                    LLVMValueRef ptr = LLVMBuildGEP2(compiler->builder, elemTy, typed, &idx, 1, "ep");
                    LLVMBuildStore(compiler->builder, vv, ptr);
                }

                LLVMBuildBr(compiler->builder, contB);

                LLVMPositionBuilderAtEnd(compiler->builder, contB);
                LLVMValueRef phi = LLVMBuildPhi(compiler->builder, i32, "serr");
                LLVMAddIncoming(phi, &err32, &okB, 1);
                if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                return phi;
            }
        }

        // Map built-in methods: `m.hasKey(k)`, `m.len()`
        if (recvVar.value && recvVar.isMap) {
            LLVMValueRef implOut = tryEmitBuiltinHandleImplMethodCall(compiler, recvVar, get, expr, wantMultiForThisCall);
            if (implOut) return implOut;

            LLVMTypeRef mapType = compilerGetMapType(compiler);
            LLVMValueRef mapPtr = NULL;
            if (recvVar.isBoxed) {
                if (!recvVar.boxPtrType) {
                    emitDebug("Missing boxed pointer type for map receiver\n");
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return NULL;
                }
                LLVMValueRef cell = LLVMBuildLoad2(compiler->builder, recvVar.boxPtrType, recvVar.value, "cell");
                mapPtr = LLVMBuildLoad2(compiler->builder, mapType, cell, "mval");
            } else {
                mapPtr = LLVMBuildLoad2(compiler->builder, mapType, recvVar.value, "mval");
            }

            unsigned got = expr->arguments ? (unsigned)expr->arguments->length : 0;

            MapMethodId mid = mapMethodId(&get->name);
            if (mid == MAP_M_LEN) {
                if (got != 0) {
                    emitDebug("map.len expects 0 arguments\n");
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return NULL;
                }
                LLVMValueRef fn = getOrCreateTuaMapLen(compiler);
                LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
                LLVMValueRef args1[1] = { mapPtr };
                LLVMValueRef out = LLVMBuildCall2(compiler->builder, fnType, fn, args1, 1, "mlen");
                if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                return out;
            }

            if (mid == MAP_M_HASKEY) {
                if (got != 1) {
                    emitDebug("map.hasKey expects 1 argument\n");
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return NULL;
                }
                Expr* keyAst = (Expr*)expr->arguments->head->data;
                LLVMValueRef keyExpr = compileExpr(compiler, keyAst);
                if (recvVar.isTypedMap && recvVar.mapKeyType) {
                    if (!typedMapKeyCompatible(compiler, recvVar.mapKeyType, keyExpr)) {
                        compilerErrorAt(compiler, keyAst ? keyAst->token.line : get->name.line, "typed map key type mismatch");
                        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                        return NULL;
                    }
                }
                LLVMValueRef key = tuaValueFromKey(compiler, keyExpr);
                if (!key) {
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return NULL;
                }
                LLVMValueRef fn = getOrCreateTuaMapHas(compiler);
                LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
                LLVMValueRef args2[2] = { mapPtr, key };
                LLVMValueRef ok32 = LLVMBuildCall2(compiler->builder, fnType, fn, args2, 2, "mhas");
                LLVMValueRef ok = LLVMBuildTrunc(compiler->builder, ok32, LLVMInt1TypeInContext(compiler->context), "ok");
                if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                return ok;
            }

            if (mid == MAP_M_DELETE) {
                if (got != 1) {
                    emitDebug("map.delete expects 1 argument\n");
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return NULL;
                }
                Expr* keyAst = (Expr*)expr->arguments->head->data;
                LLVMValueRef keyExpr = compileExpr(compiler, keyAst);
                if (recvVar.isTypedMap && recvVar.mapKeyType) {
                    if (!typedMapKeyCompatible(compiler, recvVar.mapKeyType, keyExpr)) {
                        compilerErrorAt(compiler, keyAst ? keyAst->token.line : get->name.line, "typed map key type mismatch");
                        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                        return NULL;
                    }
                }
                LLVMValueRef key = tuaValueFromKey(compiler, keyExpr);
                if (!key) {
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return NULL;
                }
                LLVMValueRef fn = getOrCreateTuaMapDelete(compiler);
                LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
                LLVMValueRef args2[2] = { mapPtr, key };
                LLVMValueRef ok32 = LLVMBuildCall2(compiler->builder, fnType, fn, args2, 2, "mdel32");
                LLVMValueRef ok = LLVMBuildTrunc(compiler->builder, ok32, LLVMInt1TypeInContext(compiler->context), "mdel");
                if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                return ok;
            }

            int isGetMut = (mid == MAP_M_GETMUT);
            if (mid == MAP_M_GET || mid == MAP_M_GETMUT) {
                if (got != 1) {
                    emitDebug("map.get/getMut expects 1 argument\n");
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return NULL;
                }
                if (isGetMut && recvVar.isConst) {
                    compilerErrorAt(compiler, get->name.line, "cannot take mutable element reference from const map");
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return NULL;
                }
                Expr* keyAst = (Expr*)expr->arguments->head->data;
                LLVMValueRef keyExpr = compileExpr(compiler, keyAst);
                if (recvVar.isTypedMap && recvVar.mapKeyType) {
                    if (!typedMapKeyCompatible(compiler, recvVar.mapKeyType, keyExpr)) {
                        compilerErrorAt(compiler, keyAst ? keyAst->token.line : get->name.line, "typed map key type mismatch");
                        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                        return NULL;
                    }
                }
                LLVMValueRef key = tuaValueFromKey(compiler, keyExpr);
                if (!key) {
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return NULL;
                }

                LLVMTypeRef i32 = LLVMInt32TypeInContext(compiler->context);
                LLVMValueRef okPtr = LLVMBuildAlloca(compiler->builder, i32, "mokptr");
                LLVMValueRef gfn = getOrCreateTuaMapGetRefWithOk(compiler);
                LLVMTypeRef gtype = LLVMGlobalGetValueType(gfn);
                LLVMValueRef args3[3] = { mapPtr, key, okPtr };
                LLVMValueRef p = LLVMBuildCall2(compiler->builder, gtype, gfn, args3, 3, "mgetp");
                LLVMValueRef ok32 = LLVMBuildLoad2(compiler->builder, i32, okPtr, "mok32");
                LLVMValueRef ok = LLVMBuildTrunc(compiler->builder, ok32, LLVMInt1TypeInContext(compiler->context), "mok");

                LLVMTypeRef vt = compilerGetTuaValueType(compiler);
                LLVMTypeRef vtPtr = LLVMPointerType(vt, 0);
                LLVMValueRef tvPtr = castValueToType(compiler, p, vtPtr);

                // Untyped map: return Option<tua_value*>
                if (!recvVar.isTypedMap || !recvVar.mapValueType) {
                    LLVMTypeRef optType = compilerGetOptionType(compiler, vtPtr);
                    LLVMValueRef opt = LLVMGetUndef(optType);
                    opt = LLVMBuildInsertValue(compiler->builder, opt, ok, 0, "o0");
                    opt = LLVMBuildInsertValue(compiler->builder, opt, tvPtr, 1, "o1");
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return opt;
                }

                // Typed map: only support element references when V is non-scalar (struct/map/array),
                // and return Option<Ref<V>> (lowered as pointer to V).
                LLVMTypeRef innerTy = recvVar.mapValueType;
                if (typedMapValueIsScalarMeta(&recvVar)) {
                    compilerErrorAt(compiler, get->name.line, "typed map scalar values do not support element references; use m[k]");
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return NULL;
                }

                LLVMContextRef context = compiler->context;
                LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
                LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(context), 0);

                // When ok==false, return null payload.
                LLVMTypeRef outPtrTy = LLVMPointerType(innerTy, 0);
                LLVMValueRef outPtr = LLVMConstNull(outPtrTy);

                LLVMValueRef fn = compiler->current->func;
                LLVMBasicBlockRef someBB = LLVMAppendBasicBlock(fn, "mref.some");
                LLVMBasicBlockRef noneBB = LLVMAppendBasicBlock(fn, "mref.none");
                LLVMBasicBlockRef contBB = LLVMAppendBasicBlock(fn, "mref.cont");
                LLVMBuildCondBr(compiler->builder, ok, someBB, noneBB);

                LLVMPositionBuilderAtEnd(compiler->builder, someBB);
                // tvPtr points to tua_value { tag:i32, payload:i64 }.
                LLVMValueRef payloadField = LLVMBuildStructGEP2(compiler->builder, vt, tvPtr, 1, "payp");
                LLVMValueRef ptrV = NULL;
                if (LLVMGetTypeKind(innerTy) == LLVMStructTypeKind) {
                    // payload contains a heap pointer to the struct bytes.
                    LLVMValueRef bits = LLVMBuildLoad2(compiler->builder, i64, payloadField, "bits");
                    LLVMValueRef p8 = LLVMBuildIntToPtr(compiler->builder, bits, i8ptr, "p8");
                    ptrV = LLVMBuildBitCast(compiler->builder, p8, outPtrTy, "sp");
                } else {
                    // innerTy is a handle/pointer type (e.g. tua_map* / tua_array* / ptr / Ref<...>):
                    // return a pointer to the stored payload bits (treating it as a pointer-sized slot).
                    ptrV = LLVMBuildBitCast(compiler->builder, payloadField, outPtrTy, "hp");
                }
                LLVMBuildBr(compiler->builder, contBB);
                LLVMBasicBlockRef someEnd = LLVMGetInsertBlock(compiler->builder);

                LLVMPositionBuilderAtEnd(compiler->builder, noneBB);
                LLVMBuildBr(compiler->builder, contBB);
                LLVMBasicBlockRef noneEnd = LLVMGetInsertBlock(compiler->builder);

                LLVMPositionBuilderAtEnd(compiler->builder, contBB);
                LLVMValueRef phi = LLVMBuildPhi(compiler->builder, outPtrTy, "mrefv");
                LLVMAddIncoming(phi, &ptrV, &someEnd, 1);
                LLVMAddIncoming(phi, &outPtr, &noneEnd, 1);

                LLVMTypeRef optType = compilerGetOptionType(compiler, outPtrTy);
                LLVMValueRef opt = LLVMGetUndef(optType);
                opt = LLVMBuildInsertValue(compiler->builder, opt, ok, 0, "o0");
                opt = LLVMBuildInsertValue(compiler->builder, opt, phi, 1, "o1");
                if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                return opt;
            }

            if (mid == MAP_M_GETREF || mid == MAP_M_GETREFWRITE) {
                compilerErrorAt(compiler, get->name.line, "map.getRef/getRefWrite is removed; use get/getMut");
                if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                return NULL;
            }

            if (mid == MAP_M_CLEAR) {
                if (got != 0) {
                    emitDebug("map.clear expects 0 arguments\n");
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return NULL;
                }
                LLVMValueRef fn = getOrCreateTuaMapClear(compiler);
                LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
                LLVMValueRef args1[1] = { mapPtr };
                LLVMBuildCall2(compiler->builder, fnType, fn, args1, 1, "");
                if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                return LLVMConstInt(LLVMInt32TypeInContext(compiler->context), 0, 0);
            }

            // Note: value reads use `m[k]`.
        }

        // Option built-in methods (variable receiver): `o.isSome()`, `o.unwrap()`, ...
        if (recvVar.value && isOptionLLVMType(recvVar.type)) {
            LLVMValueRef optVal = loadLocalValue(compiler, recvVar);
            int saw = 0;
            LLVMValueRef implOut = tryEmitBuiltinValueImplMethodCall(
                compiler,
                "Option",
                6,
                get->object,
                optVal,
                get,
                expr,
                wantMultiForThisCall,
                &saw
            );
            if (implOut) return implOut;
            LLVMTypeRef optType = recvVar.type;
            LLVMTypeRef innerType = LLVMStructGetTypeAtIndex(optType, 1);
            LLVMValueRef ok = LLVMBuildExtractValue(compiler->builder, optVal, 0, "opt_ok");
            LLVMValueRef payload = LLVMBuildExtractValue(compiler->builder, optVal, 1, "opt_v");
            unsigned got = expr->arguments ? (unsigned)expr->arguments->length : 0;

            if (tokenEquals(&get->name, "isSome")) {
                if (got != 0) {
                    emitDebug("Option.isSome expects 0 arguments\n");
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return NULL;
                }
                if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                return ok;
            }

            if (tokenEquals(&get->name, "isNone")) {
                if (got != 0) {
                    emitDebug("Option.isNone expects 0 arguments\n");
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return NULL;
                }
                LLVMValueRef out = LLVMBuildNot(compiler->builder, ok, "opt_not");
                if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                return out;
            }

            if (tokenEquals(&get->name, "unwrap")) {
                if (got != 0) {
                    emitDebug("Option.unwrap expects 0 arguments\n");
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return NULL;
                }
                LLVMValueRef fn = compiler->current->func;
                LLVMBasicBlockRef someBB = LLVMAppendBasicBlock(fn, "opt.unwrap.some");
                LLVMBasicBlockRef noneBB = LLVMAppendBasicBlock(fn, "opt.unwrap.none");
                LLVMBasicBlockRef contBB = LLVMAppendBasicBlock(fn, "opt.unwrap.cont");
                LLVMBuildCondBr(compiler->builder, ok, someBB, noneBB);

                LLVMPositionBuilderAtEnd(compiler->builder, noneBB);
                LLVMValueRef panicFn = getOrCreateTuaPanic(compiler);
                LLVMTypeRef panicType = LLVMGlobalGetValueType(panicFn);
                LLVMValueRef msg = LLVMBuildGlobalStringPtr(compiler->builder, "unwrap on None", "panicmsg");
                LLVMBuildCall2(compiler->builder, panicType, panicFn, &msg, 1, "");
                LLVMBuildUnreachable(compiler->builder);

                LLVMPositionBuilderAtEnd(compiler->builder, someBB);
                LLVMBuildBr(compiler->builder, contBB);

                LLVMPositionBuilderAtEnd(compiler->builder, contBB);
                if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                return payload;
            }

            if (tokenEquals(&get->name, "unwrapOr")) {
                if (got != 1) {
                    emitDebug("Option.unwrapOr expects 1 argument\n");
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return NULL;
                }
                LLVMValueRef defRaw = compileExpr(compiler, (Expr*)expr->arguments->head->data);
                defRaw = castValueToType(compiler, defRaw, innerType);

                LLVMValueRef fn = compiler->current->func;
                LLVMBasicBlockRef someBB = LLVMAppendBasicBlock(fn, "opt.uor.some");
                LLVMBasicBlockRef noneBB = LLVMAppendBasicBlock(fn, "opt.uor.none");
                LLVMBasicBlockRef contBB = LLVMAppendBasicBlock(fn, "opt.uor.cont");
                LLVMBuildCondBr(compiler->builder, ok, someBB, noneBB);

                LLVMPositionBuilderAtEnd(compiler->builder, someBB);
                LLVMBuildBr(compiler->builder, contBB);
                LLVMBasicBlockRef someEnd = LLVMGetInsertBlock(compiler->builder);

                LLVMPositionBuilderAtEnd(compiler->builder, noneBB);
                LLVMBuildBr(compiler->builder, contBB);
                LLVMBasicBlockRef noneEnd = LLVMGetInsertBlock(compiler->builder);

                LLVMPositionBuilderAtEnd(compiler->builder, contBB);
                LLVMValueRef phi = LLVMBuildPhi(compiler->builder, innerType, "uor");
                LLVMAddIncoming(phi, &payload, &someEnd, 1);
                LLVMAddIncoming(phi, &defRaw, &noneEnd, 1);
                if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                return phi;
            }
        }

        // Trait object method call (dynamic dispatch): when receiver is a trait object value.
        if (recvVar.value && recvVar.isTraitObj) {
            if (!recvVar.traitName || recvVar.traitNameLength <= 0) {
                compilerErrorAtToken(compiler, &get->name, "missing trait metadata for receiver");
                if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                return NULL;
            }
            TraitInfo* trait = compilerFindTrait(compiler, recvVar.traitName, recvVar.traitNameLength);
            if (!trait || !trait->decl) {
                compilerErrorAtToken(compiler, &get->name, "unknown trait for receiver");
                if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                return NULL;
            }
            int midx = traitMethodIndex(trait, &get->name);
            if (midx < 0) {
                compilerErrorAtToken(
                    compiler,
                    &get->name,
                    "method '%.*s' is not in trait '%.*s'",
                    get->name.length,
                    get->name.start,
                    trait->nameLength,
                    trait->name
                );
                if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                return NULL;
            }
            TraitMethodDecl* m = traitMethodDeclAt(trait, midx);
            if (!m) {
                if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                return NULL;
            }

            LLVMValueRef obj = loadLocalValue(compiler, recvVar);
            if (!obj) {
                if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                return NULL;
            }
            LLVMTypeRef objTy = LLVMTypeOf(obj);
            if (LLVMGetTypeKind(objTy) != LLVMStructTypeKind || LLVMCountStructElementTypes(objTy) != 2) {
                compilerErrorAtToken(compiler, &get->name, "invalid trait object representation");
                if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                return NULL;
            }

            LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
            LLVMValueRef data = LLVMBuildExtractValue(compiler->builder, obj, 0, "to_data");
            LLVMValueRef vtp = LLVMBuildExtractValue(compiler->builder, obj, 1, "to_vt");

            LLVMTypeRef vtTy = compilerGetTraitVtableType(compiler, trait);
            LLVMValueRef vtptr = LLVMBuildBitCast(compiler->builder, vtp, LLVMPointerType(vtTy, 0), "vtptr");
            LLVMValueRef slotPtr = LLVMBuildStructGEP2(compiler->builder, vtTy, vtptr, (unsigned)(1 + midx), "vt_m_p");
            LLVMValueRef fnRaw = LLVMBuildLoad2(compiler->builder, i8ptr, slotPtr, "vt_m");

            int argc = m->params ? m->params->length : 0;
            unsigned got = expr->arguments ? (unsigned)expr->arguments->length : 0;
            if (got != (unsigned)argc) {
                compilerErrorAtToken(
                    compiler,
                    &get->name,
                    "argument count mismatch for call '%.*s': expected %d, got %u",
                    get->name.length,
                    get->name.start,
                    argc,
                    got
                );
                if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                return NULL;
            }

            LLVMTypeRef retTy = llvmReturnTypeForTraitMethod(compiler, m);
            LLVMTypeRef* pts = malloc(sizeof(LLVMTypeRef) * (size_t)(1 + argc));
            pts[0] = i8ptr;
            for (int i = 0; i < argc; i++) {
                Parameter* p = (Parameter*)listGet(m->params, i);
                pts[1 + i] = llvmParamTypeForTraitMethod(compiler, p);
            }
            LLVMTypeRef fnTy = LLVMFunctionType(retTy, pts, (unsigned)(1 + argc), 0);

            LLVMValueRef fn = LLVMBuildBitCast(compiler->builder, fnRaw, LLVMPointerType(fnTy, 0), "vt_fn");

            LLVMValueRef* args = malloc(sizeof(LLVMValueRef) * (size_t)(1 + argc));
            args[0] = data;
            for (int i = 0; i < argc; i++) {
                Expr* argAst = (Expr*)listGet(expr->arguments, i);
                LLVMValueRef av = compileCallArgForParam(compiler, argAst, pts[1 + i]);
                if (!av) {
                    free(pts);
                    free(args);
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return NULL;
                }
                args[1 + i] = av;
            }
            free(pts);

            LLVMValueRef call = LLVMBuildCall2(compiler->builder, fnTy, fn, args, (unsigned)(1 + argc), callInstNameForFnType(fnTy));
            free(args);
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            LLVMValueRef out = collapseMultiReturnByTypeIfNeeded(compiler, call, retTy);
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return out;
	        }

	        // Scalar `impl` methods (e.g. `impl int { ... }`) use the same `<TypeName>__<method>` lowering
	        // as builtin value types, but don't participate in struct instance resolution.
	        if (recvVar.value && typeKindIsScalarValueKind(recvVar.typeKind)) {
	            const char* tn = NULL;
	            int tnLen = 0;
	            if (implTypeNameForTypeKind(recvVar.typeKind, &tn, &tnLen)) {
	                LLVMValueRef recvVal = loadLocalValue(compiler, recvVar);
	                if (!recvVal) {
	                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
	                    return NULL;
	                }
	                int saw = 0;
	                LLVMValueRef implOut = tryEmitBuiltinValueImplMethodCall(
	                    compiler,
	                    tn,
	                    tnLen,
	                    get->object,
	                    recvVal,
	                    get,
	                    expr,
	                    wantMultiForThisCall,
	                    &saw
	                );
	                if (implOut) return implOut;
	            }
	        }

	        bool isInstance = recvVar.value != NULL && recvVar.typeName != NULL;
	        bool isTraitDispatch = isInstance && recvVar.genericBoundTraitName != NULL && recvVar.genericBoundTraitNameLength > 0;
	        if (hasTypeArgs && isInstance) {
	            compilerErrorAtToken(compiler, &get->name, "generic type arguments on instance methods are not supported yet");
	            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
	            return NULL;
        }

        if (isTraitDispatch) {
            TraitInfo* boundTrait = compilerFindTrait(compiler, recvVar.genericBoundTraitName, recvVar.genericBoundTraitNameLength);
            if (!boundTrait || !boundTrait->decl) {
                compilerErrorAtToken(compiler, &get->name, "unknown trait bound for dispatch");
                if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                return NULL;
            }
            if (!traitDeclHasMethod(boundTrait, &get->name)) {
                compilerErrorAtToken(
                    compiler,
                    &get->name,
                    "method '%.*s' is not in trait bound '%.*s'",
                    get->name.length,
                    get->name.start,
                    recvVar.genericBoundTraitNameLength,
                    recvVar.genericBoundTraitName
                );
                if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                return NULL;
            }
        }

        int mangledLen = 0;
        char* mangled = NULL;
        LLVMValueRef func = NULL;

        if (isTraitDispatch) {
            // First try trait-namespaced method implementation: `<Struct>__<Trait>__<method>`.
            Token traitTok = (Token){TOKEN_IDENTIFIER, recvVar.genericBoundTraitName, recvVar.genericBoundTraitNameLength, get->name.line, get->name.col, 0};
            int stLen = 0;
            char* st = mangleRawAndToken(recvVar.typeName, recvVar.typeNameLength, &traitTok, &stLen);
            mangled = mangleRawAndToken(st, stLen, &get->name, &mangledLen);
            free(st);
            func = LLVMGetNamedFunction(compiler->module, mangled);
            free(mangled);
            mangled = NULL;
            mangledLen = 0;
        }

        if (isInstance) {
            mangled = mangleRawAndToken(recvVar.typeName, recvVar.typeNameLength, &get->name, &mangledLen);
        } else {
            SymbolAlias* a = compilerFindAlias(compiler, recvNameExpr->name.start, recvNameExpr->name.length);
            if (a && (a->kind == ALIAS_OBJECT || a->kind == ALIAS_ENUM || a->kind == ALIAS_STRUCT)) {
                mangled = mangleRawAndToken(a->qualified, a->qualifiedLen, &get->name, &mangledLen);
            } else if (compiler->currentModulePrefix) {
                int ql = 0;
                char* q = compilerQualifyToken(compiler, &recvNameExpr->name, &ql);
                if (q) {
                    mangled = mangleRawAndToken(q, ql, &get->name, &mangledLen);
                    free(q);
                }
            }
            if (!mangled) {
                mangled = mangleRawAndToken(recvNameExpr->name.start, recvNameExpr->name.length, &get->name, &mangledLen);
            }
        }

        if (!func) {
            func = LLVMGetNamedFunction(compiler->module, mangled);
        }
        free(mangled);

        PromotedMethodPath promoted = {0};
        StructInfo* promotedRoot = NULL;
        if (!func && isInstance) {
                Token t = (Token){TOKEN_IDENTIFIER, recvVar.typeName, recvVar.typeNameLength, get->name.line, get->name.col, 0};
                promotedRoot = compilerResolveStructByToken(compiler, &t);
                if (promotedRoot) {
                    int pr = resolvePromotedMethodPath(compiler, promotedRoot, &get->name, &promoted);
                    if (pr < 0) {
                    compilerErrorAtToken(compiler, &get->name, "ambiguous method: %.*s (write explicit path)", get->name.length, get->name.start);
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return NULL;
                }
                if (pr > 0) {
                    func = promoted.func;
                }
            }
        }
        if (!func) {
            compilerErrorAtToken(compiler, &get->name, "undefined method: %.*s", get->name.length, get->name.start);
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }

        LLVMTypeRef funcType = LLVMGlobalGetValueType(func);
        unsigned expected = LLVMCountParamTypes(funcType);
        unsigned got = expr->arguments ? (unsigned)expr->arguments->length : 0;
        if ((!isInstance && expected != got) || (isInstance && expected != got + 1)) {
            compilerErrorAtToken(
                compiler,
                &get->name,
                "argument count mismatch for call '%.*s': expected %u, got %u",
                get->name.length,
                get->name.start,
                isInstance ? (unsigned)(expected - 1) : expected,
                got
            );
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }

        LLVMTypeRef* paramTypes = NULL;
        if (expected > 0) {
            paramTypes = malloc(sizeof(LLVMTypeRef) * (size_t)expected);
            LLVMGetParamTypes(funcType, paramTypes);
        }

        LLVMValueRef* args = NULL;
        if (expected > 0) {
            args = malloc(sizeof(LLVMValueRef) * (size_t)expected);

            unsigned argIndex = 0;
            ListNode* node = expr->arguments ? expr->arguments->head : NULL;

            if (isInstance) {
                LLVMValueRef thisArg = NULL;
                if (recvVar.isBoxed) {
                    if (!recvVar.boxPtrType) {
                        emitDebug("Missing boxed pointer type for receiver\n");
                        return NULL;
                    }
                    LLVMValueRef cellPtr = LLVMBuildLoad2(compiler->builder, recvVar.boxPtrType, recvVar.value, "cellptr");
                    if (LLVMGetTypeKind(recvVar.type) == LLVMPointerTypeKind) {
                        // Cell contains a pointer value (&T)
                        thisArg = LLVMBuildLoad2(compiler->builder, recvVar.type, cellPtr, "this");
                    } else {
                        // Cell is the struct storage itself.
                        thisArg = cellPtr;
                    }
                } else if (LLVMGetTypeKind(recvVar.type) == LLVMPointerTypeKind) {
                    thisArg = LLVMBuildLoad2(compiler->builder, recvVar.type, recvVar.value, "this");
                } else {
                    thisArg = recvVar.value; // alloca already yields pointer to struct value
                }

                // Embedded-method promotion: adjust receiver to `&this.<embeddedPath>`.
                if (promotedRoot && promoted.depth > 0) {
                    LLVMValueRef p = thisArg;
                    LLVMTypeRef curType = promotedRoot->type;
                    StructInfo* curInfo = promotedRoot;
                    for (int i = 0; i < promoted.depth; i++) {
                        int embIdx = promoted.indices[i];
                        FieldDeclaration* f = curInfo && curInfo->decl ? (FieldDeclaration*)listGet(curInfo->decl->fields, embIdx) : NULL;
                        if (!f || !f->type || f->type->kind != TYPE_NAMED) {
                            compilerErrorAtToken(compiler, &get->name, "invalid embedded receiver path");
                            if (paramTypes) free(paramTypes);
                            if (args) free(args);
                            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                            return NULL;
                        }
                        StructInfo* inner = compilerResolveStructByToken(compiler, &f->type->name);
                        if (!inner) {
                            compilerErrorAtToken(compiler, &get->name, "unknown embedded struct type");
                            if (paramTypes) free(paramTypes);
                            if (args) free(args);
                            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                            return NULL;
                        }
                        p = LLVMBuildStructGEP2(compiler->builder, curType, p, (unsigned)embIdx, "emb_ptr");
                        curType = inner->type;
                        curInfo = inner;
                    }
                    thisArg = p;
                }
                thisArg = castValueToType(compiler, thisArg, paramTypes[0]);
                args[argIndex++] = thisArg;
            }

            for (; argIndex < expected; argIndex++) {
                LLVMValueRef av = compileCallArgForParam(compiler, (Expr*)node->data, paramTypes[argIndex]);
                if (!av) {
                    if (paramTypes) free(paramTypes);
                    if (args) free(args);
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return NULL;
                }
                args[argIndex] = av;
                node = node->next;
            }
        }

        LLVMValueRef call = LLVMBuildCall2(compiler->builder, funcType, func, args, expected, callInstNameForFnType(funcType));
        if (paramTypes) free(paramTypes);
        if (args) free(args);
        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
        LLVMValueRef out = collapseMultiReturnIfNeeded(compiler, func, call);
        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
        return out;
    }

    if (expr->callee->type != EXPR_VARIABLE) {
        emitDebug("Only simple calls are supported for now\n");
        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
        return NULL;
    }

    VariableExpr* callee = (VariableExpr*)expr->callee;
    BuiltinId builtinId = lookupBuiltinId(&callee->name);
    int isPrintln = builtinId == BI_PRINTLN;
    int isPrint = builtinId == BI_PRINT;

    if (hasTypeArgs && builtinId != BI_NONE) {
        compilerErrorAtToken(compiler, &callee->name, "built-in '%.*s' does not accept generic type arguments", callee->name.length, callee->name.start);
        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
        return NULL;
    }

    if (builtinId == BI_SOME) {
        unsigned got = expr->arguments ? (unsigned)expr->arguments->length : 0;
        if (got != 1) {
            emitDebug("Some expects 1 argument\n");
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        LLVMValueRef v = compileExpr(compiler, (Expr*)expr->arguments->head->data);
        if (!v) {
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        LLVMTypeRef inner = LLVMTypeOf(v);
        LLVMTypeRef optType = compilerGetOptionType(compiler, inner);
        LLVMValueRef ok = LLVMConstInt(LLVMInt1TypeInContext(compiler->context), 1, 0);
        LLVMValueRef opt = LLVMGetUndef(optType);
        opt = LLVMBuildInsertValue(compiler->builder, opt, ok, 0, "o0");
        opt = LLVMBuildInsertValue(compiler->builder, opt, v, 1, "o1");
        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
        return opt;
    }

    if (builtinId == BI_NONE_CTOR) {
        unsigned got = expr->arguments ? (unsigned)expr->arguments->length : 0;
        if (got != 0) {
            emitDebug("None expects 0 arguments\n");
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        LLVMTypeRef vt = compilerGetTuaValueType(compiler);
        LLVMTypeRef optType = compilerGetOptionType(compiler, vt);
        LLVMValueRef ok = LLVMConstInt(LLVMInt1TypeInContext(compiler->context), 0, 0);
        LLVMValueRef nil = tuaValueMake(compiler, TUA_VAL_NIL, NULL);
        LLVMValueRef opt = LLVMGetUndef(optType);
        opt = LLVMBuildInsertValue(compiler->builder, opt, ok, 0, "o0");
        opt = LLVMBuildInsertValue(compiler->builder, opt, nil, 1, "o1");
        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
        return opt;
    }

    if (builtinId == BI_ASSERT) {
        unsigned got = expr->arguments ? (unsigned)expr->arguments->length : 0;
        if (got != 1 && got != 2) {
            emitDebug("assert expects 1 or 2 arguments\n");
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }

        LLVMValueRef cond = compileExpr(compiler, (Expr*)expr->arguments->head->data);
        cond = llvmCoerceToBool(compiler, cond);
        if (!cond) {
            emitDebug("assert condition cannot be coerced to bool\n");
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }

        LLVMValueRef fn = compiler->current->func;
        LLVMBasicBlockRef okBB = LLVMAppendBasicBlock(fn, "assert.ok");
        LLVMBasicBlockRef failBB = LLVMAppendBasicBlock(fn, "assert.fail");
        LLVMBasicBlockRef contBB = LLVMAppendBasicBlock(fn, "assert.cont");

        LLVMBuildCondBr(compiler->builder, cond, okBB, failBB);

        LLVMPositionBuilderAtEnd(compiler->builder, failBB);
        LLVMValueRef msg = LLVMConstNull(LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0));
        if (got == 2) {
            LLVMValueRef raw = compileExpr(compiler, (Expr*)expr->arguments->head->next->data);
            if (raw) {
                LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
                if (LLVMTypeOf(raw) == i8ptr) msg = raw;
            }
        }
        LLVMValueRef line = LLVMConstInt(LLVMInt32TypeInContext(compiler->context), (unsigned)callee->name.line, 0);
        LLVMValueRef af = getOrCreateTuaAssertFail(compiler);
        LLVMTypeRef afType = LLVMGlobalGetValueType(af);
        LLVMValueRef args2[2] = { msg, line };
        LLVMBuildCall2(compiler->builder, afType, af, args2, 2, "");
        LLVMBuildUnreachable(compiler->builder);

        LLVMPositionBuilderAtEnd(compiler->builder, okBB);
        LLVMBuildBr(compiler->builder, contBB);

        LLVMPositionBuilderAtEnd(compiler->builder, contBB);
        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
        return LLVMConstInt(LLVMInt32TypeInContext(compiler->context), 0, 0);
    }

    if (builtinId == BI_LEN) {
        unsigned got = expr->arguments ? (unsigned)expr->arguments->length : 0;
        if (got != 1) {
            emitDebug("len expects 1 argument\n");
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        Expr* argAst = (Expr*)expr->arguments->head->data;
        if (!argAst || argAst->type != EXPR_VARIABLE) {
            emitDebug("len currently only supports map/array variables\n");
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        VariableRef v = findVariableExpr(compiler, argAst);
        LLVMValueRef arg0 = compileExpr(compiler, argAst);
        if (!arg0) {
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        if (v.value && v.isMap) {
            LLVMValueRef fn = getOrCreateTuaMapLen(compiler);
            LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
            LLVMValueRef args1[1] = { arg0 };
            LLVMValueRef out = LLVMBuildCall2(compiler->builder, fnType, fn, args1, 1, "len");
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return out;
        }
        if (v.value && v.isArray) {
            LLVMTypeRef arrStruct = LLVMGetTypeByName2(compiler->context, "tua_array");
            if (!arrStruct) {
                emitDebug("missing tua_array type\n");
                if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                return NULL;
            }
            LLVMTypeRef i64 = LLVMInt64TypeInContext(compiler->context);
            LLVMTypeRef i32 = LLVMInt32TypeInContext(compiler->context);
            LLVMValueRef lenPtr = LLVMBuildStructGEP2(compiler->builder, arrStruct, arg0, 0, "alenp");
            LLVMValueRef len64 = LLVMBuildLoad2(compiler->builder, i64, lenPtr, "alen64");
            LLVMValueRef out = LLVMBuildTrunc(compiler->builder, len64, i32, "len");
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return out;
        }
        emitDebug("len expects a map/array variable\n");
        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
        return NULL;
    }

    if (builtinId == BI_TUA_PARSE_INT) {
        unsigned got = expr->arguments ? (unsigned)expr->arguments->length : 0;
        if (got != 2) {
            emitDebug("tua_parse_int expects 2 arguments\n");
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        LLVMValueRef s = compileExpr(compiler, (Expr*)expr->arguments->head->data);
        LLVMValueRef outPtr = compileExpr(compiler, (Expr*)expr->arguments->head->next->data);
        if (!s || !outPtr) {
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
        LLVMTypeRef i32 = LLVMInt32TypeInContext(compiler->context);
        LLVMTypeRef i32ptr = LLVMPointerType(i32, 0);
        s = castValueToType(compiler, s, i8ptr);
        outPtr = castValueToType(compiler, outPtr, i32ptr);
        LLVMValueRef fn = getOrCreateTuaParseInt(compiler);
        LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
        LLVMValueRef args2[2] = { s, outPtr };
        LLVMValueRef ok32 = LLVMBuildCall2(compiler->builder, fnType, fn, args2, 2, "ok32");
        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
        return ok32;
    }

    if (builtinId == BI_TUA_FREE) {
        unsigned got = expr->arguments ? (unsigned)expr->arguments->length : 0;
        if (got != 1) {
            emitDebug("tua_free expects 1 argument\n");
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        LLVMValueRef p = compileExpr(compiler, (Expr*)expr->arguments->head->data);
        if (!p) {
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
        p = castValueToType(compiler, p, i8ptr);
        LLVMValueRef fn = getOrCreateTuaFree(compiler);
        LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
        LLVMBuildCall2(compiler->builder, fnType, fn, &p, 1, "");
        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
        return LLVMConstInt(LLVMInt32TypeInContext(compiler->context), 0, 0);
    }

    if (builtinId == BI_TUA_DEADLINE_AFTER_MS) {
        unsigned got = expr->arguments ? (unsigned)expr->arguments->length : 0;
        if (got != 1) {
            emitDebug("tua_deadline_after_ms expects 1 argument\n");
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        LLVMValueRef ms = compileExpr(compiler, (Expr*)expr->arguments->head->data);
        if (!ms) {
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        LLVMTypeRef i64 = LLVMInt64TypeInContext(compiler->context);
        ms = castValueToType(compiler, ms, i64);
        LLVMValueRef fn = getOrCreateTuaDeadlineAfterMs(compiler);
        LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
        LLVMValueRef out = LLVMBuildCall2(compiler->builder, fnType, fn, &ms, 1, "deadline");
        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
        return out;
    }

    if (builtinId == BI_TUA_TIME_MONO_NS || builtinId == BI_TUA_TIME_REAL_NS) {
        unsigned got = expr->arguments ? (unsigned)expr->arguments->length : 0;
        if (got != 0) {
            emitDebug("%s expects 0 arguments\n", builtinId == BI_TUA_TIME_MONO_NS ? "tua_time_mono_ns" : "tua_time_real_ns");
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        LLVMValueRef fn = builtinId == BI_TUA_TIME_MONO_NS ? getOrCreateTuaTimeMonoNs(compiler) : getOrCreateTuaTimeRealNs(compiler);
        LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
        LLVMValueRef out = LLVMBuildCall2(compiler->builder, fnType, fn, NULL, 0, "ns");
        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
        return out;
    }

    if (builtinId == BI_TUA_SLEEP_NS) {
        unsigned got = expr->arguments ? (unsigned)expr->arguments->length : 0;
        if (got != 1) {
            emitDebug("tua_sleep_ns expects 1 argument\n");
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        LLVMValueRef ns = compileExpr(compiler, (Expr*)expr->arguments->head->data);
        if (!ns) {
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        LLVMTypeRef i64 = LLVMInt64TypeInContext(compiler->context);
        ns = castValueToType(compiler, ns, i64);
        LLVMValueRef fn = getOrCreateTuaSleepNs(compiler);
        LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
        LLVMBuildCall2(compiler->builder, fnType, fn, &ns, 1, "");
        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
        return LLVMConstInt(LLVMInt32TypeInContext(compiler->context), 0, 0);
    }

    if (builtinId == BI_TUA_TIMER_AFTER_MS_CL) {
        unsigned got = expr->arguments ? (unsigned)expr->arguments->length : 0;
        if (got != 3) {
            emitDebug("tua_timer_after_ms_cl expects 3 arguments\n");
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        LLVMValueRef loopV = compileExpr(compiler, (Expr*)expr->arguments->head->data);
        LLVMValueRef delay = compileExpr(compiler, (Expr*)expr->arguments->head->next->data);
        LLVMValueRef cb = compileExpr(compiler, (Expr*)expr->arguments->head->next->next->data);
        if (!loopV || !delay || !cb) {
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
        LLVMTypeRef i64 = LLVMInt64TypeInContext(compiler->context);
        LLVMTypeRef closure = compilerGetClosureType(compiler);
        loopV = castValueToType(compiler, loopV, i8ptr);
        delay = castValueToType(compiler, delay, i64);
        cb = castValueToType(compiler, cb, closure);
        LLVMValueRef fn = getOrCreateTuaTimerAfterMsCl(compiler);
        LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
        LLVMValueRef args3[3] = { loopV, delay, cb };
        LLVMValueRef out = LLVMBuildCall2(compiler->builder, fnType, fn, args3, 3, "err");
        dropTemporaryClosureArgs(compiler, expr->arguments, args3, 0);
        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
        return out;
    }

    if (builtinId == BI_TUA_TIMER_EVERY_MS_CL) {
        unsigned got = expr->arguments ? (unsigned)expr->arguments->length : 0;
        if (got != 4) {
            emitDebug("tua_timer_every_ms_cl expects 4 arguments\n");
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        LLVMValueRef loopV = compileExpr(compiler, (Expr*)expr->arguments->head->data);
        LLVMValueRef interval = compileExpr(compiler, (Expr*)expr->arguments->head->next->data);
        LLVMValueRef cb = compileExpr(compiler, (Expr*)expr->arguments->head->next->next->data);
        LLVMValueRef outHandlePtr = compileExpr(compiler, (Expr*)expr->arguments->head->next->next->next->data);
        if (!loopV || !interval || !cb || !outHandlePtr) {
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
        LLVMTypeRef i8ptrptr = LLVMPointerType(i8ptr, 0);
        LLVMTypeRef i64 = LLVMInt64TypeInContext(compiler->context);
        LLVMTypeRef closure = compilerGetClosureType(compiler);
        loopV = castValueToType(compiler, loopV, i8ptr);
        interval = castValueToType(compiler, interval, i64);
        cb = castValueToType(compiler, cb, closure);
        outHandlePtr = castValueToType(compiler, outHandlePtr, i8ptrptr);
        LLVMValueRef fn = getOrCreateTuaTimerEveryMsCl(compiler);
        LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
        LLVMValueRef args4[4] = { loopV, interval, cb, outHandlePtr };
        LLVMValueRef out = LLVMBuildCall2(compiler->builder, fnType, fn, args4, 4, "err");
        dropTemporaryClosureArgs(compiler, expr->arguments, args4, 0);
        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
        return out;
    }

    if (builtinId == BI_TUA_TIMER_EVERY_CANCEL_CL) {
        unsigned got = expr->arguments ? (unsigned)expr->arguments->length : 0;
        if (got != 1) {
            emitDebug("tua_timer_every_cancel_cl expects 1 argument\n");
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        LLVMValueRef handleV = compileExpr(compiler, (Expr*)expr->arguments->head->data);
        if (!handleV) {
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
        handleV = castValueToType(compiler, handleV, i8ptr);
        LLVMValueRef fn = getOrCreateTuaTimerEveryCancelCl(compiler);
        LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
        LLVMBuildCall2(compiler->builder, fnType, fn, &handleV, 1, "");
        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
        return LLVMConstInt(LLVMInt32TypeInContext(compiler->context), 0, 0);
    }

    if (builtinId == BI_TUA_LOOP_CREATE) {
        unsigned got = expr->arguments ? (unsigned)expr->arguments->length : 0;
        if (got != 1) {
            emitDebug("tua_loop_create expects 1 argument\n");
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        LLVMValueRef outPtr = compileExpr(compiler, (Expr*)expr->arguments->head->data);
        if (!outPtr) {
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
        LLVMTypeRef i8ptrptr = LLVMPointerType(i8ptr, 0);
        outPtr = castValueToType(compiler, outPtr, i8ptrptr);
        LLVMValueRef fn = getOrCreateTuaLoopCreate(compiler);
        LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
        LLVMValueRef out = LLVMBuildCall2(compiler->builder, fnType, fn, &outPtr, 1, "err");
        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
        return out;
    }

    if (builtinId == BI_TUA_LOOP_RUN) {
        unsigned got = expr->arguments ? (unsigned)expr->arguments->length : 0;
        if (got != 1) {
            emitDebug("tua_loop_run expects 1 argument\n");
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        LLVMValueRef loopV = compileExpr(compiler, (Expr*)expr->arguments->head->data);
        if (!loopV) {
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
        loopV = castValueToType(compiler, loopV, i8ptr);
        LLVMValueRef fn = getOrCreateTuaLoopRun(compiler);
        LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
        LLVMValueRef out = LLVMBuildCall2(compiler->builder, fnType, fn, &loopV, 1, "err");
        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
        return out;
    }

    if (builtinId == BI_TUA_LOOP_POST_CL) {
        unsigned got = expr->arguments ? (unsigned)expr->arguments->length : 0;
        if (got != 2) {
            emitDebug("tua_loop_post_cl expects 2 arguments\n");
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        LLVMValueRef loopV = compileExpr(compiler, (Expr*)expr->arguments->head->data);
        LLVMValueRef cb = compileExpr(compiler, (Expr*)expr->arguments->head->next->data);
        if (!loopV || !cb) {
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
        LLVMTypeRef closure = compilerGetClosureType(compiler);
        loopV = castValueToType(compiler, loopV, i8ptr);
        cb = castValueToType(compiler, cb, closure);
        LLVMValueRef fn = getOrCreateTuaLoopPostCl(compiler);
        LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
        LLVMValueRef args2[2] = { loopV, cb };
        LLVMValueRef out = LLVMBuildCall2(compiler->builder, fnType, fn, args2, 2, "err");
        dropTemporaryClosureArgs(compiler, expr->arguments, args2, 0);
        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
        return out;
    }

    if (builtinId == BI_TUA_LOOP_STOP || builtinId == BI_TUA_LOOP_FREE) {
        unsigned got = expr->arguments ? (unsigned)expr->arguments->length : 0;
        if (got != 1) {
            emitDebug("%s expects 1 argument\n", builtinId == BI_TUA_LOOP_STOP ? "tua_loop_stop" : "tua_loop_free");
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        LLVMValueRef loopV = compileExpr(compiler, (Expr*)expr->arguments->head->data);
        if (!loopV) {
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
        loopV = castValueToType(compiler, loopV, i8ptr);
        LLVMValueRef fn = builtinId == BI_TUA_LOOP_STOP ? getOrCreateTuaLoopStop(compiler) : getOrCreateTuaLoopFree(compiler);
        LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
        LLVMBuildCall2(compiler->builder, fnType, fn, &loopV, 1, "");
        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
        return LLVMConstInt(LLVMInt32TypeInContext(compiler->context), 0, 0);
    }

    if (builtinId == BI_TUA_WORKQUEUE_CREATE) {
        unsigned got = expr->arguments ? (unsigned)expr->arguments->length : 0;
        if (got != 2) {
            emitDebug("tua_workqueue_create expects 2 arguments\n");
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        LLVMValueRef outPtr = compileExpr(compiler, (Expr*)expr->arguments->head->data);
        LLVMValueRef threads = compileExpr(compiler, (Expr*)expr->arguments->head->next->data);
        if (!outPtr || !threads) {
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        LLVMTypeRef i32 = LLVMInt32TypeInContext(compiler->context);
        LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
        LLVMTypeRef i8ptrptr = LLVMPointerType(i8ptr, 0);
        outPtr = castValueToType(compiler, outPtr, i8ptrptr);
        threads = castValueToType(compiler, threads, i32);
        LLVMValueRef fn = getOrCreateTuaWorkqueueCreate(compiler);
        LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
        LLVMValueRef args2[2] = { outPtr, threads };
        LLVMValueRef out = LLVMBuildCall2(compiler->builder, fnType, fn, args2, 2, "err");
        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
        return out;
    }

    if (builtinId == BI_TUA_WORKQUEUE_FREE) {
        unsigned got = expr->arguments ? (unsigned)expr->arguments->length : 0;
        if (got != 1) {
            emitDebug("tua_workqueue_free expects 1 argument\n");
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        LLVMValueRef wq = compileExpr(compiler, (Expr*)expr->arguments->head->data);
        if (!wq) {
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
        wq = castValueToType(compiler, wq, i8ptr);
        LLVMValueRef fn = getOrCreateTuaWorkqueueFree(compiler);
        LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
        LLVMBuildCall2(compiler->builder, fnType, fn, &wq, 1, "");
        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
        return LLVMConstInt(LLVMInt32TypeInContext(compiler->context), 0, 0);
    }

    if (builtinId == BI_TUA_TCP_LISTEN) {
        unsigned got = expr->arguments ? (unsigned)expr->arguments->length : 0;
        if (got != 4) {
            emitDebug("tua_tcp_listen expects 4 arguments\n");
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        LLVMValueRef host = compileExpr(compiler, (Expr*)expr->arguments->head->data);
        LLVMValueRef port = compileExpr(compiler, (Expr*)expr->arguments->head->next->data);
        LLVMValueRef backlog = compileExpr(compiler, (Expr*)expr->arguments->head->next->next->data);
        LLVMValueRef outPtr = compileExpr(compiler, (Expr*)expr->arguments->head->next->next->next->data);
        if (!host || !port || !backlog || !outPtr) {
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        LLVMTypeRef i32 = LLVMInt32TypeInContext(compiler->context);
        LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
        LLVMTypeRef i8ptrptr = LLVMPointerType(i8ptr, 0);
        host = castValueToType(compiler, host, i8ptr);
        port = castValueToType(compiler, port, i8ptr);
        backlog = castValueToType(compiler, backlog, i32);
        outPtr = castValueToType(compiler, outPtr, i8ptrptr);
        LLVMValueRef fn = getOrCreateTuaTcpListen(compiler);
        LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
        LLVMValueRef args4[4] = { host, port, backlog, outPtr };
        LLVMValueRef out = LLVMBuildCall2(compiler->builder, fnType, fn, args4, 4, "err");
        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
        return out;
    }

    if (builtinId == BI_TUA_TCP_LISTENER_LOCAL_PORT) {
        unsigned got = expr->arguments ? (unsigned)expr->arguments->length : 0;
        if (got != 1) {
            emitDebug("tua_tcp_listener_local_port expects 1 argument\n");
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        LLVMValueRef lst = compileExpr(compiler, (Expr*)expr->arguments->head->data);
        if (!lst) {
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
        lst = castValueToType(compiler, lst, i8ptr);
        LLVMValueRef fn = getOrCreateTuaTcpListenerLocalPort(compiler);
        LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
        LLVMValueRef out = LLVMBuildCall2(compiler->builder, fnType, fn, &lst, 1, "port");
        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
        return out;
    }

    if (builtinId == BI_TUA_TCP_LISTENER_CLOSE || builtinId == BI_TUA_TCP_SOCKET_CLOSE) {
        unsigned got = expr->arguments ? (unsigned)expr->arguments->length : 0;
        if (got != 1) {
            emitDebug("%s expects 1 argument\n", builtinId == BI_TUA_TCP_LISTENER_CLOSE ? "tua_tcp_listener_close" : "tua_tcp_socket_close");
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        LLVMValueRef p = compileExpr(compiler, (Expr*)expr->arguments->head->data);
        if (!p) {
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
        p = castValueToType(compiler, p, i8ptr);
        LLVMValueRef fn = builtinId == BI_TUA_TCP_LISTENER_CLOSE ? getOrCreateTuaTcpListenerClose(compiler) : getOrCreateTuaTcpSocketClose(compiler);
        LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
        LLVMBuildCall2(compiler->builder, fnType, fn, &p, 1, "");
        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
        return LLVMConstInt(LLVMInt32TypeInContext(compiler->context), 0, 0);
    }

    if (builtinId == BI_TUA_FS_READFILE_ALLOC) {
        unsigned got = expr->arguments ? (unsigned)expr->arguments->length : 0;
        if (got != 3) {
            emitDebug("tua_fs_readfile_alloc expects 3 arguments\n");
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        LLVMValueRef path = compileExpr(compiler, (Expr*)expr->arguments->head->data);
        LLVMValueRef outData = compileExpr(compiler, (Expr*)expr->arguments->head->next->data);
        LLVMValueRef outLen = compileExpr(compiler, (Expr*)expr->arguments->head->next->next->data);
        if (!path || !outData || !outLen) {
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        LLVMTypeRef i32 = LLVMInt32TypeInContext(compiler->context);
        LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
        LLVMTypeRef i8ptrptr = LLVMPointerType(i8ptr, 0);
        LLVMTypeRef i32ptr = LLVMPointerType(i32, 0);
        path = castValueToType(compiler, path, i8ptr);
        outData = castValueToType(compiler, outData, i8ptrptr);
        outLen = castValueToType(compiler, outLen, i32ptr);
        LLVMValueRef fn = getOrCreateTuaFsReadfileAlloc(compiler);
        LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
        LLVMValueRef args3[3] = { path, outData, outLen };
        LLVMValueRef out = LLVMBuildCall2(compiler->builder, fnType, fn, args3, 3, "err");
        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
        return out;
    }

    if (builtinId == BI_TUA_FS_WRITEFILE_STR) {
        unsigned got = expr->arguments ? (unsigned)expr->arguments->length : 0;
        if (got != 2) {
            emitDebug("tua_fs_writefile_str expects 2 arguments\n");
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        LLVMValueRef path = compileExpr(compiler, (Expr*)expr->arguments->head->data);
        LLVMValueRef data = compileExpr(compiler, (Expr*)expr->arguments->head->next->data);
        if (!path || !data) {
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
        path = castValueToType(compiler, path, i8ptr);
        data = castValueToType(compiler, data, i8ptr);
        LLVMValueRef fn = getOrCreateTuaFsWritefileStr(compiler);
        LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
        LLVMValueRef args2[2] = { path, data };
        LLVMValueRef out = LLVMBuildCall2(compiler->builder, fnType, fn, args2, 2, "err");
        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
        return out;
    }

    if (builtinId == BI_TUA_FS_STAT_SIMPLE) {
        unsigned got = expr->arguments ? (unsigned)expr->arguments->length : 0;
        if (got != 5) {
            emitDebug("tua_fs_stat_simple expects 5 arguments\n");
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        LLVMValueRef path = compileExpr(compiler, (Expr*)expr->arguments->head->data);
        LLVMValueRef outKind = compileExpr(compiler, (Expr*)expr->arguments->head->next->data);
        LLVMValueRef outSize = compileExpr(compiler, (Expr*)expr->arguments->head->next->next->data);
        LLVMValueRef outMtime = compileExpr(compiler, (Expr*)expr->arguments->head->next->next->next->data);
        LLVMValueRef outMode = compileExpr(compiler, (Expr*)expr->arguments->head->next->next->next->next->data);
        if (!path || !outKind || !outSize || !outMtime || !outMode) {
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        LLVMTypeRef i32 = LLVMInt32TypeInContext(compiler->context);
        LLVMTypeRef i64 = LLVMInt64TypeInContext(compiler->context);
        LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
        LLVMTypeRef i32ptr = LLVMPointerType(i32, 0);
        LLVMTypeRef i64ptr = LLVMPointerType(i64, 0);
        path = castValueToType(compiler, path, i8ptr);
        outKind = castValueToType(compiler, outKind, i32ptr);
        outSize = castValueToType(compiler, outSize, i64ptr);
        outMtime = castValueToType(compiler, outMtime, i64ptr);
        outMode = castValueToType(compiler, outMode, i32ptr);
        LLVMValueRef fn = getOrCreateTuaFsStatSimple(compiler);
        LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
        LLVMValueRef args5[5] = { path, outKind, outSize, outMtime, outMode };
        LLVMValueRef out = LLVMBuildCall2(compiler->builder, fnType, fn, args5, 5, "err");
        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
        return out;
    }

    if (builtinId == BI_TUA_FS_MKDIR) {
        unsigned got = expr->arguments ? (unsigned)expr->arguments->length : 0;
        if (got != 2) {
            emitDebug("tua_fs_mkdir expects 2 arguments\n");
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        LLVMValueRef path = compileExpr(compiler, (Expr*)expr->arguments->head->data);
        LLVMValueRef mode = compileExpr(compiler, (Expr*)expr->arguments->head->next->data);
        if (!path || !mode) {
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        LLVMTypeRef i32 = LLVMInt32TypeInContext(compiler->context);
        LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
        path = castValueToType(compiler, path, i8ptr);
        mode = castValueToType(compiler, mode, i32);
        LLVMValueRef fn = getOrCreateTuaFsMkdir(compiler);
        LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
        LLVMValueRef args2[2] = { path, mode };
        LLVMValueRef out = LLVMBuildCall2(compiler->builder, fnType, fn, args2, 2, "err");
        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
        return out;
    }

    if (builtinId == BI_TUA_FS_REALPATH_ALLOC) {
        unsigned got = expr->arguments ? (unsigned)expr->arguments->length : 0;
        if (got != 2) {
            emitDebug("tua_fs_realpath_alloc expects 2 arguments\n");
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        LLVMValueRef path = compileExpr(compiler, (Expr*)expr->arguments->head->data);
        LLVMValueRef outPath = compileExpr(compiler, (Expr*)expr->arguments->head->next->data);
        if (!path || !outPath) {
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
        LLVMTypeRef i8ptrptr = LLVMPointerType(i8ptr, 0);
        path = castValueToType(compiler, path, i8ptr);
        outPath = castValueToType(compiler, outPath, i8ptrptr);
        LLVMValueRef fn = getOrCreateTuaFsRealpathAlloc(compiler);
        LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
        LLVMValueRef args2[2] = { path, outPath };
        LLVMValueRef out = LLVMBuildCall2(compiler->builder, fnType, fn, args2, 2, "err");
        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
        return out;
    }

    if (builtinId == BI_TUA_FS_READDIR_ARR) {
        unsigned got = expr->arguments ? (unsigned)expr->arguments->length : 0;
        if (got != 2) {
            emitDebug("tua_fs_readdir_arr expects 2 arguments\n");
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        LLVMValueRef path = compileExpr(compiler, (Expr*)expr->arguments->head->data);
        LLVMValueRef outErr = compileExpr(compiler, (Expr*)expr->arguments->head->next->data);
        if (!path || !outErr) {
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        LLVMTypeRef i32 = LLVMInt32TypeInContext(compiler->context);
        LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
        LLVMTypeRef i32ptr = LLVMPointerType(i32, 0);
        path = castValueToType(compiler, path, i8ptr);
        outErr = castValueToType(compiler, outErr, i32ptr);
        LLVMValueRef fn = getOrCreateTuaFsReaddirArr(compiler);
        LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
        LLVMValueRef args2[2] = { path, outErr };
        LLVMValueRef out = LLVMBuildCall2(compiler->builder, fnType, fn, args2, 2, "arr");
        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
        return out;
    }

    if (builtinId == BI_TUA_TCP_ACCEPT_START_CL) {
        unsigned got = expr->arguments ? (unsigned)expr->arguments->length : 0;
        if (got != 4) {
            emitDebug("tua_tcp_accept_start_cl expects 4 arguments\n");
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        LLVMValueRef loopV = compileExpr(compiler, (Expr*)expr->arguments->head->data);
        LLVMValueRef lst = compileExpr(compiler, (Expr*)expr->arguments->head->next->data);
        LLVMValueRef cb = compileExpr(compiler, (Expr*)expr->arguments->head->next->next->data);
        LLVMValueRef outPtr = compileExpr(compiler, (Expr*)expr->arguments->head->next->next->next->data);
        if (!loopV || !lst || !cb || !outPtr) {
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
        LLVMTypeRef i8ptrptr = LLVMPointerType(i8ptr, 0);
        LLVMTypeRef closure = compilerGetClosureType(compiler);
        loopV = castValueToType(compiler, loopV, i8ptr);
        lst = castValueToType(compiler, lst, i8ptr);
        cb = castValueToType(compiler, cb, closure);
        outPtr = castValueToType(compiler, outPtr, i8ptrptr);
        LLVMValueRef fn = getOrCreateTuaTcpAcceptStartCl(compiler);
        LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
        LLVMValueRef args4[4] = { loopV, lst, cb, outPtr };
        LLVMValueRef out = LLVMBuildCall2(compiler->builder, fnType, fn, args4, 4, "err");
        dropTemporaryClosureArgs(compiler, expr->arguments, args4, 0);
        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
        return out;
    }

    if (builtinId == BI_TUA_TCP_ACCEPT_CANCEL_CL) {
        unsigned got = expr->arguments ? (unsigned)expr->arguments->length : 0;
        if (got != 1) {
            emitDebug("tua_tcp_accept_cancel_cl expects 1 argument\n");
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        LLVMValueRef h = compileExpr(compiler, (Expr*)expr->arguments->head->data);
        if (!h) {
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
        h = castValueToType(compiler, h, i8ptr);
        LLVMValueRef fn = getOrCreateTuaTcpAcceptCancelCl(compiler);
        LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
        LLVMBuildCall2(compiler->builder, fnType, fn, &h, 1, "");
        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
        return LLVMConstInt(LLVMInt32TypeInContext(compiler->context), 0, 0);
    }

    if (builtinId == BI_TUA_TCP_CONNECT_ASYNC_CL) {
        unsigned got = expr->arguments ? (unsigned)expr->arguments->length : 0;
        if (got != 6) {
            emitDebug("tua_tcp_connect_async_cl expects 6 arguments\n");
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        Expr* a0 = (Expr*)expr->arguments->head->data;
        Expr* a1 = (Expr*)expr->arguments->head->next->data;
        Expr* a2 = (Expr*)expr->arguments->head->next->next->data;
        Expr* a3 = (Expr*)expr->arguments->head->next->next->next->data;
        Expr* a4 = (Expr*)expr->arguments->head->next->next->next->next->data;
        Expr* a5 = (Expr*)expr->arguments->head->next->next->next->next->next->data;
        LLVMValueRef loopV = compileExpr(compiler, a0);
        LLVMValueRef wq = compileExpr(compiler, a1);
        LLVMValueRef host = compileExpr(compiler, a2);
        LLVMValueRef port = compileExpr(compiler, a3);
        LLVMValueRef deadline = compileExpr(compiler, a4);
        LLVMValueRef cb = compileExpr(compiler, a5);
        if (!loopV || !wq || !host || !port || !deadline || !cb) {
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
        LLVMTypeRef i64 = LLVMInt64TypeInContext(compiler->context);
        LLVMTypeRef closure = compilerGetClosureType(compiler);
        loopV = castValueToType(compiler, loopV, i8ptr);
        wq = castValueToType(compiler, wq, i8ptr);
        host = castValueToType(compiler, host, i8ptr);
        port = castValueToType(compiler, port, i8ptr);
        deadline = castValueToType(compiler, deadline, i64);
        cb = castValueToType(compiler, cb, closure);
        LLVMValueRef fn = getOrCreateTuaTcpConnectAsyncCl(compiler);
        LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
        LLVMValueRef args6[6] = { loopV, wq, host, port, deadline, cb };
        LLVMValueRef out = LLVMBuildCall2(compiler->builder, fnType, fn, args6, 6, "err");
        dropTemporaryClosureArgs(compiler, expr->arguments, args6, 0);
        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
        return out;
    }

    if (builtinId == BI_TUA_TCP_CONNECT_PORT_ASYNC_CL) {
        unsigned got = expr->arguments ? (unsigned)expr->arguments->length : 0;
        if (got != 6) {
            emitDebug("tua_tcp_connect_port_async_cl expects 6 arguments\n");
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        Expr* a0 = (Expr*)expr->arguments->head->data;
        Expr* a1 = (Expr*)expr->arguments->head->next->data;
        Expr* a2 = (Expr*)expr->arguments->head->next->next->data;
        Expr* a3 = (Expr*)expr->arguments->head->next->next->next->data;
        Expr* a4 = (Expr*)expr->arguments->head->next->next->next->next->data;
        Expr* a5 = (Expr*)expr->arguments->head->next->next->next->next->next->data;
        LLVMValueRef loopV = compileExpr(compiler, a0);
        LLVMValueRef wq = compileExpr(compiler, a1);
        LLVMValueRef host = compileExpr(compiler, a2);
        LLVMValueRef port = compileExpr(compiler, a3);
        LLVMValueRef deadline = compileExpr(compiler, a4);
        LLVMValueRef cb = compileExpr(compiler, a5);
        if (!loopV || !wq || !host || !port || !deadline || !cb) {
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
        LLVMTypeRef i32 = LLVMInt32TypeInContext(compiler->context);
        LLVMTypeRef i64 = LLVMInt64TypeInContext(compiler->context);
        LLVMTypeRef closure = compilerGetClosureType(compiler);
        loopV = castValueToType(compiler, loopV, i8ptr);
        wq = castValueToType(compiler, wq, i8ptr);
        host = castValueToType(compiler, host, i8ptr);
        port = castValueToType(compiler, port, i32);
        deadline = castValueToType(compiler, deadline, i64);
        cb = castValueToType(compiler, cb, closure);
        LLVMValueRef fn = getOrCreateTuaTcpConnectPortAsyncCl(compiler);
        LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
        LLVMValueRef args6[6] = { loopV, wq, host, port, deadline, cb };
        LLVMValueRef out = LLVMBuildCall2(compiler->builder, fnType, fn, args6, 6, "err");
        dropTemporaryClosureArgs(compiler, expr->arguments, args6, 0);
        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
        return out;
    }

    if (builtinId == BI_TUA_TCP_READ_ALLOC_ASYNC_CL) {
        unsigned got = expr->arguments ? (unsigned)expr->arguments->length : 0;
        if (got != 5) {
            emitDebug("tua_tcp_read_alloc_async_cl expects 5 arguments\n");
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        Expr* a0 = (Expr*)expr->arguments->head->data;
        Expr* a1 = (Expr*)expr->arguments->head->next->data;
        Expr* a2 = (Expr*)expr->arguments->head->next->next->data;
        Expr* a3 = (Expr*)expr->arguments->head->next->next->next->data;
        Expr* a4 = (Expr*)expr->arguments->head->next->next->next->next->data;
        LLVMValueRef loopV = compileExpr(compiler, a0);
        LLVMValueRef sock = compileExpr(compiler, a1);
        LLVMValueRef max = compileExpr(compiler, a2);
        LLVMValueRef deadline = compileExpr(compiler, a3);
        LLVMValueRef cb = compileExpr(compiler, a4);
        if (!loopV || !sock || !max || !deadline || !cb) {
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
        LLVMTypeRef i32 = LLVMInt32TypeInContext(compiler->context);
        LLVMTypeRef i64 = LLVMInt64TypeInContext(compiler->context);
        LLVMTypeRef closure = compilerGetClosureType(compiler);
        loopV = castValueToType(compiler, loopV, i8ptr);
        sock = castValueToType(compiler, sock, i8ptr);
        max = castValueToType(compiler, max, i32);
        deadline = castValueToType(compiler, deadline, i64);
        cb = castValueToType(compiler, cb, closure);
        LLVMValueRef fn = getOrCreateTuaTcpReadAllocAsyncCl(compiler);
        LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
        LLVMValueRef args5[5] = { loopV, sock, max, deadline, cb };
        LLVMValueRef out = LLVMBuildCall2(compiler->builder, fnType, fn, args5, 5, "err");
        dropTemporaryClosureArgs(compiler, expr->arguments, args5, 0);
        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
        return out;
    }

    if (builtinId == BI_TUA_TCP_WRITE_STR_ASYNC_CL) {
        unsigned got = expr->arguments ? (unsigned)expr->arguments->length : 0;
        if (got != 5) {
            emitDebug("tua_tcp_write_str_async_cl expects 5 arguments\n");
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        Expr* a0 = (Expr*)expr->arguments->head->data;
        Expr* a1 = (Expr*)expr->arguments->head->next->data;
        Expr* a2 = (Expr*)expr->arguments->head->next->next->data;
        Expr* a3 = (Expr*)expr->arguments->head->next->next->next->data;
        Expr* a4 = (Expr*)expr->arguments->head->next->next->next->next->data;
        LLVMValueRef loopV = compileExpr(compiler, a0);
        LLVMValueRef sock = compileExpr(compiler, a1);
        LLVMValueRef s = compileExpr(compiler, a2);
        LLVMValueRef deadline = compileExpr(compiler, a3);
        LLVMValueRef cb = compileExpr(compiler, a4);
        if (!loopV || !sock || !s || !deadline || !cb) {
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
        LLVMTypeRef i64 = LLVMInt64TypeInContext(compiler->context);
        LLVMTypeRef closure = compilerGetClosureType(compiler);
        loopV = castValueToType(compiler, loopV, i8ptr);
        sock = castValueToType(compiler, sock, i8ptr);
        s = castValueToType(compiler, s, i8ptr);
        deadline = castValueToType(compiler, deadline, i64);
        cb = castValueToType(compiler, cb, closure);
        LLVMValueRef fn = getOrCreateTuaTcpWriteStrAsyncCl(compiler);
        LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
        LLVMValueRef args5[5] = { loopV, sock, s, deadline, cb };
        LLVMValueRef out = LLVMBuildCall2(compiler->builder, fnType, fn, args5, 5, "err");
        dropTemporaryClosureArgs(compiler, expr->arguments, args5, 0);
        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
        return out;
    }

    if (builtinId == BI_TUA_FS_READFILE_ALLOC_ASYNC_CL || builtinId == BI_TUA_FS_STAT_ASYNC_CL ||
        builtinId == BI_TUA_FS_READDIR_ASYNC_CL) {
        unsigned got = expr->arguments ? (unsigned)expr->arguments->length : 0;
        if (got != 4) {
            const char* fnName = builtinId == BI_TUA_FS_READFILE_ALLOC_ASYNC_CL ? "tua_fs_readfile_alloc_async_cl"
                               : (builtinId == BI_TUA_FS_STAT_ASYNC_CL          ? "tua_fs_stat_async_cl"
                                                                                : "tua_fs_readdir_async_cl");
            emitDebug("%s expects 4 arguments\n", fnName);
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        LLVMValueRef loopV = compileExpr(compiler, (Expr*)expr->arguments->head->data);
        LLVMValueRef wq = compileExpr(compiler, (Expr*)expr->arguments->head->next->data);
        LLVMValueRef path = compileExpr(compiler, (Expr*)expr->arguments->head->next->next->data);
        LLVMValueRef cb = compileExpr(compiler, (Expr*)expr->arguments->head->next->next->next->data);
        if (!loopV || !wq || !path || !cb) {
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
        LLVMTypeRef closure = compilerGetClosureType(compiler);
        loopV = castValueToType(compiler, loopV, i8ptr);
        wq = castValueToType(compiler, wq, i8ptr);
        path = castValueToType(compiler, path, i8ptr);
        cb = castValueToType(compiler, cb, closure);
        LLVMValueRef fn = builtinId == BI_TUA_FS_READFILE_ALLOC_ASYNC_CL ? getOrCreateTuaFsReadfileAllocAsyncCl(compiler)
                         : (builtinId == BI_TUA_FS_STAT_ASYNC_CL          ? getOrCreateTuaFsStatAsyncCl(compiler)
                                                                          : getOrCreateTuaFsReaddirAsyncCl(compiler));
        LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
        LLVMValueRef args4[4] = { loopV, wq, path, cb };
        LLVMValueRef out = LLVMBuildCall2(compiler->builder, fnType, fn, args4, 4, "err");
        dropTemporaryClosureArgs(compiler, expr->arguments, args4, 0);
        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
        return out;
    }

    if (builtinId == BI_TUA_FS_WRITEFILE_STR_ASYNC_CL) {
        unsigned got = expr->arguments ? (unsigned)expr->arguments->length : 0;
        if (got != 5) {
            emitDebug("tua_fs_writefile_str_async_cl expects 5 arguments\n");
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        LLVMValueRef loopV = compileExpr(compiler, (Expr*)expr->arguments->head->data);
        LLVMValueRef wq = compileExpr(compiler, (Expr*)expr->arguments->head->next->data);
        LLVMValueRef path = compileExpr(compiler, (Expr*)expr->arguments->head->next->next->data);
        LLVMValueRef data = compileExpr(compiler, (Expr*)expr->arguments->head->next->next->next->data);
        LLVMValueRef cb = compileExpr(compiler, (Expr*)expr->arguments->head->next->next->next->next->data);
        if (!loopV || !wq || !path || !data || !cb) {
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
        LLVMTypeRef closure = compilerGetClosureType(compiler);
        loopV = castValueToType(compiler, loopV, i8ptr);
        wq = castValueToType(compiler, wq, i8ptr);
        path = castValueToType(compiler, path, i8ptr);
        data = castValueToType(compiler, data, i8ptr);
        cb = castValueToType(compiler, cb, closure);
        LLVMValueRef fn = getOrCreateTuaFsWritefileStrAsyncCl(compiler);
        LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
        LLVMValueRef args5[5] = { loopV, wq, path, data, cb };
        LLVMValueRef out = LLVMBuildCall2(compiler->builder, fnType, fn, args5, 5, "err");
        dropTemporaryClosureArgs(compiler, expr->arguments, args5, 0);
        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
        return out;
    }

    if (builtinId == BI_TUA_FS_STRING_ARRAY_FREE) {
        unsigned got = expr->arguments ? (unsigned)expr->arguments->length : 0;
        if (got != 1) {
            emitDebug("tua_fs_string_array_free expects 1 argument\n");
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        LLVMValueRef arr = compileExpr(compiler, (Expr*)expr->arguments->head->data);
        if (!arr) {
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        LLVMTypeRef arrType = compilerGetArrayType(compiler);
        arr = castValueToType(compiler, arr, arrType);
        LLVMValueRef fn = getOrCreateTuaFsStringArrayFree(compiler);
        LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
        LLVMBuildCall2(compiler->builder, fnType, fn, &arr, 1, "");
        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
        return LLVMConstInt(LLVMInt32TypeInContext(compiler->context), 0, 0);
    }

    if (!isPrintln && !isPrint) {
        // Closure value call: f(args...)
        VariableRef calleeVar = findVariableExpr(compiler, expr->callee);
        LLVMTypeRef closureType = compilerGetClosureType(compiler);
        if (calleeVar.value && calleeVar.type == closureType) {
            LLVMTypeRef fnType = compilerFindClosureSig(compiler, calleeVar.name, calleeVar.length);
            if (!fnType) {
                emitDebug("Missing closure signature for variable\n");
                if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                return NULL;
            }

            LLVMValueRef closureVal = loadLocalValue(compiler, calleeVar);
            if (!closureVal) {
                emitDebug("Failed to load closure value\n");
                if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                return NULL;
            }

            LLVMValueRef fnPtr = LLVMBuildExtractValue(compiler->builder, closureVal, 0, "fnptr");
            LLVMValueRef envPtr = LLVMBuildExtractValue(compiler->builder, closureVal, 1, "envptr");

            unsigned expected = LLVMCountParamTypes(fnType);
            unsigned got = expr->arguments ? (unsigned)expr->arguments->length : 0;
            if (expected != got + 1) {
                compilerErrorAtToken(
                    compiler,
                    expr->callee && expr->callee->type == EXPR_VARIABLE ? &((VariableExpr*)expr->callee)->name : &expr->base.token,
                    "argument count mismatch for call '%.*s': expected %u, got %u",
                    expr->callee && expr->callee->type == EXPR_VARIABLE ? ((VariableExpr*)expr->callee)->name.length : 0,
                    expr->callee && expr->callee->type == EXPR_VARIABLE ? ((VariableExpr*)expr->callee)->name.start : "",
                    expected > 0 ? (unsigned)(expected - 1) : 0u,
                    got
                );
                if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                return NULL;
            }

            LLVMTypeRef* paramTypes = NULL;
            if (expected > 0) {
                paramTypes = malloc(sizeof(LLVMTypeRef) * (size_t)expected);
                LLVMGetParamTypes(fnType, paramTypes);
            }

            LLVMValueRef* args = NULL;
            if (expected > 0) {
                args = malloc(sizeof(LLVMValueRef) * (size_t)expected);
                args[0] = castValueToType(compiler, envPtr, paramTypes[0]);
                ListNode* node = expr->arguments ? expr->arguments->head : NULL;
                for (unsigned i = 1; i < expected; i++) {
                    LLVMValueRef av = compileCallArgForParam(compiler, (Expr*)node->data, paramTypes[i]);
                    if (!av) {
                        if (paramTypes) free(paramTypes);
                        if (args) free(args);
                        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                        return NULL;
                    }
                    args[i] = av;
                    node = node->next;
                }
            }

            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            LLVMValueRef call = LLVMBuildCall2(compiler->builder, fnType, fnPtr, args, expected, callInstNameForFnType(fnType));
            LLVMTypeRef retType = LLVMGetReturnType(fnType);
            LLVMValueRef out = collapseMultiReturnByTypeIfNeeded(compiler, call, retType);
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;

            if (paramTypes) free(paramTypes);
            if (args) free(args);
            return out;
        }

        // Implicit object-method call inside the same `object`:
        // In `object O { fn a(){ b() } fn b(){...} }`, `b()` resolves to `O.b()`.
        // For self-calls `fn f(){ f() }`, prefer module/global resolution first
        // (so wrappers can call same-named `extern fn`), and only fall back to `O.f()` if needed.
        if (compiler && compiler->currentObjectPrefix) {
            int isSelf =
                compiler->currentObjectMethodName &&
                compiler->currentObjectMethodNameLen == callee->name.length &&
                memcmp(compiler->currentObjectMethodName, callee->name.start, (size_t)callee->name.length) == 0;
            if (!isSelf) {
                int ql = 0;
                char* q = mangleRawAndToken(compiler->currentObjectPrefix, compiler->currentObjectPrefixLen, &callee->name, &ql);
                if (q) {
                    LLVMValueRef objFunc = LLVMGetNamedFunction(compiler->module, q);
                    if (objFunc) {
                        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                        LLVMValueRef out = emitDirectFuncCall(compiler, objFunc, expr, callee->name.line);
                        free(q);
                        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                        return out;
                    }
                    free(q);
                }
            }
        }

        // Resolve functions with module-local qualified names taking precedence
        // over the generated entry `main` and over imported aliases.
        LLVMValueRef func = NULL;
        int foundQualified = 0;
        int foundAlias = 0;

        if (hasTypeArgs) {
            // Generic calls instantiate a function template; templates are registered with qualified names.
            if (compiler->currentModulePrefix) {
                int ql = 0;
                char* q = compilerQualifyToken(compiler, &callee->name, &ql);
                if (q) {
                    if (compilerFindGenericFuncTemplate(compiler, q, ql)) {
                        func = compilerInstantiateGenericFunc(compiler, q, ql, expr->typeArgs, &callee->name);
                        foundQualified = func != NULL;
                    }
                    free(q);
                }
            }
            if (!func) {
                SymbolAlias* a = compilerFindAlias(compiler, callee->name.start, callee->name.length);
                if (a && a->kind == ALIAS_FUNC) {
                    if (compilerFindGenericFuncTemplate(compiler, a->qualified, a->qualifiedLen)) {
                        func = compilerInstantiateGenericFunc(compiler, a->qualified, a->qualifiedLen, expr->typeArgs, &callee->name);
                        foundAlias = func != NULL;
                    }
                }
            }
            if (!func) {
                if (compilerFindGenericFuncTemplate(compiler, callee->name.start, callee->name.length)) {
                    func = compilerInstantiateGenericFunc(compiler, callee->name.start, callee->name.length, expr->typeArgs, &callee->name);
                }
            }
            if (!func && compiler && compiler->hadError) {
                if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                return NULL;
            }
            if (!func) {
                compilerErrorAtToken(compiler, &callee->name, "type arguments provided but '%.*s' is not a generic function", callee->name.length, callee->name.start);
                if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                return NULL;
            }
        } else {
        int genericTemplateSeen = 0;
        int genericInferFailed = 0;
        GenericFuncTemplate* seenTmpl = NULL;
        GenericInferDiag inferDiag;
        initGenericInferDiag(&inferDiag);

        // v0.5: type argument inference for generic calls when no explicit `<T>` is provided.
        // If a generic template exists for this name, infer from argument expressions and instantiate.
        if (compiler && expr && expr->arguments) {
            GenericFuncTemplate* tmpl = NULL;
            char* baseAlloc = NULL;

            if (compiler->currentModulePrefix) {
                int ql = 0;
                baseAlloc = compilerQualifyToken(compiler, &callee->name, &ql);
                if (baseAlloc) {
                    tmpl = compilerFindGenericFuncTemplate(compiler, baseAlloc, ql);
                    if (tmpl) {
                    } else {
                        free(baseAlloc);
                        baseAlloc = NULL;
                    }
                }
            }
            if (!tmpl) {
                SymbolAlias* a = compilerFindAlias(compiler, callee->name.start, callee->name.length);
                if (a && a->kind == ALIAS_FUNC) {
                    tmpl = compilerFindGenericFuncTemplate(compiler, a->qualified, a->qualifiedLen);
                }
            }
            if (!tmpl) {
                tmpl = compilerFindGenericFuncTemplate(compiler, callee->name.start, callee->name.length);
            }

            if (tmpl && tmpl->qualifiedName && tmpl->qualifiedNameLen > 0) {
                genericTemplateSeen = 1;
                seenTmpl = tmpl;
                List* inferred = inferTypeArgsForGenericCall(compiler, tmpl, expr, &inferDiag);
                if (inferred) {
                    func = compilerInstantiateGenericFunc(compiler, tmpl->qualifiedName, tmpl->qualifiedNameLen, inferred, &callee->name);
                    // inferred list elements are heap-allocated Type*; keep for compiler cache lifetime.
                    listFree(inferred);
                    if (!func) {
                        // Instantiation can fail either because inference produced an invalid set of args
                        // (in which case we should fall back to non-generic resolution if possible),
                        // or because of a real compiler error (e.g. bound not satisfied).
                        if (compiler && compiler->hadError) {
                            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                            return NULL;
                        }
                        genericInferFailed = 1;
                    }
                } else {
                    genericInferFailed = 1;
                }
            }
            if (baseAlloc) free(baseAlloc);
        }

        if (!func && compiler->currentModulePrefix) {
            int ql = 0;
            char* q = compilerQualifyToken(compiler, &callee->name, &ql);
            if (q) {
                func = LLVMGetNamedFunction(compiler->module, q);
                if (func) foundQualified = 1;
                free(q);
            }
        }

        if (!func) {
            SymbolAlias* a = compilerFindAlias(compiler, callee->name.start, callee->name.length);
            if (a && a->kind == ALIAS_FUNC) {
                func = LLVMGetNamedFunction(compiler->module, a->qualified);
                if (func) foundAlias = 1;
            }
        }

        char* name = tokenToCString(&callee->name);
        if (!func) {
            func = LLVMGetNamedFunction(compiler->module, name);
            // Prevent accidental recursion by calling the generated entry `main`
            // when the user did not define a module-local `main`.
            if (func && !foundQualified && !foundAlias && tokenEquals(&callee->name, "main")) {
                func = NULL;
            }
        }

        if (!func && genericTemplateSeen && genericInferFailed) {
            int expectedTypeParams = 0;
            if (seenTmpl && seenTmpl->decl && seenTmpl->decl->typeParams) {
                expectedTypeParams = seenTmpl->decl->typeParams->length;
            }
            switch (inferDiag.status) {
                case GEN_INFER_MISSING_TP:
                    if (inferDiag.tpName.start && inferDiag.tpName.length > 0) {
                        compilerErrorAtToken(
                            compiler,
                            &callee->name,
                            "cannot infer generic type arguments for '%.*s': type parameter '%.*s' is unconstrained; write '%.*s<...>(...)'",
                            callee->name.length,
                            callee->name.start,
                            inferDiag.tpName.length,
                            inferDiag.tpName.start,
                            callee->name.length,
                            callee->name.start
                        );
                    } else {
                        compilerErrorAtToken(
                            compiler,
                            &callee->name,
                            "cannot infer generic type arguments for '%.*s'%s%d%s; write '%.*s<...>(...)'",
                            callee->name.length,
                            callee->name.start,
                            expectedTypeParams > 0 ? " (expects " : "",
                            expectedTypeParams,
                            expectedTypeParams > 0 ? " type params)" : "",
                            callee->name.length,
                            callee->name.start
                        );
                    }
                    break;
                case GEN_INFER_CONFLICT:
                    if (inferDiag.tpName.start && inferDiag.tpName.length > 0) {
                        compilerErrorAtToken(
                            compiler,
                            &callee->name,
                            "cannot infer generic type arguments for '%.*s': conflicting inference for '%.*s' between argument %d and %d; write '%.*s<...>(...)'",
                            callee->name.length,
                            callee->name.start,
                            inferDiag.tpName.length,
                            inferDiag.tpName.start,
                            inferDiag.firstArgIndex,
                            inferDiag.secondArgIndex,
                            callee->name.length,
                            callee->name.start
                        );
                    } else {
                        compilerErrorAtToken(
                            compiler,
                            &callee->name,
                            "cannot infer generic type arguments for '%.*s': conflicting argument types; write '%.*s<...>(...)'",
                            callee->name.length,
                            callee->name.start,
                            callee->name.length,
                            callee->name.start
                        );
                    }
                    break;
                case GEN_INFER_ARGC_MISMATCH:
                    compilerErrorAtToken(
                        compiler,
                        &callee->name,
                        "argument count mismatch for generic call '%.*s': expected %d, got %d",
                        callee->name.length,
                        callee->name.start,
                        inferDiag.pc,
                        inferDiag.argc
                    );
                    break;
                case GEN_INFER_TRAIT_OBJECT_ARG:
                    compilerErrorAtToken(
                        compiler,
                        &callee->name,
                        "cannot infer generic type arguments for '%.*s': argument %d is a trait object; provide a concrete struct value or write '%.*s<...>(...)'",
                        callee->name.length,
                        callee->name.start,
                        inferDiag.secondArgIndex,
                        callee->name.length,
                        callee->name.start
                    );
                    break;
                case GEN_INFER_UNSUPPORTED_SHAPE:
                    if (inferDiag.tpName.start && inferDiag.tpName.length > 0) {
                        compilerErrorAtToken(
                            compiler,
                            &callee->name,
                            "cannot infer generic type arguments for '%.*s': type parameter '%.*s' appears in a nested position (e.g. Option<%.*s>) at argument %d; write '%.*s<...>(...)'",
                            callee->name.length,
                            callee->name.start,
                            inferDiag.tpName.length,
                            inferDiag.tpName.start,
                            inferDiag.tpName.length,
                            inferDiag.tpName.start,
                            inferDiag.secondArgIndex,
                            callee->name.length,
                            callee->name.start
                        );
                    } else {
                        compilerErrorAtToken(
                            compiler,
                            &callee->name,
                            "cannot infer generic type arguments for '%.*s': unsupported parameter type shape for inference; write '%.*s<...>(...)'",
                            callee->name.length,
                            callee->name.start,
                            callee->name.length,
                            callee->name.start
                        );
                    }
                    break;
                default:
                    if (expectedTypeParams > 0) {
                        compilerErrorAtToken(
                            compiler,
                            &callee->name,
                            "cannot infer generic type arguments for '%.*s' (expects %d type params); write '%.*s<...>(...)'",
                            callee->name.length,
                            callee->name.start,
                            expectedTypeParams,
                            callee->name.length,
                            callee->name.start
                        );
                    } else {
                        compilerErrorAtToken(
                            compiler,
                            &callee->name,
                            "cannot infer generic type arguments for '%.*s'; write '%.*s<...>(...)'",
                            callee->name.length,
                            callee->name.start,
                            callee->name.length,
                            callee->name.start
                        );
                    }
                    break;
            }
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            free(name);
            return NULL;
        }

        if (!func) {
            StructInfo* info = compilerResolveStructByToken(compiler, &callee->name);
            free(name);
            if (info) {
                LLVMValueRef out = emitStructConstructor(compiler, info, expr);
                if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                return out;
            }

            // Self-recursion fallback for object methods: if we are in `object O { fn f(){ f() } }`
            // and there's no global `f`, resolve to `O.f()` here.
            if (compiler && compiler->currentObjectPrefix) {
                int isSelf =
                    compiler->currentObjectMethodName &&
                    compiler->currentObjectMethodNameLen == callee->name.length &&
                    memcmp(compiler->currentObjectMethodName, callee->name.start, (size_t)callee->name.length) == 0;
                if (isSelf) {
                    int ql = 0;
                    char* q = mangleRawAndToken(compiler->currentObjectPrefix, compiler->currentObjectPrefixLen, &callee->name, &ql);
                    if (q) {
                        LLVMValueRef objFunc = LLVMGetNamedFunction(compiler->module, q);
                        if (objFunc) {
                            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                            LLVMValueRef out = emitDirectFuncCall(compiler, objFunc, expr, callee->name.line);
                            free(q);
                            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                            return out;
                        }
                        free(q);
                    }
                }
            }

            // If a generic template exists for this name, require explicit type arguments in v0.
            int hasTemplate = 0;
            if (compiler->currentModulePrefix) {
                int ql = 0;
                char* q = compilerQualifyToken(compiler, &callee->name, &ql);
                if (q) {
                    if (compilerFindGenericFuncTemplate(compiler, q, ql)) hasTemplate = 1;
                    free(q);
                }
            }
            if (!hasTemplate) {
                SymbolAlias* a = compilerFindAlias(compiler, callee->name.start, callee->name.length);
                if (a && a->kind == ALIAS_FUNC) {
                    if (compilerFindGenericFuncTemplate(compiler, a->qualified, a->qualifiedLen)) hasTemplate = 1;
                }
            }
            if (!hasTemplate) {
                if (compilerFindGenericFuncTemplate(compiler, callee->name.start, callee->name.length)) hasTemplate = 1;
            }
            if (hasTemplate) {
                compilerErrorAtToken(
                    compiler,
                    &callee->name,
                    "generic function '%.*s' requires explicit type arguments (e.g. %.*s<int>(...))",
                    callee->name.length,
                    callee->name.start,
                    callee->name.length,
                    callee->name.start
                );
                if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                return NULL;
            }

            compilerErrorAtToken(compiler, &callee->name, "undefined function '%.*s'",
                callee->name.length, callee->name.start);
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        free(name);
        }

        LLVMTypeRef funcType = LLVMGlobalGetValueType(func);
        unsigned expected = LLVMCountParamTypes(funcType);
        unsigned got = expr->arguments ? (unsigned)expr->arguments->length : 0;
        if (expected != got) {
            compilerErrorAtToken(
                compiler,
                &callee->name,
                "argument count mismatch for call '%.*s': expected %u, got %u",
                callee->name.length,
                callee->name.start,
                expected,
                got
            );
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }

        LLVMTypeRef* paramTypes = NULL;
        if (expected > 0) {
            paramTypes = malloc(sizeof(LLVMTypeRef) * (size_t)expected);
            LLVMGetParamTypes(funcType, paramTypes);
        }

        LLVMValueRef* args = NULL;
        if (expected > 0) {
            args = malloc(sizeof(LLVMValueRef) * (size_t)expected);
            ListNode* node = expr->arguments->head;
            for (unsigned i = 0; i < expected; i++) {
                LLVMValueRef av = compileCallArgForParam(compiler, (Expr*)node->data, paramTypes[i]);
                if (!av) {
                    if (paramTypes) free(paramTypes);
                    if (args) free(args);
                    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                    return NULL;
                }
                args[i] = av;
                node = node->next;
            }
        }

        LLVMValueRef call = LLVMBuildCall2(compiler->builder, funcType, func, args, expected, callInstNameForFnType(funcType));
        if (paramTypes) free(paramTypes);
        if (args) free(args);
        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
        LLVMValueRef out = collapseMultiReturnIfNeeded(compiler, func, call);
        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
        return out;
    }

    if (!expr->arguments || expr->arguments->length <= 0) {
        compilerErrorAtToken(compiler, &callee->name, "%.*s expects at least 1 argument", callee->name.length, callee->name.start);
        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
        return NULL;
    }

    // Default: `print(x)` / `println(x)`
    if (expr->arguments->length == 1) {
        LLVMValueRef argValue = compileExpr(compiler, (Expr*)expr->arguments->head->data);
        if (!argValue) {
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        LLVMValueRef out = emitPrintValue(compiler, argValue, isPrintln ? 1 : 0);
        if (!out) {
            emitDebug("Unsupported print argument type\n");
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
        return out;
    }

    // Formatted printing: `println("x {} y {}", a, b)` using `{}` placeholders.
    // - Only supported when the first argument is a string literal.
    // - Placeholder count must match the remaining argument count.
    Expr* fmtAst = (Expr*)expr->arguments->head->data;
    fmtAst = unwrapGroupingExpr(fmtAst);
    if (!fmtAst || fmtAst->type != EXPR_LITERAL) {
        compilerErrorAtToken(compiler, &callee->name, "%.*s with multiple arguments requires a string literal format", callee->name.length, callee->name.start);
        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
        return NULL;
    }
    LiteralExpr* lit = (LiteralExpr*)fmtAst;
    if (lit->value.type != TOKEN_STRING_LITERAL) {
        compilerErrorAtToken(compiler, &callee->name, "%.*s with multiple arguments requires a string literal format", callee->name.length, callee->name.start);
        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
        return NULL;
    }

    char* fmt = dupStringLiteralToken(lit->value);
    int placeholders = countBracePlaceholders(fmt);
    int got = expr->arguments->length;
    int expectedArgs = 1 + placeholders;
    if (got != expectedArgs) {
        compilerErrorAtToken(
            compiler,
            &callee->name,
            "format placeholder count mismatch for '%.*s': expected %d args, got %d",
            callee->name.length,
            callee->name.start,
            expectedArgs,
            got
        );
        free(fmt);
        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
        return NULL;
    }

    // Emit: segments and values (no newline), then optional trailing newline.
    ListNode* argNode = expr->arguments->head ? expr->arguments->head->next : NULL; // start from 2nd arg
    const char* p = fmt;
    while (p && *p) {
        const char* m = strstr(p, "{}");
        if (!m) break;
        if (m > p) {
            int segLen = (int)(m - p);
            char* seg = malloc((size_t)segLen + 1);
            memcpy(seg, p, (size_t)segLen);
            seg[segLen] = '\0';
            LLVMValueRef segV = LLVMBuildGlobalStringPtr(compiler->builder, seg, "fseg");
            free(seg);
            if (!emitPrintValue(compiler, segV, 0)) {
                free(fmt);
                if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                return NULL;
            }
        }
        if (!argNode) {
            free(fmt);
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        LLVMValueRef av = compileExpr(compiler, (Expr*)argNode->data);
        if (!av) {
            free(fmt);
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        if (!emitPrintValue(compiler, av, 0)) {
            emitDebug("Unsupported print argument type\n");
            free(fmt);
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        argNode = argNode->next;
        p = m + 2;
    }
    if (p && *p) {
        int segLen = (int)strlen(p);
        if (segLen > 0) {
            LLVMValueRef tailV = LLVMBuildGlobalStringPtr(compiler->builder, p, "fseg");
            if (!emitPrintValue(compiler, tailV, 0)) {
                free(fmt);
                if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                return NULL;
            }
        }
    }
    free(fmt);

    if (isPrintln) {
        LLVMValueRef nlV = LLVMBuildGlobalStringPtr(compiler->builder, "\n", "nl");
        (void)emitPrintValue(compiler, nlV, 0);
    }
    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
    return LLVMConstInt(LLVMInt32TypeInContext(compiler->context), 0, 0);
    // LLVMBuilderRef builder = compiler->builder;
    // Block* block = compiler->current;

    // // Lookup function value
    // VariableRef func = findVariableExpr(compiler, expr->callee);
    // if (!func.value) {
    //     emitDebug("Undefined function");
    //     return NULL;
    // }

    // // // Compile arguments
    // // LLVMValueRef* args = NULL;
    // // int argCount = 0;
    // // if (expr->arguments != NULL) {
    // //     args = malloc(sizeof(LLVMValueRef) * expr->arguments->length);
    // //     ListNode* arg = expr->arguments->head;
    // //     while (arg != NULL) {
    // //         args[argCount++] = emitExpr(compiler, (Expr*)arg->data);
    // //         arg = arg->next;
    // //     }
    // // }

    // // // Build call instruction
    // // LLVMValueRef call = LLVMBuildCall2(
    // //     builder,
    // //     LLVMTypeOf(func.value),
    // //     func.value,
    // //     args,
    // //     argCount,
    // //     "call"
    // // );

    // // // Cleanup
    // // if (args) {
    // //     free(args);
    // // }

    // // 使用栈内存存储参数，避免频繁的堆内存分配
    // LLVMValueRef stackArgs[64];  // 支持最多64个参数
    // LLVMValueRef* args = stackArgs;
    // int argCount = 0;

    // if (expr->arguments) {
    //     if (expr->arguments->length > 64) {
    //         emitDebug("Too many arguments (max 64)");
    //         return NULL;
    //     }

    //     // 编译参数
    //     for (ListNode* arg = expr->arguments->head; arg != NULL; arg = arg->next) {
    //         LLVMValueRef argValue = emitExpr(compiler, (Expr*)arg->data);
    //         if (!argValue) {
    //             emitDebug("Failed to compile argument");
    //             return NULL;
    //         }
    //         args[argCount++] = argValue;
    //     }
    // }

    // // Build call instruction
    // LLVMValueRef call = LLVMBuildCall2(
    //     builder,
    //     LLVMTypeOf(func.value),
    //     func.value,
    //     args,
    //     argCount,
    //     "call"
    // );

    // return call;
}

LLVMValueRef emitGuardExpr(Compiler* compiler, GuardExpr* expr) {
    if (!compiler || !expr || !expr->call || !expr->onErr) return NULL;
    LLVMBuilderRef builder = compiler->builder;
    if (!builder || !compiler->current || !compiler->current->func) return NULL;

    // Evaluate the call once and keep all return values (multi-return struct).
    LLVMValueRef callValue = compileExprMulti(compiler, expr->call);
    if (!callValue) return NULL;

    LLVMTypeRef retTy = LLVMTypeOf(callValue);
    if (LLVMGetTypeKind(retTy) != LLVMStructTypeKind) {
        compilerErrorAtToken(compiler, &expr->qmark, "`? { ... }` requires a multi-return call (>=2 values) with last `int` error code");
        return NULL;
    }

    unsigned n = LLVMCountStructElementTypes(retTy);
    if (n < 2) {
        compilerErrorAtToken(compiler, &expr->qmark, "`? { ... }` requires a call returning at least 2 values (value, err)");
        return NULL;
    }

    unsigned errIndex = n - 1;
    LLVMTypeRef errTy = LLVMStructGetTypeAtIndex(retTy, errIndex);
    LLVMTypeRef i32 = LLVMInt32TypeInContext(compiler->context);
    if (errTy != i32) {
        compilerErrorAtToken(compiler, &expr->qmark, "`? { ... }` requires the last return value to be `int` (error code)");
        return NULL;
    }

    LLVMValueRef errVal = LLVMBuildExtractValue(builder, callValue, errIndex, "guard.err");
    LLVMValueRef isErr = LLVMBuildICmp(builder, LLVMIntNE, errVal, LLVMConstInt(i32, 0, 1), "guard.is_err");

    LLVMValueRef func = compiler->current->func;
    LLVMBasicBlockRef errBB = LLVMAppendBasicBlockInContext(compiler->context, func, "guard.err.bb");
    LLVMBasicBlockRef okBB = LLVMAppendBasicBlockInContext(compiler->context, func, "guard.ok.bb");
    LLVMBuildCondBr(builder, isErr, errBB, okBB);

    // Error path: run the guard block (must not fall through).
    LLVMPositionBuilderAtEnd(builder, errBB);
    compileBlockStmt(compiler, expr->onErr);
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(builder))) {
        // Parser validates "must interrupt"; keep IR valid even if it slips through.
        LLVMBuildUnreachable(builder);
    }

    // Success path: drop trailing `err` and yield remaining return values.
    LLVMPositionBuilderAtEnd(builder, okBB);
    unsigned okCount = errIndex; // n-1
    if (!compiler->wantMultiValue || okCount <= 1) {
        return LLVMBuildExtractValue(builder, callValue, 0, "guard.ok");
    }

    LLVMTypeRef* elemTys = (LLVMTypeRef*)malloc(sizeof(LLVMTypeRef) * (size_t)okCount);
    for (unsigned i = 0; i < okCount; i++) {
        elemTys[i] = LLVMStructGetTypeAtIndex(retTy, i);
    }
    LLVMTypeRef outTy = LLVMStructTypeInContext(compiler->context, elemTys, okCount, 0);
    free(elemTys);

    LLVMValueRef out = LLVMGetUndef(outTy);
    for (unsigned i = 0; i < okCount; i++) {
        LLVMValueRef v = LLVMBuildExtractValue(builder, callValue, i, "guard.mv");
        out = LLVMBuildInsertValue(builder, out, v, i, "guard.mvo");
    }
    return out;
}
