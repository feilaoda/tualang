#include "llvm.h"
#include "compiler.h"
#include "debug.h"

static int isOptionLLVMType(LLVMTypeRef t);
static LLVMValueRef collapseMultiReturnIfNeeded(Compiler* compiler, LLVMValueRef func, LLVMValueRef call);

static int tokenEquals(const Token* token, const char* s) {
    int n = (int)strlen(s);
    return token->length == n && memcmp(token->start, s, (size_t)n) == 0;
}

static char* tokenToCString(const Token* token) {
    char* s = malloc((size_t)token->length + 1);
    memcpy(s, token->start, (size_t)token->length);
    s[token->length] = '\0';
    return s;
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
    switch (type->kind) {
        case TYPE_INT: return LLVMInt32TypeInContext(compiler->context);
        case TYPE_LONG: return LLVMInt64TypeInContext(compiler->context);
        case TYPE_DOUBLE: return LLVMDoubleTypeInContext(compiler->context);
        case TYPE_FLOAT: return LLVMFloatTypeInContext(compiler->context);
        case TYPE_BOOL: return LLVMInt1TypeInContext(compiler->context);
        case TYPE_STRING: return LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
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

static LLVMTypeRef resolveClosureReturnSigForSimpleCall(Compiler* compiler, CallExpr* call) {
    if (!compiler || !call || !call->callee) return NULL;
    if (call->callee->type != EXPR_VARIABLE) return NULL;

    VariableExpr* callee = (VariableExpr*)call->callee;

    SymbolAlias* a = compilerFindAlias(compiler, callee->name.start, callee->name.length);
    if (a && a->kind == ALIAS_FUNC) {
        return compilerFindClosureReturnSig(compiler, a->qualified, a->qualifiedLen);
    }

    if (compiler->currentModulePrefix) {
        int ql = 0;
        char* q = compilerQualifyToken(compiler, &callee->name, &ql);
        if (q) {
            LLVMTypeRef t = compilerFindClosureReturnSig(compiler, q, ql);
            free(q);
            if (t) return t;
        }
    }

    return compilerFindClosureReturnSig(compiler, callee->name.start, callee->name.length);
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
            LLVMValueRef argVal = compileExpr(compiler, (Expr*)node->data);
            argVal = castValueToType(compiler, argVal, paramTypes[i]);
            args[i] = argVal;
            node = node->next;
        }
    }

    LLVMValueRef call = LLVMBuildCall2(compiler->builder, funcType, func, args, expected, "call");
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

static int isNumericLLVMType(LLVMTypeRef t) {
    if (!t) return 0;
    LLVMTypeKind k = LLVMGetTypeKind(t);
    if (k != LLVMIntegerTypeKind) return 0;
    return LLVMGetIntTypeWidth(t) != 1;
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

    // Multi-return selection is only for the current call's return value.
    // Nested calls (arguments) should keep the default "first value" rule.
    int wantMultiForThisCall = compiler ? compiler->wantMultiValue : 0;
    if (compiler) compiler->wantMultiValue = 0;

    // Immediate lambda call: (fn(...) { ... })(args)
    if (expr->callee->type == EXPR_LAMBDA) {
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
                LLVMValueRef argVal = compileExpr(compiler, (Expr*)node->data);
                argVal = castValueToType(compiler, argVal, paramTypes[i]);
                args[i] = argVal;
                node = node->next;
            }
        }

        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
        LLVMValueRef call = LLVMBuildCall2(compiler->builder, fnType, fnPtr, args, expected, "call");
        LLVMTypeRef retType = LLVMGetReturnType(fnType);
        LLVMValueRef out = collapseMultiReturnByTypeIfNeeded(compiler, call, retType);
        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;

        if (paramTypes) free(paramTypes);
        if (args) free(args);
        return out;
    }

    // Call a closure value returned by a simple call: `makeAdder(1)(2)`
    // Requires that the inner call target has an explicit closure return type `(args)->ret`.
    Expr* calleeExpr = expr->callee;
    while (calleeExpr && calleeExpr->type == EXPR_GROUPING) {
        calleeExpr = ((GroupingExpr*)calleeExpr)->expression;
    }
    if (calleeExpr && calleeExpr->type == EXPR_CALL) {
        LLVMTypeRef fnType = resolveClosureReturnSigForSimpleCall(compiler, (CallExpr*)calleeExpr);
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
                    LLVMValueRef argVal = compileExpr(compiler, (Expr*)node->data);
                    argVal = castValueToType(compiler, argVal, paramTypes[i]);
                    args[i] = argVal;
                    node = node->next;
                }
            }

            LLVMValueRef call = LLVMBuildCall2(compiler->builder, fnType, fnPtr, args, expected, "call");
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
                LLVMValueRef func = LLVMGetNamedFunction(compiler->module, q);
                if (func) {
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

            // Option built-in methods: `opt.isSome()`, `opt.unwrap()`, ...
            if (isOptionLLVMType(recvType)) {
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

        // Array built-in methods: `a.len()`, `a.clone()`
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

            unsigned got = expr->arguments ? (unsigned)expr->arguments->length : 0;
            LLVMTypeRef i64 = LLVMInt64TypeInContext(compiler->context);
            LLVMTypeRef i32 = LLVMInt32TypeInContext(compiler->context);

            if (tokenEquals(&get->name, "len")) {
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

            if (tokenEquals(&get->name, "clone")) {
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

            if (tokenEquals(&get->name, "push")) {
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

            compilerErrorAt(compiler, get->name.line, "unknown array method: %.*s", get->name.length, get->name.start);
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }

        // Map built-in methods: `m.hasKey(k)`, `m.get(k)`, `m.len()`
        if (recvVar.value && recvVar.isMap) {
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

            if (tokenEquals(&get->name, "delete")) {
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

                LLVMTypeRef vt = compilerGetTuaValueType(compiler);
                LLVMTypeRef innerType = recvVar.isTypedMap && recvVar.mapValueType ? recvVar.mapValueType : vt;

                LLVMValueRef okPtr = LLVMBuildAlloca(compiler->builder, LLVMInt32TypeInContext(compiler->context), "mokptr");
                LLVMValueRef gfn = getOrCreateTuaMapGetWithOk(compiler);
                LLVMTypeRef gtype = LLVMGlobalGetValueType(gfn);
                LLVMValueRef args3[3] = { mapPtr, key, okPtr };
                LLVMValueRef tv = LLVMBuildCall2(compiler->builder, gtype, gfn, args3, 3, "mget");
                LLVMValueRef ok32 = LLVMBuildLoad2(compiler->builder, LLVMInt32TypeInContext(compiler->context), okPtr, "mok32");
                LLVMValueRef ok = LLVMBuildTrunc(compiler->builder, ok32, LLVMInt1TypeInContext(compiler->context), "mok");

                LLVMValueRef payload = NULL;
                if (innerType == vt) {
                    payload = tv;
                } else {
                    LLVMValueRef fn = compiler->current->func;
                    LLVMBasicBlockRef someBB = LLVMAppendBasicBlock(fn, "opt.some");
                    LLVMBasicBlockRef noneBB = LLVMAppendBasicBlock(fn, "opt.none");
                    LLVMBasicBlockRef contBB = LLVMAppendBasicBlock(fn, "opt.cont");
                    LLVMBuildCondBr(compiler->builder, ok, someBB, noneBB);

                    LLVMPositionBuilderAtEnd(compiler->builder, someBB);
                    LLVMValueRef someV = castValueToType(compiler, tv, innerType);
                    LLVMBuildBr(compiler->builder, contBB);
                    LLVMBasicBlockRef someEnd = LLVMGetInsertBlock(compiler->builder);

                    LLVMPositionBuilderAtEnd(compiler->builder, noneBB);
                    LLVMValueRef noneV = LLVMConstNull(innerType);
                    LLVMBuildBr(compiler->builder, contBB);
                    LLVMBasicBlockRef noneEnd = LLVMGetInsertBlock(compiler->builder);

                    LLVMPositionBuilderAtEnd(compiler->builder, contBB);
                    LLVMValueRef phi = LLVMBuildPhi(compiler->builder, innerType, "optv");
                    LLVMAddIncoming(phi, &someV, &someEnd, 1);
                    LLVMAddIncoming(phi, &noneV, &noneEnd, 1);
                    payload = phi;
                }

                LLVMTypeRef optType = compilerGetOptionType(compiler, innerType);
                LLVMValueRef opt = LLVMGetUndef(optType);
                opt = LLVMBuildInsertValue(compiler->builder, opt, ok, 0, "o0");
                opt = LLVMBuildInsertValue(compiler->builder, opt, payload, 1, "o1");
                if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                return opt;
            }
        }

        // Option built-in methods (variable receiver): `o.isSome()`, `o.unwrap()`, ...
        if (recvVar.value && isOptionLLVMType(recvVar.type)) {
            LLVMValueRef optVal = loadLocalValue(compiler, recvVar);
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

        bool isInstance = recvVar.value != NULL && recvVar.typeName != NULL;

        int mangledLen = 0;
        char* mangled = NULL;
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

        LLVMValueRef func = LLVMGetNamedFunction(compiler->module, mangled);
        free(mangled);

        if (!func) {
            emitDebug("Undefined object method\n");
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
                thisArg = castValueToType(compiler, thisArg, paramTypes[0]);
                args[argIndex++] = thisArg;
            }

            for (; argIndex < expected; argIndex++) {
                LLVMValueRef argVal = compileExpr(compiler, (Expr*)node->data);
                argVal = castValueToType(compiler, argVal, paramTypes[argIndex]);
                args[argIndex] = argVal;
                node = node->next;
            }
        }

        LLVMValueRef call = LLVMBuildCall2(compiler->builder, funcType, func, args, expected, "call");
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
    int isPrintln = tokenEquals(&callee->name, "println");
    int isPrint = tokenEquals(&callee->name, "print");
    int isAssert = tokenEquals(&callee->name, "assert");
    int isLen = tokenEquals(&callee->name, "len");
    int isSomeCtor = tokenEquals(&callee->name, "Some");
    int isNoneCtor = tokenEquals(&callee->name, "None");

    if (isSomeCtor) {
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

    if (isNoneCtor) {
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

    if (isAssert) {
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

    if (isLen) {
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
                    LLVMValueRef argVal = compileExpr(compiler, (Expr*)node->data);
                    argVal = castValueToType(compiler, argVal, paramTypes[i]);
                    args[i] = argVal;
                    node = node->next;
                }
            }

            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            LLVMValueRef call = LLVMBuildCall2(compiler->builder, fnType, fnPtr, args, expected, "call");
            LLVMTypeRef retType = LLVMGetReturnType(fnType);
            LLVMValueRef out = collapseMultiReturnByTypeIfNeeded(compiler, call, retType);
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;

            if (paramTypes) free(paramTypes);
            if (args) free(args);
            return out;
        }

        // Resolve functions with module-local qualified names taking precedence
        // over the generated entry `main` and over imported aliases.
        LLVMValueRef func = NULL;
        int foundQualified = 0;
        int foundAlias = 0;

        if (compiler->currentModulePrefix) {
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

        if (!func) {
            StructInfo* info = compilerResolveStructByToken(compiler, &callee->name);
            free(name);
            if (info) {
                LLVMValueRef out = emitStructConstructor(compiler, info, expr);
                if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
                return out;
            }
            compilerErrorAtToken(compiler, &callee->name, "undefined function '%.*s'",
                callee->name.length, callee->name.start);
            if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
            return NULL;
        }
        free(name);

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
                LLVMValueRef argVal = compileExpr(compiler, (Expr*)node->data);
                argVal = castValueToType(compiler, argVal, paramTypes[i]);
                args[i] = argVal;
                node = node->next;
            }
        }

        LLVMValueRef call = LLVMBuildCall2(compiler->builder, funcType, func, args, expected, "call");
        if (paramTypes) free(paramTypes);
        if (args) free(args);
        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
        LLVMValueRef out = collapseMultiReturnIfNeeded(compiler, func, call);
        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
        return out;
    }

    if (!expr->arguments || expr->arguments->length != 1) {
        compilerErrorAtToken(compiler, &callee->name, "%.*s expects exactly 1 argument", callee->name.length, callee->name.start);
        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
        return NULL;
    }

    LLVMValueRef printfFunc = getOrCreatePrintf(compiler);
    LLVMTypeRef printfType = getPrintfType(compiler);

    LLVMValueRef argValue = compileExpr(compiler, (Expr*)expr->arguments->head->data);
    if (argValue && LLVMTypeOf(argValue) == compilerGetTuaValueType(compiler)) {
        LLVMValueRef fn = getOrCreateTuaPrintValue(compiler);
        LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
        LLVMValueRef nl = LLVMConstInt(LLVMInt32TypeInContext(compiler->context), isPrintln ? 1 : 0, 0);
        LLVMValueRef args2[2] = { argValue, nl };
        LLVMBuildCall2(compiler->builder, fnType, fn, args2, 2, "");
        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
        return LLVMConstInt(LLVMInt32TypeInContext(compiler->context), 0, 0);
    }
    const char* fmt = formatForValue(argValue, isPrintln);
    if (!fmt) {
        emitDebug("Unsupported print argument type\n");
        if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
        return NULL;
    }
    argValue = castForPrintf(compiler, argValue);

    LLVMValueRef formatStr = LLVMBuildGlobalStringPtr(compiler->builder, fmt, "fmt");
    LLVMValueRef args[] = { formatStr, argValue };
    LLVMValueRef out = LLVMBuildCall2(compiler->builder, printfType, printfFunc, args, 2, "");
    if (compiler) compiler->wantMultiValue = wantMultiForThisCall;
    return out;
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
