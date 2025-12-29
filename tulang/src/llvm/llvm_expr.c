#include "llvm.h"
#include "compiler.h"
#include "debug.h"

static LLVMValueRef castToType(Compiler* compiler, LLVMValueRef value, LLVMTypeRef targetType);

static char* dupTokenCString(const Token* token) {
    if (!token || !token->start || token->length <= 0) return NULL;
    char* s = malloc((size_t)token->length + 1);
    memcpy(s, token->start, (size_t)token->length);
    s[token->length] = '\0';
    return s;
}

static int32_t parseIntToken(Token token) {
    char* s = dupTokenCString(&token);
    if (!s) return 0;
    long v = strtol(s, NULL, 10);
    free(s);
    return (int32_t)v;
}

static int64_t parseLongToken(Token token) {
    char* s = dupTokenCString(&token);
    if (!s) return 0;
    long long v = strtoll(s, NULL, 10);
    free(s);
    return (int64_t)v;
}

static double parseDoubleToken(Token token) {
    char* s = dupTokenCString(&token);
    if (!s) return 0.0;
    double v = strtod(s, NULL);
    free(s);
    return v;
}

static char* dupStringLiteral(Token token) {
    if (!token.start || token.length < 2) return dupTokenCString(&token);
    // Best-effort: strip surrounding quotes without unescaping.
    int innerLen = token.length - 2;
    if (innerLen < 0) innerLen = 0;
    char* s = malloc((size_t)innerLen + 1);
    memcpy(s, token.start + 1, (size_t)innerLen);
    s[innerLen] = '\0';
    return s;
}

static LLVMTypeRef lambdaTypeToLLVMType(Compiler* compiler, Type* type, bool defaultToVoid) {
    if (type == NULL) {
        return defaultToVoid ? LLVMVoidTypeInContext(compiler->context)
                             : LLVMInt32TypeInContext(compiler->context);
    }

    switch (type->kind) {
        case TYPE_INT:
            return LLVMInt32TypeInContext(compiler->context);
        case TYPE_LONG:
            return LLVMInt64TypeInContext(compiler->context);
        case TYPE_DOUBLE:
            return LLVMDoubleTypeInContext(compiler->context);
        case TYPE_BOOL:
            return LLVMInt1TypeInContext(compiler->context);
        case TYPE_STRING:
            return LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
        case TYPE_NAMED: {
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
                LLVMTypeRef innerTy = lambdaTypeToLLVMType(compiler, inner, false);
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
            return info->type;
        }
        case TYPE_REF: {
            LLVMTypeRef inner = lambdaTypeToLLVMType(compiler, type->inner, false);
            return LLVMPointerType(inner, 0);
        }
        case TYPE_FUNC:
            return compilerGetClosureType(compiler);
        case TYPE_VOID:
            return LLVMVoidTypeInContext(compiler->context);
        default:
            return defaultToVoid ? LLVMVoidTypeInContext(compiler->context)
                                 : LLVMInt32TypeInContext(compiler->context);
    }
}

static LLVMValueRef getOrCreateMalloc(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "malloc");
    if (existing) return existing;
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef i64 = LLVMInt64TypeInContext(compiler->context);
    LLVMTypeRef fnType = LLVMFunctionType(i8ptr, &i64, 1, 0);
    return LLVMAddFunction(compiler->module, "malloc", fnType);
}

static LLVMValueRef getOrCreateStrcmp(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "strcmp");
    if (existing) return existing;
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef params[2] = { i8ptr, i8ptr };
    LLVMTypeRef fnType = LLVMFunctionType(LLVMInt32TypeInContext(compiler->context), params, 2, 0);
    return LLVMAddFunction(compiler->module, "strcmp", fnType);
}

static LLVMValueRef getOrCreateTuaStrConcat(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_str_concat");
    if (existing) return existing;
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef params[2] = { i8ptr, i8ptr };
    LLVMTypeRef fnType = LLVMFunctionType(i8ptr, params, 2, 0);
    return LLVMAddFunction(compiler->module, "tua_str_concat", fnType);
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
    LLVMBuilderRef builder = compiler->builder;
    LLVMContextRef context = compiler->context;
    LLVMTypeRef vt = compilerGetTuaValueType(compiler);
    LLVMValueRef v = LLVMGetUndef(vt);
    LLVMValueRef tagV = LLVMConstInt(LLVMInt32TypeInContext(context), (unsigned)tag, 0);
    LLVMValueRef payload = payloadI64 ? payloadI64 : LLVMConstInt(LLVMInt64TypeInContext(context), 0, 0);
    v = LLVMBuildInsertValue(builder, v, tagV, 0, "t_tag");
    v = LLVMBuildInsertValue(builder, v, payload, 1, "t_payload");
    return v;
}

static LLVMValueRef tuaValueFromKey(Compiler* compiler, LLVMValueRef key) {
    LLVMBuilderRef builder = compiler->builder;
    LLVMContextRef context = compiler->context;
    LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
    LLVMTypeRef t = LLVMTypeOf(key);
    LLVMTypeKind k = LLVMGetTypeKind(t);

    if (k == LLVMIntegerTypeKind) {
        unsigned bits = LLVMGetIntTypeWidth(t);
        LLVMValueRef k64 = key;
        if (bits < 64) k64 = LLVMBuildSExt(builder, key, i64, "k_sext");
        else if (bits > 64) k64 = LLVMBuildTrunc(builder, key, i64, "k_trunc");
        return tuaValueMake(compiler, TUA_VAL_LONG, k64); // normalize int/long to long key
    }
    if (k == LLVMPointerTypeKind) {
        LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(context), 0);
        if (t != i8ptr) {
            error("Map key pointer type must be string (i8*)\n");
            return NULL;
        }
        LLVMValueRef p64 = LLVMBuildPtrToInt(builder, key, i64, "k_ptr");
        return tuaValueMake(compiler, TUA_VAL_STRING, p64);
    }

    error("Map key must be int/long/string\n");
    return NULL;
}

static LLVMValueRef tuaValueFromValue(Compiler* compiler, LLVMValueRef value) {
    LLVMBuilderRef builder = compiler->builder;
    LLVMContextRef context = compiler->context;
    LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
    LLVMTypeRef t = LLVMTypeOf(value);
    LLVMTypeKind k = LLVMGetTypeKind(t);

    if (k == LLVMIntegerTypeKind) {
        unsigned bits = LLVMGetIntTypeWidth(t);
        if (bits == 1) {
            LLVMValueRef b64 = LLVMBuildZExt(builder, value, i64, "b64");
            return tuaValueMake(compiler, TUA_VAL_BOOL, b64);
        }
        if (bits == 32) {
            LLVMValueRef i64v = LLVMBuildSExt(builder, value, i64, "i64");
            return tuaValueMake(compiler, TUA_VAL_INT, i64v);
        }
        if (bits == 64) {
            return tuaValueMake(compiler, TUA_VAL_LONG, value);
        }
        LLVMValueRef i64v = LLVMBuildSExt(builder, value, i64, "i64x");
        return tuaValueMake(compiler, TUA_VAL_LONG, i64v);
    }

    if (k == LLVMDoubleTypeKind) {
        LLVMValueRef bits = LLVMBuildBitCast(builder, value, i64, "dblbits");
        return tuaValueMake(compiler, TUA_VAL_DOUBLE, bits);
    }

    if (k == LLVMPointerTypeKind) {
        LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(context), 0);
        LLVMValueRef ptr = value;
        int tag = TUA_VAL_PTR;
        if (t == i8ptr) {
            tag = TUA_VAL_STRING;
        } else {
            ptr = LLVMBuildBitCast(builder, value, i8ptr, "p_i8p");
        }
        LLVMValueRef bits = LLVMBuildPtrToInt(builder, ptr, i64, "p64");
        return tuaValueMake(compiler, tag, bits);
    }

    // For aggregate values, box into heap and store pointer.
    if (k == LLVMStructTypeKind) {
        LLVMValueRef mallocFn = getOrCreateMalloc(compiler);
        LLVMValueRef sizeV = LLVMSizeOf(t);
        LLVMValueRef raw = LLVMBuildCall2(builder, LLVMGlobalGetValueType(mallocFn), mallocFn, &sizeV, 1, "malloc");
        LLVMValueRef cell = LLVMBuildBitCast(builder, raw, LLVMPointerType(t, 0), "cell");
        LLVMBuildStore(builder, value, cell);
        LLVMValueRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(context), 0);
        LLVMValueRef p = LLVMBuildBitCast(builder, cell, i8ptr, "cell_i8");
        LLVMValueRef bits = LLVMBuildPtrToInt(builder, p, i64, "cell64");
        return tuaValueMake(compiler, TUA_VAL_PTR, bits);
    }

    error("Unsupported map value type\n");
    return NULL;
}

static LLVMValueRef getOrCreateTuaMapNew(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_map_new");
    if (existing) return existing;
    LLVMTypeRef mapType = compilerGetMapType(compiler);
    LLVMTypeRef fnType = LLVMFunctionType(mapType, NULL, 0, 0);
    return LLVMAddFunction(compiler->module, "tua_map_new", fnType);
}

static LLVMValueRef getOrCreateTuaMapGet(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_map_get");
    if (existing) return existing;
    LLVMTypeRef mapType = compilerGetMapType(compiler);
    LLVMTypeRef vt = compilerGetTuaValueType(compiler);
    LLVMTypeRef params[2] = { mapType, vt };
    LLVMTypeRef fnType = LLVMFunctionType(vt, params, 2, 0);
    return LLVMAddFunction(compiler->module, "tua_map_get", fnType);
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

static LLVMValueRef getOrCreateTuaMapSet(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_map_set");
    if (existing) return existing;
    LLVMTypeRef mapType = compilerGetMapType(compiler);
    LLVMTypeRef vt = compilerGetTuaValueType(compiler);
    LLVMTypeRef params[3] = { mapType, vt, vt };
    LLVMTypeRef fnType = LLVMFunctionType(LLVMVoidTypeInContext(compiler->context), params, 3, 0);
    return LLVMAddFunction(compiler->module, "tua_map_set", fnType);
}

static LLVMValueRef getOrCreateTuaValueToInt(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_value_to_int");
    if (existing) return existing;
    LLVMTypeRef vt = compilerGetTuaValueType(compiler);
    LLVMTypeRef params[1] = { vt };
    LLVMTypeRef fnType = LLVMFunctionType(LLVMInt32TypeInContext(compiler->context), params, 1, 0);
    return LLVMAddFunction(compiler->module, "tua_value_to_int", fnType);
}

static LLVMValueRef getOrCreateTuaValueToLong(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_value_to_long");
    if (existing) return existing;
    LLVMTypeRef vt = compilerGetTuaValueType(compiler);
    LLVMTypeRef params[1] = { vt };
    LLVMTypeRef fnType = LLVMFunctionType(LLVMInt64TypeInContext(compiler->context), params, 1, 0);
    return LLVMAddFunction(compiler->module, "tua_value_to_long", fnType);
}

static LLVMValueRef getOrCreateTuaValueToDouble(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_value_to_double");
    if (existing) return existing;
    LLVMTypeRef vt = compilerGetTuaValueType(compiler);
    LLVMTypeRef params[1] = { vt };
    LLVMTypeRef fnType = LLVMFunctionType(LLVMDoubleTypeInContext(compiler->context), params, 1, 0);
    return LLVMAddFunction(compiler->module, "tua_value_to_double", fnType);
}

static LLVMValueRef getOrCreateTuaValueToBool(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_value_to_bool");
    if (existing) return existing;
    LLVMTypeRef vt = compilerGetTuaValueType(compiler);
    LLVMTypeRef params[1] = { vt };
    LLVMTypeRef fnType = LLVMFunctionType(LLVMInt32TypeInContext(compiler->context), params, 1, 0);
    return LLVMAddFunction(compiler->module, "tua_value_to_bool", fnType);
}

static LLVMValueRef getOrCreateTuaValueToString(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_value_to_string");
    if (existing) return existing;
    LLVMTypeRef vt = compilerGetTuaValueType(compiler);
    LLVMTypeRef params[1] = { vt };
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef fnType = LLVMFunctionType(i8ptr, params, 1, 0);
    return LLVMAddFunction(compiler->module, "tua_value_to_string", fnType);
}

static LLVMValueRef castFromTuaValue(Compiler* compiler, LLVMValueRef value, LLVMTypeRef targetType) {
    if (!compiler || !value || !targetType) return value;
    LLVMTypeRef vt = compilerGetTuaValueType(compiler);
    if (LLVMTypeOf(value) != vt) return value;

    LLVMBuilderRef builder = compiler->builder;
    LLVMContextRef context = compiler->context;
    LLVMTypeKind dstKind = LLVMGetTypeKind(targetType);

    if (dstKind == LLVMIntegerTypeKind) {
        unsigned bits = LLVMGetIntTypeWidth(targetType);
        if (bits == 1) {
            LLVMValueRef fn = getOrCreateTuaValueToBool(compiler);
            LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
            LLVMValueRef b32 = LLVMBuildCall2(builder, fnType, fn, &value, 1, "b32");
            return LLVMBuildTrunc(builder, b32, targetType, "b");
        }
        if (bits <= 32) {
            LLVMValueRef fn = getOrCreateTuaValueToInt(compiler);
            LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
            LLVMValueRef i32 = LLVMBuildCall2(builder, fnType, fn, &value, 1, "i32");
            if (bits < 32) return LLVMBuildTrunc(builder, i32, targetType, "itr");
            if (bits > 32) return LLVMBuildSExt(builder, i32, targetType, "isx");
            return i32;
        }
        if (bits == 64) {
            LLVMValueRef fn = getOrCreateTuaValueToLong(compiler);
            LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
            return LLVMBuildCall2(builder, fnType, fn, &value, 1, "i64");
        }
    }

    if (dstKind == LLVMDoubleTypeKind) {
        LLVMValueRef fn = getOrCreateTuaValueToDouble(compiler);
        LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
        return LLVMBuildCall2(builder, fnType, fn, &value, 1, "d");
    }

    if (dstKind == LLVMPointerTypeKind) {
        LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(context), 0);
        if (targetType == i8ptr) {
            LLVMValueRef fn = getOrCreateTuaValueToString(compiler);
            LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
            return LLVMBuildCall2(builder, fnType, fn, &value, 1, "s");
        }
    }

    return value;
}

static int isOptionLLVMType(LLVMTypeRef t) {
    if (!t) return 0;
    if (LLVMGetTypeKind(t) != LLVMStructTypeKind) return 0;
    if (LLVMCountStructElementTypes(t) != 2) return 0;
    LLVMTypeRef f0 = LLVMStructGetTypeAtIndex(t, 0);
    if (LLVMGetTypeKind(f0) != LLVMIntegerTypeKind) return 0;
    return LLVMGetIntTypeWidth(f0) == 1;
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
    if (k == LLVMDoubleTypeKind) return 1;
    if (k != LLVMIntegerTypeKind) return 0;
    return LLVMGetIntTypeWidth(t) != 1;
}

static int typedMapKeyCompatible(Compiler* compiler, LLVMTypeRef expectedKeyTy, LLVMValueRef keyVal) {
    if (!compiler || !expectedKeyTy || !keyVal) return 0;
    LLVMTypeRef actualTy = LLVMTypeOf(keyVal);
    if (isTuaValueLLVMType(compiler, actualTy)) return 0;

    if (isStringLLVMType(compiler, expectedKeyTy)) {
        return isStringLLVMType(compiler, actualTy);
    }

    // int/long keys: accept any non-bool integer.
    return isNumericLLVMType(actualTy);
}

static int typedMapValueCompatible(Compiler* compiler, LLVMTypeRef expectedValTy, LLVMValueRef rawVal) {
    if (!compiler || !expectedValTy || !rawVal) return 0;
    LLVMTypeRef actualTy = LLVMTypeOf(rawVal);
    if (isTuaValueLLVMType(compiler, actualTy)) return 0;

    if (isStringLLVMType(compiler, expectedValTy)) {
        return isStringLLVMType(compiler, actualTy);
    }
    if (isBoolLLVMType(expectedValTy)) {
        return isBoolLLVMType(actualTy);
    }
    if (LLVMGetTypeKind(expectedValTy) == LLVMDoubleTypeKind) {
        return isNumericLLVMType(actualTy);
    }
    if (LLVMGetTypeKind(expectedValTy) == LLVMIntegerTypeKind && LLVMGetIntTypeWidth(expectedValTy) != 1) {
        return isNumericLLVMType(actualTy);
    }
    return 0;
}

static int nameSetContains(List* set, const char* name, int len) {
    if (!set) return 0;
    for (ListNode* n = set->head; n != NULL; n = n->next) {
        Token* t = (Token*)n->data;
        if (!t) continue;
        if (t->length != len) continue;
        if (memcmp(t->start, name, (size_t)len) == 0) return 1;
    }
    return 0;
}

static void nameSetAdd(List* set, const char* name, int len) {
    if (!set || !name || len <= 0) return;
    if (nameSetContains(set, name, len)) return;
    Token* t = malloc(sizeof(Token));
    t->type = TOKEN_IDENTIFIER;
    t->start = name;
    t->length = len;
    t->line = 0;
    t->hasDot = 0;
    listAppend(set, t);
}

static void freeTokenSet(List* set) {
    if (!set) return;
    for (ListNode* n = set->head; n != NULL; n = n->next) {
        free(n->data);
    }
    listFree(set);
}

static void collectLambdaLocalsStmt(List* locals, Stmt* stmt);
static void collectLambdaLocalsExpr(List* locals, Expr* expr) {
    if (!expr) return;
    if (expr->type == EXPR_LAMBDA) return; // stop at nested lambda
    switch (expr->type) {
        case EXPR_BINARY: {
            BinaryExpr* b = (BinaryExpr*)expr;
            collectLambdaLocalsExpr(locals, b->left);
            collectLambdaLocalsExpr(locals, b->right);
            break;
        }
        case EXPR_UNARY: {
            UnaryExpr* u = (UnaryExpr*)expr;
            collectLambdaLocalsExpr(locals, u->right);
            break;
        }
        case EXPR_GROUPING: {
            collectLambdaLocalsExpr(locals, ((GroupingExpr*)expr)->expression);
            break;
        }
        case EXPR_CALL: {
            CallExpr* c = (CallExpr*)expr;
            collectLambdaLocalsExpr(locals, c->callee);
            for (ListNode* n = c->arguments ? c->arguments->head : NULL; n != NULL; n = n->next) {
                collectLambdaLocalsExpr(locals, (Expr*)n->data);
            }
            break;
        }
        case EXPR_ASSIGN: {
            AssignExpr* a = (AssignExpr*)expr;
            collectLambdaLocalsExpr(locals, a->value);
            break;
        }
        case EXPR_GET: {
            GetExpr* g = (GetExpr*)expr;
            collectLambdaLocalsExpr(locals, g->object);
            break;
        }
        case EXPR_SET: {
            SetExpr* s = (SetExpr*)expr;
            collectLambdaLocalsExpr(locals, s->object);
            collectLambdaLocalsExpr(locals, s->value);
            break;
        }
        case EXPR_MAP_LITERAL: {
            MapLiteralExpr* m = (MapLiteralExpr*)expr;
            for (ListNode* n = m->entries ? m->entries->head : NULL; n != NULL; n = n->next) {
                MapEntry* e = (MapEntry*)n->data;
                if (e) collectLambdaLocalsExpr(locals, e->value);
            }
            break;
        }
        case EXPR_INDEX: {
            IndexExpr* i = (IndexExpr*)expr;
            collectLambdaLocalsExpr(locals, i->object);
            collectLambdaLocalsExpr(locals, i->index);
            break;
        }
        case EXPR_INDEX_SET: {
            IndexSetExpr* s = (IndexSetExpr*)expr;
            collectLambdaLocalsExpr(locals, s->object);
            collectLambdaLocalsExpr(locals, s->index);
            collectLambdaLocalsExpr(locals, s->value);
            break;
        }
        case EXPR_POSTFIX:
            collectLambdaLocalsExpr(locals, ((PostfixExpr*)expr)->operand);
            break;
        case EXPR_PREFIX:
            collectLambdaLocalsExpr(locals, ((PrefixExpr*)expr)->operand);
            break;
        default:
            break;
    }
}

static void collectLambdaLocalsStmt(List* locals, Stmt* stmt) {
    if (!stmt) return;
    if (stmt->type == STMT_PRIVATE) {
        collectLambdaLocalsStmt(locals, ((PrivateStmt*)stmt)->inner);
        return;
    }
    if (stmt->type == STMT_VAR) {
        VarStmt* v = (VarStmt*)stmt;
        nameSetAdd(locals, v->name.start, v->name.length);
        collectLambdaLocalsExpr(locals, v->initializer);
        return;
    }
    if (stmt->type == STMT_DESTRUCTURE) {
        DestructureStmt* d = (DestructureStmt*)stmt;
        if (d->isDeclaration && d->names) {
            for (ListNode* n = d->names->head; n != NULL; n = n->next) {
                Token* t = (Token*)n->data;
                if (t) nameSetAdd(locals, t->start, t->length);
            }
        }
        collectLambdaLocalsExpr(locals, d->value);
        return;
    }
    switch (stmt->type) {
        case STMT_BLOCK: {
            BlockStmt* b = (BlockStmt*)stmt;
            for (ListNode* n = b->statements ? b->statements->head : NULL; n != NULL; n = n->next) {
                collectLambdaLocalsStmt(locals, (Stmt*)n->data);
            }
            break;
        }
        case STMT_IF: {
            IfStmt* i = (IfStmt*)stmt;
            collectLambdaLocalsExpr(locals, i->condition);
            collectLambdaLocalsStmt(locals, i->thenBranch);
            collectLambdaLocalsStmt(locals, i->elseBranch);
            break;
        }
        case STMT_FOR: {
            ForStmt* f = (ForStmt*)stmt;
            collectLambdaLocalsStmt(locals, f->initializer);
            collectLambdaLocalsExpr(locals, f->condition);
            collectLambdaLocalsExpr(locals, f->increment);
            collectLambdaLocalsStmt(locals, f->body);
            break;
        }
        case STMT_FOR_IN: {
            ForInStmt* fi = (ForInStmt*)stmt;
            nameSetAdd(locals, fi->loopVar.start, fi->loopVar.length);
            collectLambdaLocalsExpr(locals, fi->range);
            collectLambdaLocalsStmt(locals, fi->body);
            break;
        }
        case STMT_WHILE: {
            WhileStmt* w = (WhileStmt*)stmt;
            collectLambdaLocalsExpr(locals, w->condition);
            collectLambdaLocalsStmt(locals, w->body);
            break;
        }
        case STMT_DO_WHILE: {
            DoWhileStmt* dw = (DoWhileStmt*)stmt;
            collectLambdaLocalsStmt(locals, dw->body);
            collectLambdaLocalsExpr(locals, dw->condition);
            break;
        }
        case STMT_RETURN: {
            ReturnStmt* r = (ReturnStmt*)stmt;
            if (r->values) {
                for (ListNode* n = r->values->head; n != NULL; n = n->next) {
                    collectLambdaLocalsExpr(locals, (Expr*)n->data);
                }
            } else {
                collectLambdaLocalsExpr(locals, r->value);
            }
            break;
        }
        case STMT_EXPR:
            collectLambdaLocalsExpr(locals, ((ExprStmt*)stmt)->expression);
            break;
        default:
            break;
    }
}

static void collectLambdaUsesExpr(List* uses, Expr* expr) {
    if (!expr) return;
    if (expr->type == EXPR_LAMBDA) return; // stop at nested lambda
    switch (expr->type) {
        case EXPR_VARIABLE: {
            VariableExpr* v = (VariableExpr*)expr;
            if (v->name.type != TOKEN_THIS) {
                nameSetAdd(uses, v->name.start, v->name.length);
            }
            break;
        }
        case EXPR_BINARY: {
            BinaryExpr* b = (BinaryExpr*)expr;
            collectLambdaUsesExpr(uses, b->left);
            collectLambdaUsesExpr(uses, b->right);
            break;
        }
        case EXPR_UNARY:
            collectLambdaUsesExpr(uses, ((UnaryExpr*)expr)->right);
            break;
        case EXPR_GROUPING:
            collectLambdaUsesExpr(uses, ((GroupingExpr*)expr)->expression);
            break;
        case EXPR_CALL: {
            CallExpr* c = (CallExpr*)expr;
            collectLambdaUsesExpr(uses, c->callee);
            for (ListNode* n = c->arguments ? c->arguments->head : NULL; n != NULL; n = n->next) {
                collectLambdaUsesExpr(uses, (Expr*)n->data);
            }
            break;
        }
        case EXPR_ASSIGN:
            collectLambdaUsesExpr(uses, ((AssignExpr*)expr)->value);
            break;
        case EXPR_GET:
            collectLambdaUsesExpr(uses, ((GetExpr*)expr)->object);
            break;
        case EXPR_SET: {
            SetExpr* s = (SetExpr*)expr;
            collectLambdaUsesExpr(uses, s->object);
            collectLambdaUsesExpr(uses, s->value);
            break;
        }
        case EXPR_MAP_LITERAL: {
            MapLiteralExpr* m = (MapLiteralExpr*)expr;
            for (ListNode* n = m->entries ? m->entries->head : NULL; n != NULL; n = n->next) {
                MapEntry* e = (MapEntry*)n->data;
                if (e) collectLambdaUsesExpr(uses, e->value);
            }
            break;
        }
        case EXPR_INDEX: {
            IndexExpr* i = (IndexExpr*)expr;
            collectLambdaUsesExpr(uses, i->object);
            collectLambdaUsesExpr(uses, i->index);
            break;
        }
        case EXPR_INDEX_SET: {
            IndexSetExpr* s = (IndexSetExpr*)expr;
            collectLambdaUsesExpr(uses, s->object);
            collectLambdaUsesExpr(uses, s->index);
            collectLambdaUsesExpr(uses, s->value);
            break;
        }
        case EXPR_POSTFIX:
            collectLambdaUsesExpr(uses, ((PostfixExpr*)expr)->operand);
            break;
        case EXPR_PREFIX:
            collectLambdaUsesExpr(uses, ((PrefixExpr*)expr)->operand);
            break;
        default:
            break;
    }
}

static void collectLambdaUsesStmt(List* uses, Stmt* stmt) {
    if (!stmt) return;
    if (stmt->type == STMT_PRIVATE) {
        collectLambdaUsesStmt(uses, ((PrivateStmt*)stmt)->inner);
        return;
    }
    switch (stmt->type) {
        case STMT_VAR:
            collectLambdaUsesExpr(uses, ((VarStmt*)stmt)->initializer);
            break;
        case STMT_DESTRUCTURE:
            collectLambdaUsesExpr(uses, ((DestructureStmt*)stmt)->value);
            break;
        case STMT_BLOCK: {
            BlockStmt* b = (BlockStmt*)stmt;
            for (ListNode* n = b->statements ? b->statements->head : NULL; n != NULL; n = n->next) {
                collectLambdaUsesStmt(uses, (Stmt*)n->data);
            }
            break;
        }
        case STMT_IF: {
            IfStmt* i = (IfStmt*)stmt;
            collectLambdaUsesExpr(uses, i->condition);
            collectLambdaUsesStmt(uses, i->thenBranch);
            collectLambdaUsesStmt(uses, i->elseBranch);
            break;
        }
        case STMT_FOR: {
            ForStmt* f = (ForStmt*)stmt;
            collectLambdaUsesStmt(uses, f->initializer);
            collectLambdaUsesExpr(uses, f->condition);
            collectLambdaUsesExpr(uses, f->increment);
            collectLambdaUsesStmt(uses, f->body);
            break;
        }
        case STMT_FOR_IN: {
            ForInStmt* fi = (ForInStmt*)stmt;
            collectLambdaUsesExpr(uses, fi->range);
            collectLambdaUsesStmt(uses, fi->body);
            break;
        }
        case STMT_WHILE: {
            WhileStmt* w = (WhileStmt*)stmt;
            collectLambdaUsesExpr(uses, w->condition);
            collectLambdaUsesStmt(uses, w->body);
            break;
        }
        case STMT_DO_WHILE: {
            DoWhileStmt* dw = (DoWhileStmt*)stmt;
            collectLambdaUsesStmt(uses, dw->body);
            collectLambdaUsesExpr(uses, dw->condition);
            break;
        }
        case STMT_RETURN: {
            ReturnStmt* r = (ReturnStmt*)stmt;
            if (r->values) {
                for (ListNode* n = r->values->head; n != NULL; n = n->next) {
                    collectLambdaUsesExpr(uses, (Expr*)n->data);
                }
            } else {
                collectLambdaUsesExpr(uses, r->value);
            }
            break;
        }
        case STMT_EXPR:
            collectLambdaUsesExpr(uses, ((ExprStmt*)stmt)->expression);
            break;
        default:
            break;
    }
}

static List* computeLambdaFreeNames(Compiler* compiler, LambdaExpr* expr) {
    (void)compiler;
    List* locals = listNew();
    if (expr->params) {
        for (ListNode* n = expr->params->head; n != NULL; n = n->next) {
            Parameter* p = (Parameter*)n->data;
            if (!p) continue;
            nameSetAdd(locals, p->name.start, p->name.length);
        }
    }
    for (ListNode* n = expr->body ? expr->body->head : NULL; n != NULL; n = n->next) {
        collectLambdaLocalsStmt(locals, (Stmt*)n->data);
    }

    List* uses = listNew();
    for (ListNode* n = expr->body ? expr->body->head : NULL; n != NULL; n = n->next) {
        collectLambdaUsesStmt(uses, (Stmt*)n->data);
    }

    List* freeNames = listNew();
    for (ListNode* n = uses->head; n != NULL; n = n->next) {
        Token* t = (Token*)n->data;
        if (!t) continue;
        if (!nameSetContains(locals, t->start, t->length)) {
            nameSetAdd(freeNames, t->start, t->length);
        }
    }

    // Note: tokens inside `locals`/`uses` are shallow wrappers; free lists only.
    freeTokenSet(locals);
    freeTokenSet(uses);
    return freeNames;
}

// void emitExprStmt(Compiler* compiler, ExprStmt* stmt) {
//     emitDebug("emitExprStmt\n");
//     emitExpr(compiler, stmt->expression);
// }


// LLVMValueRef emitExpr(Compiler* compiler, Expr* expr) {
//     emitDebug("emitExpr type:%d\n", expr->type);
//     switch (expr->type) {
//         case EXPR_LITERAL:
//             return emitLiteralExpr(compiler, (LiteralExpr*)expr);
//         // case EXPR_VARIABLE:
//         //     return emitVariableExpr(compiler, (VariableExpr*)expr);
//         case EXPR_ASSIGN:
//             return emitAssignExpr(compiler, (AssignExpr*)expr);
//         case EXPR_CALL:
//             return emitCallExpr(compiler, (CallExpr*)expr);
//         default:
//             emitDebug("Invalid expression type %d\n", expr->type);
//             return NULL;
//     }
// }


LLVMValueRef emitBinaryExpr(Compiler* compiler, BinaryExpr* expr) {
    emitDebug("emitBinaryExpr start\n");
    
    // 编译左右操作数
    // Short-circuit for logical operators
    if (expr->operator.type == TOKEN_AND || expr->operator.type == TOKEN_OR) {
        LLVMValueRef leftVal = compileExpr(compiler, expr->left);
        LLVMValueRef leftBool = llvmCoerceToBool(compiler, leftVal);
        if (!leftBool) {
            error("Failed to compile left operand for logical op\n");
            return NULL;
        }

        LLVMValueRef function = compiler->current->func;
        LLVMBuilderRef builder = compiler->builder;

        LLVMBasicBlockRef currentBlock = LLVMGetInsertBlock(builder);
        LLVMBasicBlockRef rhsBlock = LLVMAppendBasicBlock(function, expr->operator.type == TOKEN_AND ? "and.rhs" : "or.rhs");
        LLVMBasicBlockRef endBlock = LLVMAppendBasicBlock(function, expr->operator.type == TOKEN_AND ? "and.end" : "or.end");

        if (expr->operator.type == TOKEN_AND) {
            LLVMBuildCondBr(builder, leftBool, rhsBlock, endBlock);
        } else {
            LLVMBuildCondBr(builder, leftBool, endBlock, rhsBlock);
        }

        // RHS
        LLVMPositionBuilderAtEnd(builder, rhsBlock);
        LLVMValueRef rightVal = compileExpr(compiler, expr->right);
        LLVMValueRef rightBool = llvmCoerceToBool(compiler, rightVal);
        if (!rightBool) {
            error("Failed to compile right operand for logical op\n");
            return NULL;
        }
        LLVMBasicBlockRef rhsEnd = LLVMGetInsertBlock(builder);
        LLVMBuildBr(builder, endBlock);

        // End / Phi
        LLVMPositionBuilderAtEnd(builder, endBlock);
        LLVMValueRef phi = LLVMBuildPhi(builder, LLVMInt1TypeInContext(compiler->context),
                                        expr->operator.type == TOKEN_AND ? "and" : "or");

        LLVMValueRef falseVal = LLVMConstInt(LLVMInt1TypeInContext(compiler->context), 0, 0);
        LLVMValueRef trueVal = LLVMConstInt(LLVMInt1TypeInContext(compiler->context), 1, 0);

        if (expr->operator.type == TOKEN_AND) {
            LLVMValueRef incomingVals[] = { rightBool, falseVal };
            LLVMBasicBlockRef incomingBlocks[] = { rhsEnd, currentBlock };
            LLVMAddIncoming(phi, incomingVals, incomingBlocks, 2);
        } else {
            LLVMValueRef incomingVals[] = { trueVal, rightBool };
            LLVMBasicBlockRef incomingBlocks[] = { currentBlock, rhsEnd };
            LLVMAddIncoming(phi, incomingVals, incomingBlocks, 2);
        }

        return phi;
    }

    // Coalesce: Option<T> ?? T -> T (short-circuit; RHS evaluated only on None)
    if (expr->operator.type == TOKEN_COALESCE) {
        LLVMBuilderRef builder = compiler->builder;
        LLVMValueRef leftVal = compileExpr(compiler, expr->left);
        if (!leftVal) {
            error("Failed to compile left operand for ??\n");
            return NULL;
        }
        LLVMTypeRef leftTy = LLVMTypeOf(leftVal);
        if (!isOptionLLVMType(leftTy)) {
            error("Left operand of ?? must be Option<T>\n");
            return NULL;
        }
        LLVMTypeRef innerTy = LLVMStructGetTypeAtIndex(leftTy, 1);
        LLVMValueRef ok = LLVMBuildExtractValue(builder, leftVal, 0, "opt_ok");
        LLVMValueRef payload = LLVMBuildExtractValue(builder, leftVal, 1, "opt_v");

        LLVMValueRef fn = compiler->current->func;
        LLVMBasicBlockRef currentBB = LLVMGetInsertBlock(builder);
        LLVMBasicBlockRef rhsBB = LLVMAppendBasicBlock(fn, "coalesce.rhs");
        LLVMBasicBlockRef contBB = LLVMAppendBasicBlock(fn, "coalesce.cont");
        LLVMBuildCondBr(builder, ok, contBB, rhsBB);

        LLVMPositionBuilderAtEnd(builder, rhsBB);
        LLVMValueRef rhsVal = compileExpr(compiler, expr->right);
        if (rhsVal) {
            LLVMTypeRef vt = compilerGetTuaValueType(compiler);
            if (innerTy == vt && LLVMTypeOf(rhsVal) != vt) {
                rhsVal = tuaValueFromValue(compiler, rhsVal);
            } else {
                rhsVal = castToType(compiler, rhsVal, innerTy);
            }
        }
        LLVMBuildBr(builder, contBB);
        LLVMBasicBlockRef rhsEnd = LLVMGetInsertBlock(builder);

        LLVMPositionBuilderAtEnd(builder, contBB);
        LLVMValueRef phi = LLVMBuildPhi(builder, innerTy, "coalesce");
        LLVMAddIncoming(phi, &payload, &currentBB, 1);
        LLVMAddIncoming(phi, &rhsVal, &rhsEnd, 1);
        return phi;
    }

    LLVMValueRef left = compileExpr(compiler, expr->left);
    LLVMValueRef right = compileExpr(compiler, expr->right);
    
    if (!left ) {
        error("Failed to compile left operands, op:%s\n",tokenToString(expr->operator.type));
        return NULL;
    }

    if (!right) {
        error("Failed to compile right operands\n");
        return NULL;
    }

    if (!left || !right) {
        error("Failed to compile operands\n");
        return NULL;
    }
    
    LLVMBuilderRef builder = compiler->builder;
    LLVMContextRef context = compiler->context;
    LLVMTypeRef leftType = LLVMTypeOf(left);
    LLVMTypeRef rightType = LLVMTypeOf(right);
    LLVMTypeKind leftKind = LLVMGetTypeKind(leftType);
    LLVMTypeKind rightKind = LLVMGetTypeKind(rightType);
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(context), 0);

    // Better compile-time errors for Option<T>:
    // - disallow comparing Option<T> with non-Option directly
    int opIsEq = (expr->operator.type == TOKEN_EQ || expr->operator.type == TOKEN_NEQ);
    int opIsOrd = (expr->operator.type == TOKEN_LT ||
                   expr->operator.type == TOKEN_GT ||
                   expr->operator.type == TOKEN_LE ||
                   expr->operator.type == TOKEN_GE);
    if ((opIsEq || opIsOrd) && (isOptionLLVMType(leftType) ^ isOptionLLVMType(rightType))) {
        compilerErrorAt(compiler, expr->operator.line, "cannot compare Option<T> with non-Option; use unwrap()/isSome() or compare with Some(...)");
        return NULL;
    }

    // String concatenation: i8* + i8*
    if (expr->operator.type == TOKEN_PLUS &&
        leftKind == LLVMPointerTypeKind && rightKind == LLVMPointerTypeKind &&
        leftType == i8ptr && rightType == i8ptr) {
        LLVMValueRef fn = getOrCreateTuaStrConcat(compiler);
        LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
        LLVMValueRef args2[2] = { left, right };
        return LLVMBuildCall2(builder, fnType, fn, args2, 2, "sadd");
    }

    // String content equality: i8* ==/!= i8*
    if ((expr->operator.type == TOKEN_EQ || expr->operator.type == TOKEN_NEQ) &&
        leftKind == LLVMPointerTypeKind && rightKind == LLVMPointerTypeKind &&
        leftType == i8ptr && rightType == i8ptr) {
        LLVMValueRef nullPtr = LLVMConstNull(i8ptr);
        LLVMValueRef leftNull = LLVMBuildICmp(builder, LLVMIntEQ, left, nullPtr, "l_null");
        LLVMValueRef rightNull = LLVMBuildICmp(builder, LLVMIntEQ, right, nullPtr, "r_null");
        LLVMValueRef eitherNull = LLVMBuildOr(builder, leftNull, rightNull, "either_null");
        LLVMValueRef bothNull = LLVMBuildAnd(builder, leftNull, rightNull, "both_null");

        LLVMValueRef fn = compiler->current->func;
        LLVMBasicBlockRef nullBB = LLVMAppendBasicBlock(fn, "str.null");
        LLVMBasicBlockRef cmpBB = LLVMAppendBasicBlock(fn, "str.cmp");
        LLVMBasicBlockRef contBB = LLVMAppendBasicBlock(fn, "str.cont");

        LLVMBuildCondBr(builder, eitherNull, nullBB, cmpBB);

        LLVMPositionBuilderAtEnd(builder, nullBB);
        LLVMValueRef nullRes = bothNull;
        if (expr->operator.type == TOKEN_NEQ) {
            nullRes = LLVMBuildNot(builder, bothNull, "null_neq");
        }
        LLVMBuildBr(builder, contBB);

        LLVMPositionBuilderAtEnd(builder, cmpBB);
        LLVMValueRef strcmpFn = getOrCreateStrcmp(compiler);
        LLVMTypeRef strcmpType = LLVMGlobalGetValueType(strcmpFn);
        LLVMValueRef args2[2] = { left, right };
        LLVMValueRef cmp = LLVMBuildCall2(builder, strcmpType, strcmpFn, args2, 2, "strcmp");
        LLVMValueRef zero = LLVMConstInt(LLVMInt32TypeInContext(context), 0, 0);
        LLVMValueRef strRes = NULL;
        if (expr->operator.type == TOKEN_EQ) {
            strRes = LLVMBuildICmp(builder, LLVMIntEQ, cmp, zero, "streq");
        } else {
            strRes = LLVMBuildICmp(builder, LLVMIntNE, cmp, zero, "strneq");
        }
        LLVMBuildBr(builder, contBB);

        LLVMPositionBuilderAtEnd(builder, contBB);
        LLVMValueRef phi = LLVMBuildPhi(builder, LLVMInt1TypeInContext(context), "str_phi");
        LLVMAddIncoming(phi, &nullRes, &nullBB, 1);
        LLVMAddIncoming(phi, &strRes, &cmpBB, 1);
        return phi;
    }

    // Option<T> equality: Option<T> ==/!= Option<T>
    if ((expr->operator.type == TOKEN_EQ || expr->operator.type == TOKEN_NEQ) &&
        isOptionLLVMType(leftType) && isOptionLLVMType(rightType)) {
        if (leftType != rightType) {
            compilerErrorAt(compiler, expr->operator.line, "Option<T> comparison requires same type");
            return NULL;
        }

        LLVMTypeRef innerTy = LLVMStructGetTypeAtIndex(leftType, 1);
        LLVMValueRef okL = LLVMBuildExtractValue(builder, left, 0, "opt_ok_l");
        LLVMValueRef okR = LLVMBuildExtractValue(builder, right, 0, "opt_ok_r");
        LLVMValueRef okEq = LLVMBuildICmp(builder, LLVMIntEQ, okL, okR, "opt_ok_eq");
        LLVMValueRef bothSome = LLVMBuildAnd(builder, okL, okR, "opt_both_some");

        LLVMValueRef fn = compiler->current->func;
        LLVMBasicBlockRef someBB = LLVMAppendBasicBlock(fn, "opt.eq.some");
        LLVMBasicBlockRef noneBB = LLVMAppendBasicBlock(fn, "opt.eq.none");
        LLVMBasicBlockRef contBB = LLVMAppendBasicBlock(fn, "opt.eq.cont");
        LLVMBuildCondBr(builder, bothSome, someBB, noneBB);

        // both Some: compare payloads
        LLVMPositionBuilderAtEnd(builder, someBB);
        LLVMValueRef vL = LLVMBuildExtractValue(builder, left, 1, "opt_v_l");
        LLVMValueRef vR = LLVMBuildExtractValue(builder, right, 1, "opt_v_r");

        LLVMValueRef payloadEq = NULL;
        LLVMTypeKind innerKind = LLVMGetTypeKind(innerTy);
        if (innerKind == LLVMIntegerTypeKind || innerKind == LLVMPointerTypeKind) {
            if (innerKind == LLVMPointerTypeKind && innerTy == i8ptr) {
                // string content equality for Option<string>
                LLVMValueRef nullPtr = LLVMConstNull(i8ptr);
                LLVMValueRef lNull = LLVMBuildICmp(builder, LLVMIntEQ, vL, nullPtr, "osl_null");
                LLVMValueRef rNull = LLVMBuildICmp(builder, LLVMIntEQ, vR, nullPtr, "osr_null");
                LLVMValueRef eitherNull = LLVMBuildOr(builder, lNull, rNull, "os_either_null");
                LLVMValueRef bothNull = LLVMBuildAnd(builder, lNull, rNull, "os_both_null");

                LLVMBasicBlockRef snullBB = LLVMAppendBasicBlock(fn, "opt.str.null");
                LLVMBasicBlockRef scmpBB = LLVMAppendBasicBlock(fn, "opt.str.cmp");
                LLVMBasicBlockRef scontBB = LLVMAppendBasicBlock(fn, "opt.str.cont");

                LLVMBuildCondBr(builder, eitherNull, snullBB, scmpBB);

                LLVMPositionBuilderAtEnd(builder, snullBB);
                LLVMValueRef nullRes = bothNull;
                LLVMBuildBr(builder, scontBB);

                LLVMPositionBuilderAtEnd(builder, scmpBB);
                LLVMValueRef strcmpFn = getOrCreateStrcmp(compiler);
                LLVMTypeRef strcmpType = LLVMGlobalGetValueType(strcmpFn);
                LLVMValueRef args2[2] = { vL, vR };
                LLVMValueRef cmp = LLVMBuildCall2(builder, strcmpType, strcmpFn, args2, 2, "strcmp");
                LLVMValueRef zero = LLVMConstInt(LLVMInt32TypeInContext(context), 0, 0);
                LLVMValueRef strEq = LLVMBuildICmp(builder, LLVMIntEQ, cmp, zero, "os_streq");
                LLVMBuildBr(builder, scontBB);

                LLVMPositionBuilderAtEnd(builder, scontBB);
                LLVMValueRef phi = LLVMBuildPhi(builder, LLVMInt1TypeInContext(context), "os_phi");
                LLVMAddIncoming(phi, &nullRes, &snullBB, 1);
                LLVMAddIncoming(phi, &strEq, &scmpBB, 1);
                payloadEq = phi;
            } else {
                payloadEq = LLVMBuildICmp(builder, LLVMIntEQ, vL, vR, "opt_v_eq");
            }
        } else if (innerKind == LLVMDoubleTypeKind) {
            payloadEq = LLVMBuildFCmp(builder, LLVMRealOEQ, vL, vR, "opt_v_feq");
        } else {
            compilerErrorAt(compiler, expr->operator.line, "unsupported Option<T> payload comparison");
            return NULL;
        }

        LLVMBuildBr(builder, contBB);
        LLVMBasicBlockRef someEnd = LLVMGetInsertBlock(builder);

        // not both Some: payload is equal by definition (if okEq is true, both None)
        LLVMPositionBuilderAtEnd(builder, noneBB);
        LLVMValueRef payloadOk = LLVMConstInt(LLVMInt1TypeInContext(context), 1, 0);
        LLVMBuildBr(builder, contBB);
        LLVMBasicBlockRef noneEnd = LLVMGetInsertBlock(builder);

        // merge
        LLVMPositionBuilderAtEnd(builder, contBB);
        LLVMValueRef payloadPhi = LLVMBuildPhi(builder, LLVMInt1TypeInContext(context), "opt_payeq");
        LLVMAddIncoming(payloadPhi, &payloadEq, &someEnd, 1);
        LLVMAddIncoming(payloadPhi, &payloadOk, &noneEnd, 1);

        LLVMValueRef eq = LLVMBuildAnd(builder, okEq, payloadPhi, "opt_eq");
        if (expr->operator.type == TOKEN_NEQ) {
            eq = LLVMBuildNot(builder, eq, "opt_neq");
        }
        return eq;
    }

    // Numeric type promotion (int/long/double) for arithmetic & comparisons.
    int opIsArithmetic = (expr->operator.type == TOKEN_PLUS ||
                          expr->operator.type == TOKEN_MINUS ||
                          expr->operator.type == TOKEN_STAR ||
                          expr->operator.type == TOKEN_SLASH);
    int opIsCompare = (expr->operator.type == TOKEN_EQ ||
                       expr->operator.type == TOKEN_NEQ ||
                       expr->operator.type == TOKEN_LT ||
                       expr->operator.type == TOKEN_GT ||
                       expr->operator.type == TOKEN_LE ||
                       expr->operator.type == TOKEN_GE);

    bool leftIsNum = (leftKind == LLVMIntegerTypeKind || leftKind == LLVMDoubleTypeKind);
    bool rightIsNum = (rightKind == LLVMIntegerTypeKind || rightKind == LLVMDoubleTypeKind);

    LLVMTypeRef commonType = NULL;
    bool isFloat = false;
    if ((opIsArithmetic || opIsCompare) && leftIsNum && rightIsNum) {
        if (leftKind == LLVMDoubleTypeKind || rightKind == LLVMDoubleTypeKind) {
            commonType = LLVMDoubleTypeInContext(context);
            isFloat = true;
        } else {
            unsigned lb = LLVMGetIntTypeWidth(leftType);
            unsigned rb = LLVMGetIntTypeWidth(rightType);
            // Promote bool to int for arithmetic.
            if (opIsArithmetic) {
                if (lb == 1) lb = 32;
                if (rb == 1) rb = 32;
            }
            unsigned cb = lb > rb ? lb : rb;
            if (cb < 32) cb = 32;
            commonType = LLVMIntTypeInContext(context, cb);
            isFloat = false;
        }
        left = castToType(compiler, left, commonType);
        right = castToType(compiler, right, commonType);
        leftType = LLVMTypeOf(left);
        rightType = LLVMTypeOf(right);
        leftKind = LLVMGetTypeKind(leftType);
        rightKind = LLVMGetTypeKind(rightType);
    } else {
        isFloat = (leftKind == LLVMDoubleTypeKind);
    }

    emitDebug("emitBinaryExpr op type:%s\n", tokenToString(expr->operator.type));
    switch (expr->operator.type) {
        case TOKEN_PLUS:
            return isFloat ? 
                LLVMBuildFAdd(builder, left, right, "fadd") :
                LLVMBuildAdd(builder, left, right, "add");
            
        case TOKEN_MINUS:
            return isFloat ? 
                LLVMBuildFSub(builder, left, right, "fsub") :
                LLVMBuildSub(builder, left, right, "sub");
            
        case TOKEN_STAR:
            return isFloat ? 
                LLVMBuildFMul(builder, left, right, "fmul") :
                LLVMBuildMul(builder, left, right, "mul");
            
        case TOKEN_SLASH:
            return isFloat ? 
                LLVMBuildFDiv(builder, left, right, "fdiv") :
                LLVMBuildSDiv(builder, left, right, "div");
            
        case TOKEN_EQ:
            return isFloat ?
                LLVMBuildFCmp(builder, LLVMRealOEQ, left, right, "fcmp_eq") :
                LLVMBuildICmp(builder, LLVMIntEQ, left, right, "icmp_eq");
            
        case TOKEN_NEQ:
            return isFloat ?
                LLVMBuildFCmp(builder, LLVMRealONE, left, right, "fcmp_ne") :
                LLVMBuildICmp(builder, LLVMIntNE, left, right, "icmp_ne");
            
        case TOKEN_LT:
         emitDebug("Building < comparison\n");
            return isFloat ?
                LLVMBuildFCmp(builder, LLVMRealOLT, left, right, "fcmp_lt") :
                LLVMBuildICmp(builder, LLVMIntSLT, left, right, "icmp_lt");
            
        case TOKEN_GT:
            return isFloat ?
                LLVMBuildFCmp(builder, LLVMRealOGT, left, right, "fcmp_gt") :
                LLVMBuildICmp(builder, LLVMIntSGT, left, right, "icmp_gt");
            
        case TOKEN_LE:
            emitDebug("Building <= comparison\n");
            return isFloat ?
                LLVMBuildFCmp(builder, LLVMRealOLE, left, right, "fcmp_le") :
                LLVMBuildICmp(builder, LLVMIntSLE, left, right, "icmp_le");
            
        case TOKEN_GE:
            return isFloat ?
                LLVMBuildFCmp(builder, LLVMRealOGE, left, right, "fcmp_ge") :
                LLVMBuildICmp(builder, LLVMIntSGE, left, right, "icmp_ge");
            
        default:
            error("Unknown binary operator");
            return NULL;
    }

    emitDebug("emitBinaryExpr end\n");
}

LLVMValueRef emitVariableExpr(Compiler* compiler, VariableExpr* expr) {
    emitDebug("emitVariableExpr\n");
    
    VariableRef var = (VariableRef){NULL, 0, NULL, NULL, NULL, 0, 0, 0, 0, NULL};

    // Resolve in current block chain
    Block* block = compiler->current;
    while (block != NULL) {
        var = findVariableWithLength(block->variables, expr->name.start, expr->name.length);
        if (var.value) break;
        block = block->parent;
    }

    if (!var.value && compiler->currentModulePrefix) {
        int ql = 0;
        char* q = compilerQualifyToken(compiler, &expr->name, &ql);
        if (q) {
            block = compiler->current;
            while (block != NULL) {
                var = findVariableWithLength(block->variables, q, ql);
                if (var.value) break;
                block = block->parent;
            }
            free(q);
        }
    }
    
    if (!var.value) {
        error("Undefined variable, name: %.*s\n", expr->name.length, expr->name.start);
        return NULL;
    }
    
    // 如果是局部变量，需要加载其值
    if (!var.isGlobal) {
        if (!var.type) {
            error("Missing variable type metadata, name: %.*s\n", expr->name.length, expr->name.start);
            return NULL;
        }
        if (var.isBoxed) {
            if (!var.boxPtrType) {
                error("Missing boxed pointer type metadata, name: %.*s\n", expr->name.length, expr->name.start);
                return NULL;
            }
            LLVMValueRef ptr = LLVMBuildLoad2(compiler->builder, var.boxPtrType, var.value, "boxptr");
            return LLVMBuildLoad2(compiler->builder, var.type, ptr, "load");
        } else {
            return LLVMBuildLoad2(
                compiler->builder,
                var.type,
                var.value,
                "load"
            );
        }
    }
    
    // 全局变量直接返回其值
    return var.value;
}

LLVMValueRef emitLiteralExpr(Compiler* compiler, LiteralExpr* expr) {
    emitDebug("emitLiteralExpr type:%s\n", tokenToString(expr->value.type));
    
    switch(expr->value.type) {
        case TOKEN_INT: {
            int32_t value = parseIntToken(expr->value);
            return LLVMConstInt(LLVMInt32TypeInContext(compiler->context), 
                              value, 0);
        }
        case TOKEN_LONG: {
            int64_t value = parseLongToken(expr->value);
            return LLVMConstInt(LLVMInt64TypeInContext(compiler->context), (uint64_t)value, 0);
        }
        case TOKEN_DOUBLE: {
            double value = parseDoubleToken(expr->value);
            return LLVMConstReal(LLVMDoubleTypeInContext(compiler->context), 
                               value);
        }
        case TOKEN_STRING_LITERAL: {
            char* s = dupStringLiteral(expr->value);
            if (!s) return NULL;
            LLVMValueRef out = LLVMBuildGlobalStringPtr(compiler->builder, s, "str");
            free(s);
            return out;
        }
        case TOKEN_NULL: {
            return LLVMConstNull(LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0));
        }
        case TOKEN_TRUE:
            return LLVMConstInt(LLVMInt1TypeInContext(compiler->context), 1, 0);
        case TOKEN_FALSE:
            return LLVMConstInt(LLVMInt1TypeInContext(compiler->context), 0, 0);
        default:
            emitDebug("Unknown literal type");
            return NULL;
    }
}

LLVMValueRef emitMapLiteralExpr(Compiler* compiler, MapLiteralExpr* expr) {
    if (!compiler || !expr) return NULL;
    LLVMBuilderRef builder = compiler->builder;
    LLVMTypeRef mapType = compilerGetMapType(compiler);
    LLVMValueRef newFn = getOrCreateTuaMapNew(compiler);
    LLVMValueRef mapVal = LLVMBuildCall2(builder, LLVMGlobalGetValueType(newFn), newFn, NULL, 0, "map");
    mapVal = castToType(compiler, mapVal, mapType);

    LLVMTypeRef expectedKeyTy = compiler->expectedMapKeyType;
    LLVMTypeRef expectedValTy = compiler->expectedMapValueType;
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);

    LLVMValueRef setFn = getOrCreateTuaMapSet(compiler);
    LLVMTypeRef setType = LLVMGlobalGetValueType(setFn);

    for (ListNode* n = expr->entries ? expr->entries->head : NULL; n != NULL; n = n->next) {
        MapEntry* e = (MapEntry*)n->data;
        if (!e) continue;

        LLVMValueRef keyConst = NULL;
        if (e->key.type == TOKEN_INT) {
            int32_t v = parseIntToken(e->key);
            keyConst = LLVMConstInt(LLVMInt32TypeInContext(compiler->context), (uint64_t)(int64_t)v, 1);
        } else if (e->key.type == TOKEN_LONG) {
            int64_t v = parseLongToken(e->key);
            keyConst = LLVMConstInt(LLVMInt64TypeInContext(compiler->context), (uint64_t)v, 1);
        } else if (e->key.type == TOKEN_STRING_LITERAL) {
            char* s = dupStringLiteral(e->key);
            if (!s) return NULL;
            keyConst = LLVMBuildGlobalStringPtr(builder, s, "kstr");
            free(s);
        } else {
            error("Map key must be int/long/string literal\n");
            return NULL;
        }

        if (expectedKeyTy) {
            if (expectedKeyTy == i8ptr) {
                if (e->key.type != TOKEN_STRING_LITERAL) {
                    compilerErrorAt(compiler, e->key.line, "typed map key type is string; got non-string key");
                    return NULL;
                }
            } else {
                if (e->key.type == TOKEN_STRING_LITERAL) {
                    compilerErrorAt(compiler, e->key.line, "typed map key type is int/long; got string key");
                    return NULL;
                }
            }
        }

        LLVMValueRef key = tuaValueFromKey(compiler, keyConst);
        if (!key) return NULL;
        if (expectedValTy && e->value && e->value->type == EXPR_LITERAL) {
            LiteralExpr* lit = (LiteralExpr*)e->value;
            if (lit->value.type == TOKEN_NULL) {
                compilerErrorAt(compiler, lit->value.line, "cannot assign null into typed map value");
                return NULL;
            }
        }

        LLVMValueRef rawValue = compileExpr(compiler, e->value);
        if (!rawValue) return NULL;
        if (expectedValTy) {
            if (!typedMapValueCompatible(compiler, expectedValTy, rawValue)) {
                compilerErrorAt(compiler, e->value ? e->value->token.line : e->key.line, "typed map value type mismatch");
                return NULL;
            }
            rawValue = castToType(compiler, rawValue, expectedValTy);
        }
        LLVMValueRef v = tuaValueFromValue(compiler, rawValue);
        if (!v) return NULL;

        LLVMValueRef args[3] = { mapVal, key, v };
        LLVMBuildCall2(builder, setType, setFn, args, 3, "");
    }

    return mapVal;
}

LLVMValueRef emitIndexExpr(Compiler* compiler, IndexExpr* expr) {
    if (!compiler || !expr) return NULL;
    LLVMBuilderRef builder = compiler->builder;
    LLVMValueRef obj = compileExpr(compiler, expr->object);
    if (!obj) return NULL;
    if (LLVMTypeOf(obj) != compilerGetMapType(compiler)) {
        error("Indexing is only supported on map for now\n");
        return NULL;
    }
    LLVMValueRef keyExpr = compileExpr(compiler, expr->index);
    if (!keyExpr) return NULL;

    // Typed map key check when receiver is a simple variable.
    if (expr->object && expr->object->type == EXPR_VARIABLE) {
        VariableRef recvVar = findVariableExpr(compiler, expr->object);
        if (recvVar.value && recvVar.isTypedMap && recvVar.mapKeyType) {
            if (!typedMapKeyCompatible(compiler, recvVar.mapKeyType, keyExpr)) {
                compilerErrorAt(compiler, expr->index ? expr->index->token.line : expr->base.token.line,
                                "typed map key type mismatch");
                return NULL;
            }
        }
    }

    LLVMValueRef key = tuaValueFromKey(compiler, keyExpr);
    if (!key) return NULL;

    // Determine V for typed maps when receiver is a simple variable.
    LLVMTypeRef innerType = compilerGetTuaValueType(compiler);
    if (expr->object && expr->object->type == EXPR_VARIABLE) {
        VariableRef recvVar = findVariableExpr(compiler, expr->object);
        if (recvVar.value && recvVar.isTypedMap && recvVar.mapValueType) {
            innerType = recvVar.mapValueType;
        }
    }

    LLVMValueRef okPtr = LLVMBuildAlloca(builder, LLVMInt32TypeInContext(compiler->context), "mokptr");
    LLVMValueRef getFn = getOrCreateTuaMapGetWithOk(compiler);
    LLVMTypeRef getType = LLVMGlobalGetValueType(getFn);
    LLVMValueRef args3[3] = { obj, key, okPtr };
    LLVMValueRef tv = LLVMBuildCall2(builder, getType, getFn, args3, 3, "mget");
    LLVMValueRef ok32 = LLVMBuildLoad2(builder, LLVMInt32TypeInContext(compiler->context), okPtr, "mok32");
    LLVMValueRef ok = LLVMBuildTrunc(builder, ok32, LLVMInt1TypeInContext(compiler->context), "mok");

    LLVMValueRef payload = NULL;
    LLVMTypeRef vt = compilerGetTuaValueType(compiler);
    if (innerType == vt) {
        payload = tv;
    } else {
        LLVMValueRef fn = compiler->current->func;
        LLVMBasicBlockRef someBB = LLVMAppendBasicBlock(fn, "opt.some");
        LLVMBasicBlockRef noneBB = LLVMAppendBasicBlock(fn, "opt.none");
        LLVMBasicBlockRef contBB = LLVMAppendBasicBlock(fn, "opt.cont");
        LLVMBuildCondBr(builder, ok, someBB, noneBB);

        LLVMPositionBuilderAtEnd(builder, someBB);
        LLVMValueRef someV = castFromTuaValue(compiler, tv, innerType);
        LLVMBuildBr(builder, contBB);
        LLVMBasicBlockRef someEnd = LLVMGetInsertBlock(builder);

        LLVMPositionBuilderAtEnd(builder, noneBB);
        LLVMValueRef noneV = LLVMConstNull(innerType);
        LLVMBuildBr(builder, contBB);
        LLVMBasicBlockRef noneEnd = LLVMGetInsertBlock(builder);

        LLVMPositionBuilderAtEnd(builder, contBB);
        LLVMValueRef phi = LLVMBuildPhi(builder, innerType, "optv");
        LLVMAddIncoming(phi, &someV, &someEnd, 1);
        LLVMAddIncoming(phi, &noneV, &noneEnd, 1);
        payload = phi;
    }

    LLVMTypeRef optType = compilerGetOptionType(compiler, innerType);
    LLVMValueRef opt = LLVMGetUndef(optType);
    opt = LLVMBuildInsertValue(builder, opt, ok, 0, "o0");
    opt = LLVMBuildInsertValue(builder, opt, payload, 1, "o1");
    return opt;
}

LLVMValueRef emitIndexSetExpr(Compiler* compiler, IndexSetExpr* expr) {
    if (!compiler || !expr) return NULL;
    LLVMBuilderRef builder = compiler->builder;
    LLVMContextRef context = compiler->context;
    LLVMTypeRef mapType = compilerGetMapType(compiler);

    LLVMValueRef keyExpr = compileExpr(compiler, expr->index);
    if (!keyExpr) return NULL;

    LLVMValueRef rawValue = compileExpr(compiler, expr->value);
    if (!rawValue) return NULL;

    LLVMValueRef objVal = NULL;
    int canAutoInit = expr->object && expr->object->type == EXPR_VARIABLE;
    LLVMTypeRef expectedKeyTy = NULL;
    LLVMTypeRef expectedValTy = NULL;

    if (canAutoInit) {
        VariableExpr* ve = (VariableExpr*)expr->object;
        VariableRef var = findVariableExpr(compiler, (Expr*)ve);
        if (!var.value || !var.type) {
            error("Undefined map variable\n");
            return NULL;
        }
        if (var.isConst) {
            error("Cannot assign into const map\n");
            return NULL;
        }
        if (var.type != mapType) {
            error("Index assignment target is not a map\n");
            return NULL;
        }
        if (var.isTypedMap) {
            expectedKeyTy = var.mapKeyType;
            expectedValTy = var.mapValueType;
        }

        // Load current map pointer.
        if (var.isBoxed) {
            LLVMValueRef cell = LLVMBuildLoad2(builder, var.boxPtrType, var.value, "cell");
            objVal = LLVMBuildLoad2(builder, mapType, cell, "mval");
        } else {
            objVal = LLVMBuildLoad2(builder, mapType, var.value, "mval");
        }

        LLVMValueRef isNull = LLVMBuildICmp(builder, LLVMIntEQ, objVal, LLVMConstNull(mapType), "isnull");

        LLVMBasicBlockRef current = LLVMGetInsertBlock(builder);
        LLVMValueRef fn = compiler->current->func;
        LLVMBasicBlockRef initBB = LLVMAppendBasicBlock(fn, "map.init");
        LLVMBasicBlockRef contBB = LLVMAppendBasicBlock(fn, "map.cont");
        LLVMBuildCondBr(builder, isNull, initBB, contBB);

        // initBB: m = tua_map_new()
        LLVMPositionBuilderAtEnd(builder, initBB);
        LLVMValueRef newFn = getOrCreateTuaMapNew(compiler);
        LLVMValueRef newMap = LLVMBuildCall2(builder, LLVMGlobalGetValueType(newFn), newFn, NULL, 0, "newmap");
        newMap = castToType(compiler, newMap, mapType);
        if (var.isBoxed) {
            LLVMValueRef cell = LLVMBuildLoad2(builder, var.boxPtrType, var.value, "cell2");
            LLVMBuildStore(builder, newMap, cell);
        } else {
            LLVMBuildStore(builder, newMap, var.value);
        }
        LLVMBuildBr(builder, contBB);

        // contBB: reload map pointer (now guaranteed non-null)
        LLVMPositionBuilderAtEnd(builder, contBB);
        if (var.isBoxed) {
            LLVMValueRef cell = LLVMBuildLoad2(builder, var.boxPtrType, var.value, "cell3");
            objVal = LLVMBuildLoad2(builder, mapType, cell, "mval2");
        } else {
            objVal = LLVMBuildLoad2(builder, mapType, var.value, "mval2");
        }

        (void)current;
    } else {
        objVal = compileExpr(compiler, expr->object);
        if (!objVal) return NULL;
        if (LLVMTypeOf(objVal) != mapType) {
            error("Index assignment target is not a map\n");
            return NULL;
        }
    }

    if (expectedKeyTy) {
        if (!typedMapKeyCompatible(compiler, expectedKeyTy, keyExpr)) {
            compilerErrorAt(compiler, expr->index ? expr->index->token.line : expr->base.token.line,
                            "typed map key type mismatch");
            return NULL;
        }
    }
    if (expectedValTy) {
        if (expr->value && expr->value->type == EXPR_LITERAL) {
            LiteralExpr* lit = (LiteralExpr*)expr->value;
            if (lit->value.type == TOKEN_NULL) {
                compilerErrorAt(compiler, lit->value.line, "cannot assign null into typed map value");
                return NULL;
            }
        }
        if (!typedMapValueCompatible(compiler, expectedValTy, rawValue)) {
            compilerErrorAt(compiler, expr->value ? expr->value->token.line : expr->base.token.line,
                            "typed map value type mismatch");
            return NULL;
        }
        rawValue = castToType(compiler, rawValue, expectedValTy);
    }

    LLVMValueRef key = tuaValueFromKey(compiler, keyExpr);
    if (!key) return NULL;
    LLVMValueRef v = tuaValueFromValue(compiler, rawValue);
    if (!v) return NULL;

    LLVMValueRef setFn = getOrCreateTuaMapSet(compiler);
    LLVMTypeRef setType = LLVMGlobalGetValueType(setFn);
    LLVMValueRef args[3] = { objVal, key, v };
    LLVMBuildCall2(builder, setType, setFn, args, 3, "");

    // Return assigned value as tua_value for potential chaining.
    return v;
}

LLVMValueRef emitAssignExpr(Compiler* compiler, AssignExpr* expr) {
    emitDebug("emitAssignExpr\n");
    
    // Find variable reference
    VariableRef var = (VariableRef){NULL, 0, NULL, NULL, NULL, 0, 0, 0, 0, NULL};
    Block* block = compiler->current;
    while (block != NULL) {
        var = findVariableWithLength(block->variables, expr->name.start, expr->name.length);
        if (var.value) break;
        block = block->parent;
    }
    if (!var.value && compiler->currentModulePrefix) {
        int ql = 0;
        char* q = compilerQualifyToken(compiler, &expr->name, &ql);
        if (q) {
            block = compiler->current;
            while (block != NULL) {
                var = findVariableWithLength(block->variables, q, ql);
                if (var.value) break;
                block = block->parent;
            }
            free(q);
        }
    }
    if (!var.value) {
        error("Undefined variable\n");
        return NULL;
    }

    // Check if variable is const
    if (var.isConst) {
        error("Cannot assign to const variable\n");
        return NULL;
    }
    // Compile value to be assigned
    LLVMValueRef value = compileExpr(compiler, expr->value);
    if (!value) {
        error("Failed to compile Assign expr\n");
        return NULL;
    }
    value = castToType(compiler, value, var.type);

    if (var.isBoxed) {
        if (!var.boxPtrType) {
            error("Missing boxed pointer type metadata\n");
            return NULL;
        }
        LLVMValueRef ptr = LLVMBuildLoad2(compiler->builder, var.boxPtrType, var.value, "boxptr");
        LLVMBuildStore(compiler->builder, value, ptr);
    } else {
        LLVMBuildStore(compiler->builder, value, var.value);
    }

    // Best-effort: keep closure call signature in sync across assignments.
    if (var.type == compilerGetClosureType(compiler)) {
        LLVMTypeRef rhsSig = NULL;
        if (expr->value && expr->value->type == EXPR_LAMBDA) {
            rhsSig = compiler->lastLambdaFuncType;
        } else if (expr->value && expr->value->type == EXPR_VARIABLE) {
            VariableExpr* ve = (VariableExpr*)expr->value;
            rhsSig = compilerFindClosureSig(compiler, ve->name.start, ve->name.length);
        }
        if (rhsSig) compilerRegisterClosureSig(compiler, expr->name.start, expr->name.length, rhsSig);
    }
    
    // Return value for chained assignments
    emitDebug("emitAssignExpr end\n");
    return value;
}

static int tokenEqualsN(const Token* token, const char* s) {
    int n = (int)strlen(s);
    return token->length == n && memcmp(token->start, s, (size_t)n) == 0;
}

static int fieldIndexOf(StructInfo* info, const Token* fieldName) {
    if (!info || !info->decl || !info->decl->fields) return -1;
    for (int i = 0; i < info->decl->fields->length; i++) {
        FieldDeclaration* f = listGet(info->decl->fields, i);
        if (!f) continue;
        if (f->name.length != fieldName->length) continue;
        if (memcmp(f->name.start, fieldName->start, (size_t)fieldName->length) == 0) return i;
    }
    return -1;
}

static int enumVariantIntTagOf(EnumInfo* info, const Token* variantName) {
    if (!info || !info->decl || !info->decl->variants) return -1;
    int current = -1;
    for (int i = 0; i < info->decl->variants->length; i++) {
        EnumVariantDecl* v = listGet(info->decl->variants, i);
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
        if (v->name.length != variantName->length) continue;
        if (memcmp(v->name.start, variantName->start, (size_t)variantName->length) == 0) return current;
    }
    return -1;
}

static LLVMValueRef enumVariantStringTagOf(Compiler* compiler, EnumInfo* info, const Token* variantName) {
    if (!compiler || !info || !info->decl || !info->decl->variants) return NULL;
    for (int i = 0; i < info->decl->variants->length; i++) {
        EnumVariantDecl* v = listGet(info->decl->variants, i);
        if (!v) continue;
        if (v->name.length != variantName->length) continue;
        if (memcmp(v->name.start, variantName->start, (size_t)variantName->length) != 0) continue;

        if (v->valueKind == ENUM_VALUE_STRING) {
            int len = v->value.length - 2; // strip quotes
            if (len < 0) len = 0;
            char* raw = malloc((size_t)len + 1);
            if (len > 0) memcpy(raw, v->value.start + 1, (size_t)len);
            raw[len] = '\0';
            LLVMValueRef str = LLVMBuildGlobalStringPtr(compiler->builder, raw, "enum_tag");
            free(raw);
            return str;
        }

        char* variantNameC = malloc((size_t)v->name.length + 1);
        memcpy(variantNameC, v->name.start, (size_t)v->name.length);
        variantNameC[v->name.length] = '\0';
        LLVMValueRef str = LLVMBuildGlobalStringPtr(compiler->builder, variantNameC, "enum_tag");
        free(variantNameC);
        return str;
    }
    return NULL;
}

static LLVMTypeRef fieldLLVMType(Compiler* compiler, StructInfo* info, int idx) {
    if (!info || !info->decl || !info->decl->fields) return LLVMInt32TypeInContext(compiler->context);
    FieldDeclaration* f = listGet(info->decl->fields, idx);
    if (!f || !f->type) return LLVMInt32TypeInContext(compiler->context);
    // Only primitives + named pointers are supported for now.
    switch (f->type->kind) {
        case TYPE_INT: return LLVMInt32TypeInContext(compiler->context);
        case TYPE_LONG: return LLVMInt64TypeInContext(compiler->context);
        case TYPE_DOUBLE: return LLVMDoubleTypeInContext(compiler->context);
        case TYPE_BOOL: return LLVMInt1TypeInContext(compiler->context);
        case TYPE_STRING: return LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
        case TYPE_NAMED: {
            StructInfo* inner = compilerFindStruct(compiler, f->type->name.start, f->type->name.length);
            if (!inner) return LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
            return inner->type;
        }
        case TYPE_REF: {
            // Pointer to inner type
            if (!f->type->inner) return LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
            if (f->type->inner->kind == TYPE_NAMED) {
                StructInfo* inner = compilerFindStruct(compiler, f->type->inner->name.start, f->type->inner->name.length);
                if (!inner) return LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
                return LLVMPointerType(inner->type, 0);
            }
            // Fallback for refs to primitives
            switch (f->type->inner->kind) {
                case TYPE_INT: return LLVMPointerType(LLVMInt32TypeInContext(compiler->context), 0);
                case TYPE_LONG: return LLVMPointerType(LLVMInt64TypeInContext(compiler->context), 0);
                case TYPE_DOUBLE: return LLVMPointerType(LLVMDoubleTypeInContext(compiler->context), 0);
                case TYPE_BOOL: return LLVMPointerType(LLVMInt1TypeInContext(compiler->context), 0);
                case TYPE_STRING: return LLVMPointerType(LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0), 0);
                default: return LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
            }
        }
        default: return LLVMInt32TypeInContext(compiler->context);
    }
}

static LLVMValueRef castToType(Compiler* compiler, LLVMValueRef value, LLVMTypeRef targetType) {
    if (!value) return NULL;
    LLVMTypeRef srcType = LLVMTypeOf(value);
    if (srcType == targetType) return value;

    value = castFromTuaValue(compiler, value, targetType);
    if (!value) return NULL;
    srcType = LLVMTypeOf(value);
    if (srcType == targetType) return value;

    LLVMTypeKind srcKind = LLVMGetTypeKind(srcType);
    LLVMTypeKind dstKind = LLVMGetTypeKind(targetType);

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
    if (srcKind == LLVMIntegerTypeKind && dstKind == LLVMDoubleTypeKind) {
        return LLVMBuildSIToFP(compiler->builder, value, targetType, "sitofp");
    }
    if (srcKind == LLVMDoubleTypeKind && dstKind == LLVMIntegerTypeKind) {
        return LLVMBuildFPToSI(compiler->builder, value, targetType, "fptosi");
    }
    return value;
}

LLVMValueRef emitGetExpr(Compiler* compiler, GetExpr* expr) {
    emitDebug("emitGetExpr\n");
    if (!expr || !expr->object) return NULL;

    // Only support member access on variables for now.
    if (expr->object->type != EXPR_VARIABLE) {
        error("Member access receiver must be a variable for now\n");
        return NULL;
    }

    VariableExpr* recv = (VariableExpr*)expr->object;
    VariableRef recvVar = findVariableExpr(compiler, expr->object);
    if (!recvVar.value) {
        // Support enum variant access: `Enum.Variant`
        EnumInfo* enumInfo = compilerResolveEnumByToken(compiler, &recv->name);
        if (!enumInfo) {
            error("Undefined receiver\n");
            return NULL;
        }
        if (enumInfo->isStringTag) {
            LLVMValueRef tag = enumVariantStringTagOf(compiler, enumInfo, &expr->name);
            if (!tag) {
                error("Unknown enum variant\n");
                return NULL;
            }
            return tag;
        } else {
            int idx = enumVariantIntTagOf(enumInfo, &expr->name);
            if (idx < 0) {
                error("Unknown enum variant\n");
                return NULL;
            }
            return LLVMConstInt(LLVMInt32TypeInContext(compiler->context), (uint64_t)idx, 0);
        }
    }
    if (!recvVar.typeName) {
        error("Receiver has no struct type info\n");
        return NULL;
    }

    StructInfo* info = compilerFindStruct(compiler, recvVar.typeName, recvVar.typeNameLength);
    if (!info) {
        error("Unknown struct type\n");
        return NULL;
    }

    int idx = fieldIndexOf(info, &expr->name);
    if (idx < 0) {
        error("Unknown field\n");
        return NULL;
    }

    LLVMValueRef structPtr = NULL;
    if (recvVar.isBoxed) {
        // Boxed variable slot stores pointer-to-cell.
        if (!recvVar.boxPtrType) {
            error("Missing boxed pointer type for receiver\n");
            return NULL;
        }
        LLVMValueRef cellPtr = LLVMBuildLoad2(compiler->builder, recvVar.boxPtrType, recvVar.value, "cellptr");
        if (LLVMGetTypeKind(recvVar.type) == LLVMPointerTypeKind) {
            // Cell contains a pointer value (e.g. &T)
            structPtr = LLVMBuildLoad2(compiler->builder, recvVar.type, cellPtr, "recv_ptr");
        } else {
            // Cell is the struct storage itself.
            structPtr = cellPtr;
        }
    } else if (LLVMGetTypeKind(recvVar.type) == LLVMPointerTypeKind) {
        // Variable holds a pointer value (e.g. `&A`), so load it from its slot.
        structPtr = LLVMBuildLoad2(compiler->builder, recvVar.type, recvVar.value, "recv_ptr");
    } else {
        // Variable holds a struct value, so its alloca is already a pointer to the struct.
        structPtr = recvVar.value;
    }

    LLVMValueRef fieldPtr = LLVMBuildStructGEP2(compiler->builder, info->type, structPtr, (unsigned)idx, "field_ptr");
    LLVMTypeRef fType = fieldLLVMType(compiler, info, idx);
    return LLVMBuildLoad2(compiler->builder, fType, fieldPtr, "field");
}

LLVMValueRef emitSetExpr(Compiler* compiler, SetExpr* expr) {
    emitDebug("emitSetExpr\n");
    if (!expr || !expr->object) return NULL;

    if (expr->object->type != EXPR_VARIABLE) {
        error("Member assignment receiver must be a variable for now\n");
        return NULL;
    }

    VariableExpr* recv = (VariableExpr*)expr->object;
    VariableRef recvVar = findVariableExpr(compiler, expr->object);
    if (!recvVar.value) {
        error("Undefined receiver\n");
        return NULL;
    }
    if (!recvVar.typeName) {
        error("Receiver has no struct type info\n");
        return NULL;
    }

    StructInfo* info = compilerFindStruct(compiler, recvVar.typeName, recvVar.typeNameLength);
    if (!info) {
        error("Unknown struct type\n");
        return NULL;
    }

    int idx = fieldIndexOf(info, &expr->name);
    if (idx < 0) {
        error("Unknown field\n");
        return NULL;
    }

    LLVMValueRef structPtr = NULL;
    if (recvVar.isBoxed) {
        if (!recvVar.boxPtrType) {
            error("Missing boxed pointer type for receiver\n");
            return NULL;
        }
        LLVMValueRef cellPtr = LLVMBuildLoad2(compiler->builder, recvVar.boxPtrType, recvVar.value, "cellptr");
        if (LLVMGetTypeKind(recvVar.type) == LLVMPointerTypeKind) {
            structPtr = LLVMBuildLoad2(compiler->builder, recvVar.type, cellPtr, "recv_ptr");
        } else {
            structPtr = cellPtr;
        }
    } else if (LLVMGetTypeKind(recvVar.type) == LLVMPointerTypeKind) {
        structPtr = LLVMBuildLoad2(compiler->builder, recvVar.type, recvVar.value, "recv_ptr");
    } else {
        structPtr = recvVar.value;
    }

    LLVMValueRef fieldPtr = LLVMBuildStructGEP2(compiler->builder, info->type, structPtr, (unsigned)idx, "field_ptr");
    LLVMTypeRef fType = fieldLLVMType(compiler, info, idx);

    LLVMValueRef rhs = compileExpr(compiler, expr->value);
    rhs = castToType(compiler, rhs, fType);
    LLVMBuildStore(compiler->builder, rhs, fieldPtr);
    return rhs;
}

LLVMValueRef emitUnaryExpr(Compiler* compiler, UnaryExpr* expr) {
    // Compile right expression
    LLVMBuilderRef builder = compiler->builder;
    

    switch (expr->operator.type) {
        case TOKEN_AMP: {
            // Address-of: currently supports variables only.
            if (!expr->right || expr->right->type != EXPR_VARIABLE) {
                error("Address-of expects a variable for now\n");
                return NULL;
            }
            VariableRef var = findVariableExpr(compiler, expr->right);
            if (!var.value) {
                error("Undefined variable in address-of\n");
                return NULL;
            }
            if (var.isBoxed) {
                if (!var.boxPtrType) {
                    error("Missing boxed pointer type metadata\n");
                    return NULL;
                }
                return LLVMBuildLoad2(compiler->builder, var.boxPtrType, var.value, "boxptr");
            }
            return var.value;
        }
        case TOKEN_MINUS: {
            LLVMValueRef operand = compileExpr(compiler, expr->right);
            if (!operand) {
                error("Failed to compile right operand");
                return NULL;
            }
            // 数值取反
            LLVMTypeRef type = LLVMTypeOf(operand);
            if (LLVMGetTypeKind(type) == LLVMIntegerTypeKind) {
                return LLVMBuildNeg(builder, operand, "neg");
            } else if (LLVMGetTypeKind(type) == LLVMDoubleTypeKind) {
                return LLVMBuildFNeg(builder, operand, "fneg");
            }
            error("Invalid operand type for unary minus");
            return NULL;
        }
        
        case TOKEN_NOT: {
            LLVMValueRef operand = compileExpr(compiler, expr->right);
            if (!operand) {
                error("Failed to compile right operand");
                return NULL;
            }
            LLVMValueRef b = llvmCoerceToBool(compiler, operand);
            if (!b) {
                error("Operand cannot be coerced to bool for logical not");
                return NULL;
            }
            return LLVMBuildNot(builder, b, "not");
        }
        
        // case TOKEN_BITNOT: {
        //     // 按位取反
        //     if (LLVMGetTypeKind(LLVMTypeOf(operand)) != LLVMIntegerTypeKind) {
        //         error("Operand must be integer for bitwise not");
        //         return NULL;
        //     }
        //     return LLVMBuildNot(builder, operand, "bitnot");
        // }
        
        default:
            error("Unknown unary operator");
            return NULL;
    }
}


LLVMValueRef emitPostfixExpr(Compiler* compiler, PostfixExpr* expr) {
    emitDebug("emitPostfixExpr start\n");
    LLVMBuilderRef builder = compiler->builder;
    
    // 处理变量表达式
    if (expr->operand->type == EXPR_VARIABLE) {
        VariableRef var = findVariableExpr(compiler, expr->operand);
        
        if (!var.value) {
            error("Undefined variable in postfix expression");
            return NULL;
        }
        if (!var.type) {
            error("Missing variable type in postfix expression");
            return NULL;
        }
        
        LLVMValueRef currentValue = NULL;
        if (var.isBoxed) {
            if (!var.boxPtrType) {
                error("Missing boxed pointer type in postfix expression");
                return NULL;
            }
            LLVMValueRef ptr = LLVMBuildLoad2(builder, var.boxPtrType, var.value, "boxptr");
            currentValue = LLVMBuildLoad2(builder, var.type, ptr, "load");
        } else {
            currentValue = LLVMBuildLoad2(builder, var.type, var.value, "load");
        }
        
        // 创建增减后的值
        LLVMValueRef newValue;
        switch (expr->operator.type) {
            case TOKEN_INC:
                newValue = LLVMBuildAdd(builder, currentValue, 
                    LLVMConstInt(var.type, 1, 0),
                    "inc");
                break;
            case TOKEN_DEC:
                newValue = LLVMBuildSub(builder, currentValue,
                    LLVMConstInt(var.type, 1, 0),
                    "dec");
                break;
            default:
                error("Unknown postfix operator");
                return NULL;
        }
        
        // 存储新值
        if (var.isBoxed) {
            LLVMValueRef ptr = LLVMBuildLoad2(builder, var.boxPtrType, var.value, "boxptr");
            LLVMBuildStore(builder, newValue, ptr);
        } else {
            LLVMBuildStore(builder, newValue, var.value);
        }
        
        // 返回原值（后缀操作返回操作前的值）
        return currentValue;
    }
    
    error("Invalid postfix expression operand");
    return NULL;
}

LLVMValueRef emitPrefixExpr(Compiler* compiler, PrefixExpr* expr) {
    emitDebug("emitPrefixExpr start\n");
    
    LLVMBuilderRef builder = compiler->builder;
    
    // 处理变量表达式
    if (expr->operand->type == EXPR_VARIABLE) {
        VariableRef var = findVariableExpr(compiler, expr->operand);
        
        if (!var.value) {
            error("Undefined variable in prefix expression");
            return NULL;
        }
        if (!var.type) {
            error("Missing variable type in prefix expression");
            return NULL;
        }
        
        // 加载当前值
        LLVMValueRef currentValue = NULL;
        if (var.isBoxed) {
            if (!var.boxPtrType) {
                error("Missing boxed pointer type in prefix expression");
                return NULL;
            }
            LLVMValueRef ptr = LLVMBuildLoad2(builder, var.boxPtrType, var.value, "boxptr");
            currentValue = LLVMBuildLoad2(builder, var.type, ptr, "load");
        } else {
            currentValue = LLVMBuildLoad2(builder, var.type, var.value, "load");
        }
        
        // 创建增减后的值
        LLVMValueRef newValue;
        switch (expr->operator.type) {
            case TOKEN_INC:
                newValue = LLVMBuildAdd(builder, currentValue,
                    LLVMConstInt(var.type, 1, 0),
                    "inc");
                break;
            case TOKEN_DEC:
                newValue = LLVMBuildSub(builder, currentValue,
                    LLVMConstInt(var.type, 1, 0),
                    "dec");
                break;
            default:
                error("Unknown prefix operator");
                return NULL;
        }
        
        // 存储新值
        if (var.isBoxed) {
            LLVMValueRef ptr = LLVMBuildLoad2(builder, var.boxPtrType, var.value, "boxptr");
            LLVMBuildStore(builder, newValue, ptr);
        } else {
            LLVMBuildStore(builder, newValue, var.value);
        }
        
        // 返回新值（前缀操作返回操作后的值）
        return newValue;
    }
    
    error("Invalid prefix expression operand");
    return NULL;
}

LLVMValueRef emitLambdaExpr(Compiler* compiler, LambdaExpr* expr) {
    emitDebug("emitLambdaExpr\n");
    if (!compiler || !expr) return NULL;

    // Compute direct free variable names (excluding nested lambdas).
    List* freeNames = computeLambdaFreeNames(compiler, expr);

    LLVMContextRef context = compiler->context;
    LLVMBuilderRef builder = compiler->builder;

    int id = compiler->lambdaCount++;

    // Name the lambda function and its env struct for debugging.
    char fnNameBuf[256];
    if (compiler->currentModulePrefix && compiler->currentModulePrefixLen > 0) {
        snprintf(fnNameBuf, sizeof(fnNameBuf), "%.*s__lambda_%d", compiler->currentModulePrefixLen, compiler->currentModulePrefix, id);
    } else {
        snprintf(fnNameBuf, sizeof(fnNameBuf), "__lambda_%d", id);
    }

    char envNameBuf[256];
    snprintf(envNameBuf, sizeof(envNameBuf), "__env_%d", id);

    // Build env type (struct of pointers to captured cells).
    LLVMTypeRef envType = NULL;
    LLVMTypeRef envPtrType = NULL;
    LLVMTypeRef* envFieldTypes = NULL;
    LLVMTypeRef* envValueTypes = NULL;
    int captureCount = freeNames ? freeNames->length : 0;

    if (captureCount > 0) {
        envType = LLVMStructCreateNamed(context, envNameBuf);
        envFieldTypes = malloc(sizeof(LLVMTypeRef) * (size_t)captureCount);
        envValueTypes = malloc(sizeof(LLVMTypeRef) * (size_t)captureCount);

        for (int i = 0; i < captureCount; i++) {
            Token* t = (Token*)listGet(freeNames, i);
            VariableExpr ve;
            memset(&ve, 0, sizeof(ve));
            ve.base.type = EXPR_VARIABLE;
            ve.name = *t;
            VariableRef ref = findVariableExpr(compiler, (Expr*)&ve);
            if (!ref.value || !ref.type) {
                // Unresolved capture: treat as opaque pointer.
                envFieldTypes[i] = LLVMPointerType(LLVMInt8TypeInContext(context), 0);
                envValueTypes[i] = LLVMPointerType(LLVMInt8TypeInContext(context), 0);
                continue;
            }
            envValueTypes[i] = ref.type;
            envFieldTypes[i] = ref.isBoxed ? ref.boxPtrType : LLVMPointerType(ref.type, 0);
        }
        LLVMStructSetBody(envType, envFieldTypes, (unsigned)captureCount, 0);
        envPtrType = LLVMPointerType(envType, 0);
    }

    // Build lambda function type: (env, args...) -> ret
    int paramCount = expr->params ? expr->params->length : 0;
    int totalParams = paramCount + 1;

    LLVMTypeRef* paramTypes = malloc(sizeof(LLVMTypeRef) * (size_t)totalParams);
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(context), 0);
    paramTypes[0] = envPtrType ? envPtrType : i8ptr;

    for (int i = 0; i < paramCount; i++) {
        Parameter* p = listGet(expr->params, i);
        paramTypes[i + 1] = lambdaTypeToLLVMType(compiler, p ? p->type : NULL, false);
    }

    LLVMTypeRef retType = LLVMVoidTypeInContext(context);
    int multiCount = 0;
    if (expr->returnTypes && expr->returnTypes->length > 1) {
        multiCount = expr->returnTypes->length;
        LLVMTypeRef* rts = malloc(sizeof(LLVMTypeRef) * (size_t)multiCount);
        for (int i = 0; i < multiCount; i++) {
            Type* t = listGet(expr->returnTypes, i);
            rts[i] = lambdaTypeToLLVMType(compiler, t, false);
        }
        retType = LLVMStructTypeInContext(context, rts, (unsigned)multiCount, 0);
        free(rts);
    } else {
        retType = lambdaTypeToLLVMType(compiler, expr->returnType, true);
    }

    LLVMTypeRef fnType = LLVMFunctionType(retType, paramTypes, (unsigned)totalParams, 0);
    LLVMValueRef fn = LLVMAddFunction(compiler->module, fnNameBuf, fnType);
    compiler->lastLambdaFuncType = fnType;
    if (multiCount > 1) {
        compilerRegisterMultiReturn(compiler, fnNameBuf, (int)strlen(fnNameBuf), multiCount);
    }

    // Compile lambda body in its own function.
    LLVMBasicBlockRef savedBlock = LLVMGetInsertBlock(builder);
    Block* savedCurrent = compiler->current;
    int savedBox = compiler->boxAllLocals;
    int savedLastSetLine = compiler->lastSetLine;
    compiler->boxAllLocals = 1; // simplest safe default for closures

    LLVMBasicBlockRef entry = LLVMAppendBasicBlock(fn, "entry");
    LLVMPositionBuilderAtEnd(builder, entry);
    compiler->lastSetLine = 0;

    Block* funcBlock = malloc(sizeof(Block));
    funcBlock->parent = savedCurrent;
    funcBlock->func = fn;
    funcBlock->variables = listNew();
    funcBlock->labels = listNew();
    compiler->current = funcBlock;

    // Bind captured variables as boxed locals pointing to env cells.
    if (captureCount > 0 && envType) {
        LLVMValueRef envArg = LLVMGetParam(fn, 0);
        for (int i = 0; i < captureCount; i++) {
            Token* t = (Token*)listGet(freeNames, i);
            if (!t) continue;
            LLVMTypeRef cellPtrType = envFieldTypes ? envFieldTypes[i] : i8ptr;
            LLVMValueRef fieldPtr = LLVMBuildStructGEP2(builder, envType, envArg, (unsigned)i, "cap_gep");
            LLVMValueRef cellPtr = LLVMBuildLoad2(builder, cellPtrType, fieldPtr, "cap");

            char* localName = malloc((size_t)t->length + 1);
            memcpy(localName, t->start, (size_t)t->length);
            localName[t->length] = '\0';
            LLVMValueRef slot = LLVMBuildAlloca(builder, cellPtrType, localName);
            LLVMBuildStore(builder, cellPtr, slot);

            VariableRef* vr = malloc(sizeof(VariableRef));
            vr->name = localName;
            vr->length = t->length;
            vr->value = slot;
            vr->type = envValueTypes ? envValueTypes[i] : LLVMPointerType(LLVMInt8TypeInContext(context), 0);
            vr->typeName = NULL;
            vr->typeNameLength = 0;
            vr->isConst = 0;
            vr->isGlobal = 0;
            vr->isBoxed = 1;
            vr->boxPtrType = cellPtrType;
            listAppend(funcBlock->variables, vr);
        }
    }

    // Bind parameters (boxed).
    for (int i = 0; i < paramCount; i++) {
        Parameter* p = listGet(expr->params, i);
        LLVMValueRef arg = LLVMGetParam(fn, (unsigned)(i + 1));

        char* paramName = malloc((size_t)p->name.length + 1);
        memcpy(paramName, p->name.start, (size_t)p->name.length);
        paramName[p->name.length] = '\0';

        LLVMTypeRef vType = paramTypes[i + 1];
        LLVMTypeRef cellPtrType = LLVMPointerType(vType, 0);
        LLVMValueRef slot = LLVMBuildAlloca(builder, cellPtrType, paramName);

        LLVMValueRef mallocFn = getOrCreateMalloc(compiler);
        LLVMValueRef sizeV = LLVMSizeOf(vType);
        LLVMValueRef raw = LLVMBuildCall2(builder, LLVMGlobalGetValueType(mallocFn), mallocFn, &sizeV, 1, "malloc");
        LLVMValueRef cell = LLVMBuildBitCast(builder, raw, cellPtrType, "cell");
        LLVMBuildStore(builder, arg, cell);
        LLVMBuildStore(builder, cell, slot);

        VariableRef* variable = malloc(sizeof(VariableRef));
        variable->name = paramName;
        variable->length = p->name.length;
        variable->value = slot;
        variable->type = vType;
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
        variable->isBoxed = 1;
        variable->boxPtrType = cellPtrType;
        listAppend(funcBlock->variables, variable);
    }

    // Compile body statements
    for (ListNode* node = expr->body ? expr->body->head : NULL; node != NULL; node = node->next) {
        if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(builder))) break;
        compileStmt(compiler, (Stmt*)node->data);
    }

    // Implicit return
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(builder))) {
        if (LLVMGetTypeKind(retType) == LLVMVoidTypeKind) {
            LLVMBuildRetVoid(builder);
        } else {
            LLVMBuildRet(builder, LLVMConstNull(retType));
        }
    }

    // Restore insertion point and compiler state.
    compiler->current = savedCurrent;
    compiler->boxAllLocals = savedBox;
    LLVMPositionBuilderAtEnd(builder, savedBlock);
    compiler->lastSetLine = savedLastSetLine;

    // Allocate env instance and pack closure value.
    LLVMTypeRef closureType = compilerGetClosureType(compiler);
    LLVMValueRef closure = LLVMGetUndef(closureType);

    LLVMValueRef fnPtr = LLVMBuildBitCast(builder, fn, i8ptr, "fnptr");
    LLVMValueRef envPtrI8 = LLVMConstNull(i8ptr);

    if (captureCount > 0 && envType && envPtrType) {
        LLVMValueRef mallocFn = getOrCreateMalloc(compiler);
        LLVMValueRef sizeV = LLVMSizeOf(envType);
        LLVMValueRef raw = LLVMBuildCall2(builder, LLVMGlobalGetValueType(mallocFn), mallocFn, &sizeV, 1, "malloc");
        LLVMValueRef envPtr = LLVMBuildBitCast(builder, raw, envPtrType, "env");

        for (int i = 0; i < captureCount; i++) {
            Token* t = (Token*)listGet(freeNames, i);
            if (!t) continue;
            VariableExpr ve;
            memset(&ve, 0, sizeof(ve));
            ve.base.type = EXPR_VARIABLE;
            ve.name = *t;
            VariableRef ref = findVariableExpr(compiler, (Expr*)&ve);
            if (!ref.value || !ref.type) continue;

            LLVMTypeRef fieldType = envFieldTypes ? envFieldTypes[i] : i8ptr;
            LLVMValueRef cellPtr = NULL;
            if (ref.isBoxed) {
                cellPtr = LLVMBuildLoad2(builder, ref.boxPtrType, ref.value, "cap_cell");
            } else {
                cellPtr = ref.value;
            }
            cellPtr = LLVMBuildBitCast(builder, cellPtr, fieldType, "cap_cast");
            LLVMValueRef fieldPtr = LLVMBuildStructGEP2(builder, envType, envPtr, (unsigned)i, "env_gep");
            LLVMBuildStore(builder, cellPtr, fieldPtr);
        }

        envPtrI8 = LLVMBuildBitCast(builder, envPtr, i8ptr, "env_i8");
    }

    closure = LLVMBuildInsertValue(builder, closure, fnPtr, 0, "c0");
    closure = LLVMBuildInsertValue(builder, closure, envPtrI8, 1, "c1");

    // If this lambda was used as initializer, the surrounding var-stmt can consume this.
    compiler->lastLambdaFuncType = fnType;

    if (paramTypes) free(paramTypes);
    if (envFieldTypes) free(envFieldTypes);
    if (envValueTypes) free(envValueTypes);
    if (freeNames) freeTokenSet(freeNames);
    return closure;
}
