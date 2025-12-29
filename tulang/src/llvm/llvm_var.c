#include "llvm.h"
#include "compiler.h"
#include "debug.h"

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

static LLVMValueRef getOrCreateMalloc(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "malloc");
    if (existing) return existing;
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef i64 = LLVMInt64TypeInContext(compiler->context);
    LLVMTypeRef fnType = LLVMFunctionType(i8ptr, &i64, 1, 0);
    return LLVMAddFunction(compiler->module, "malloc", fnType);
}

static LLVMTypeRef toLLVMType(Compiler* compiler, Type* type) {
    if (!type) return LLVMInt32TypeInContext(compiler->context);

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
                    // Best-effort: Option without type arg defaults to tua_value.
                    return compilerGetOptionType(compiler, compilerGetTuaValueType(compiler));
                }
                LLVMTypeRef innerTy = toLLVMType(compiler, inner);
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
            LLVMTypeRef inner = toLLVMType(compiler, type->inner);
            return LLVMPointerType(inner, 0);
        }
        case TYPE_FUNC:
            return compilerGetClosureType(compiler);
        default:
            return LLVMInt32TypeInContext(compiler->context);
    }
}

static LLVMTypeRef inferLLVMTypeFromInitializer(Compiler* compiler, Expr* initializer) {
    if (!initializer) return LLVMInt32TypeInContext(compiler->context);

    if (initializer->type == EXPR_LAMBDA) {
        return compilerGetClosureType(compiler);
    }
    if (initializer->type == EXPR_MAP_LITERAL) {
        return compilerGetMapType(compiler);
    }
    if (initializer->type == EXPR_INDEX) {
        // Default to Option<tua_value>; if receiver is a typed map variable, infer Option<V>.
        IndexExpr* ix = (IndexExpr*)initializer;
        LLVMTypeRef vt = compilerGetTuaValueType(compiler);
        if (ix->object && ix->object->type == EXPR_VARIABLE) {
            VariableRef rv = findVariableExpr(compiler, ix->object);
            if (rv.value && rv.isTypedMap && rv.mapValueType) {
                return compilerGetOptionType(compiler, rv.mapValueType);
            }
        }
        return compilerGetOptionType(compiler, vt);
    }
    if (initializer->type == EXPR_INDEX_SET) {
        return compilerGetTuaValueType(compiler);
    }

    if (initializer->type == EXPR_VARIABLE) {
        VariableRef ref = findVariableExpr(compiler, initializer);
        if (ref.value && ref.type) return ref.type;
    }

    if (initializer->type == EXPR_CALL) {
        CallExpr* call = (CallExpr*)initializer;
        if (call->callee && call->callee->type == EXPR_GET) {
            GetExpr* get = (GetExpr*)call->callee;
            // m.get(k) -> Option<V>
            if (get->object) {
                LLVMTypeRef mapType = compilerGetMapType(compiler);
                LLVMTypeRef vt = compilerGetTuaValueType(compiler);

                if (get->object->type == EXPR_VARIABLE) {
                    VariableRef rv = findVariableExpr(compiler, get->object);
                    if (rv.value && rv.type == mapType) {
                        LLVMTypeRef inner = (rv.isTypedMap && rv.mapValueType) ? rv.mapValueType : vt;
                        return compilerGetOptionType(compiler, inner);
                    }
                }
            }
        }
        if (call->callee && call->callee->type == EXPR_VARIABLE) {
            VariableExpr* callee = (VariableExpr*)call->callee;
            StructInfo* info = compilerResolveStructByToken(compiler, &callee->name);
            if (info) {
                return info->type;
            }
        }
    }

    if (initializer->type == EXPR_UNARY) {
        UnaryExpr* un = (UnaryExpr*)initializer;
        if (un->operator.type == TOKEN_AMP && un->right && un->right->type == EXPR_VARIABLE) {
            VariableRef base = findVariableExpr(compiler, un->right);
            if (base.value && base.type) {
                return LLVMPointerType(base.type, 0);
            }
        }
    }

    if (initializer->type == EXPR_GET) {
        GetExpr* get = (GetExpr*)initializer;
        if (get->object && get->object->type == EXPR_VARIABLE) {
            VariableExpr* recv = (VariableExpr*)get->object;
            EnumInfo* info = compilerResolveEnumByToken(compiler, &recv->name);
            if (info) {
                if (info->isStringTag) {
                    return LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
                }
                return LLVMInt32TypeInContext(compiler->context);
            }
        }
    }

    if (initializer->type != EXPR_LITERAL) return LLVMInt32TypeInContext(compiler->context);

    LiteralExpr* literal = (LiteralExpr*)initializer;
    switch (literal->value.type) {
        case TOKEN_INT:
            return LLVMInt32TypeInContext(compiler->context);
        case TOKEN_LONG:
            return LLVMInt64TypeInContext(compiler->context);
        case TOKEN_DOUBLE:
            return LLVMDoubleTypeInContext(compiler->context);
        case TOKEN_STRING_LITERAL:
            return LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
        case TOKEN_TRUE:
        case TOKEN_FALSE:
            return LLVMInt1TypeInContext(compiler->context);
        default:
            return LLVMInt32TypeInContext(compiler->context);
    }
}

static int inferTypedMapKVFromLiteral(Compiler* compiler, MapLiteralExpr* lit, LLVMTypeRef* outKeyTy, LLVMTypeRef* outValTy) {
    if (!compiler || !lit || !outKeyTy || !outValTy) return 0;
    *outKeyTy = NULL;
    *outValTy = NULL;

    // key inference (string vs numeric only; mixed => no inference)
    int sawStringKey = 0;
    int sawNumericKey = 0;
    int sawLongKey = 0;
    for (ListNode* n = lit->entries ? lit->entries->head : NULL; n != NULL; n = n->next) {
        MapEntry* e = (MapEntry*)n->data;
        if (!e) continue;
        if (e->key.type == TOKEN_STRING_LITERAL) {
            sawStringKey = 1;
        } else if (e->key.type == TOKEN_INT) {
            sawNumericKey = 1;
        } else if (e->key.type == TOKEN_LONG) {
            sawNumericKey = 1;
            sawLongKey = 1;
        }
    }
    if (sawStringKey && sawNumericKey) return 0;
    if (sawStringKey) {
        *outKeyTy = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    } else {
        *outKeyTy = sawLongKey ? LLVMInt64TypeInContext(compiler->context)
                               : LLVMInt32TypeInContext(compiler->context);
    }

    // value inference: only when all values are non-null literals of a consistent family.
    int sawInt = 0, sawLong = 0, sawDouble = 0, sawBool = 0, sawString = 0;
    for (ListNode* n = lit->entries ? lit->entries->head : NULL; n != NULL; n = n->next) {
        MapEntry* e = (MapEntry*)n->data;
        if (!e || !e->value) return 0;
        if (e->value->type != EXPR_LITERAL) return 0;
        LiteralExpr* v = (LiteralExpr*)e->value;
        switch (v->value.type) {
            case TOKEN_INT: sawInt = 1; break;
            case TOKEN_LONG: sawLong = 1; break;
            case TOKEN_DOUBLE: sawDouble = 1; break;
            case TOKEN_TRUE:
            case TOKEN_FALSE: sawBool = 1; break;
            case TOKEN_STRING_LITERAL: sawString = 1; break;
            case TOKEN_NULL:
            default:
                return 0;
        }
    }

    int families = 0;
    if (sawBool) families++;
    if (sawString) families++;
    if (sawInt || sawLong || sawDouble) families++;
    if (families != 1) return 0;

    if (sawString) {
        *outValTy = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
        return 1;
    }
    if (sawBool) {
        *outValTy = LLVMInt1TypeInContext(compiler->context);
        return 1;
    }
    if (sawDouble) {
        *outValTy = LLVMDoubleTypeInContext(compiler->context);
        return 1;
    }
    if (sawLong) {
        *outValTy = LLVMInt64TypeInContext(compiler->context);
        return 1;
    }
    *outValTy = LLVMInt32TypeInContext(compiler->context);
    return 1;
}

static LLVMValueRef castIfNeeded(Compiler* compiler, LLVMValueRef value, LLVMTypeRef targetType) {
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
            LLVMValueRef someV = castIfNeeded(compiler, payload, dstInner);
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

    // Keep it simple for now; extend as language grows.
    return value;
}

void emitVarStmt(Compiler* compiler, VarStmt* stmt) {
    emitDebug("emitVarStmt\n");
    char *var = malloc(stmt->name.length + 1);
    memcpy(var, stmt->name.start, stmt->name.length);
    var[stmt->name.length] = '\0';
    emitDebug("emitVarStmt var name:%s\n", var);
    LLVMTypeRef valueType = stmt->type ? toLLVMType(compiler, stmt->type) : inferLLVMTypeFromInitializer(compiler, stmt->initializer);

    int hasAnnotatedTypedMap = 0;
    LLVMTypeRef annotatedKeyTy = NULL;
    LLVMTypeRef annotatedValTy = NULL;
    if (stmt->type && stmt->type->kind == TYPE_NAMED &&
        stmt->type->name.length == 3 && memcmp(stmt->type->name.start, "map", 3) == 0 &&
        stmt->type->typeArgs && stmt->type->typeArgs->length == 2) {
        Type* kAst = (Type*)stmt->type->typeArgs->head->data;
        Type* vAst = (Type*)stmt->type->typeArgs->head->next->data;
        int okKey = kAst && (kAst->kind == TYPE_STRING || kAst->kind == TYPE_INT || kAst->kind == TYPE_LONG);
        int okVal = vAst && (vAst->kind == TYPE_STRING || vAst->kind == TYPE_INT || vAst->kind == TYPE_LONG ||
                             vAst->kind == TYPE_DOUBLE || vAst->kind == TYPE_BOOL);
        if (okKey && okVal) {
            hasAnnotatedTypedMap = 1;
            annotatedKeyTy = toLLVMType(compiler, kAst);
            annotatedValTy = toLLVMType(compiler, vAst);
        }
    }

    int inferredTypedMap = 0;
    LLVMTypeRef inferredKeyTy = NULL;
    LLVMTypeRef inferredValTy = NULL;
    if (!stmt->type && stmt->initializer && stmt->initializer->type == EXPR_MAP_LITERAL) {
        inferredTypedMap = inferTypedMapKVFromLiteral(compiler, (MapLiteralExpr*)stmt->initializer, &inferredKeyTy, &inferredValTy);
    }

    int shouldBox = compiler && compiler->boxAllLocals;
    LLVMTypeRef boxPtrType = shouldBox ? LLVMPointerType(valueType, 0) : NULL;
    LLVMTypeRef slotElemType = shouldBox ? boxPtrType : valueType;
    LLVMValueRef slot = LLVMBuildAlloca(compiler->builder, slotElemType, var);

    LLVMTypeRef compiledLambdaSig = NULL;
    LLVMTypeRef declaredSig = NULL;
    if (stmt->type && stmt->type->kind == TYPE_FUNC) {
        declaredSig = compilerClosureSigFromType(compiler, stmt->type);
    }
    if (stmt->initializer != NULL) {
        emitDebug("emitVarStmt: init %.*s type:%d\n", stmt->name.length, stmt->name.start, stmt->initializer->type);
        LLVMTypeRef savedKey = compiler->expectedMapKeyType;
        LLVMTypeRef savedVal = compiler->expectedMapValueType;
        if (valueType == compilerGetMapType(compiler) && stmt->initializer->type == EXPR_MAP_LITERAL) {
            if (hasAnnotatedTypedMap) {
                compiler->expectedMapKeyType = annotatedKeyTy;
                compiler->expectedMapValueType = annotatedValTy;
            } else if (inferredTypedMap) {
                compiler->expectedMapKeyType = inferredKeyTy;
                compiler->expectedMapValueType = inferredValTy;
            }
        }

        LLVMValueRef initValue = compileExpr(compiler, stmt->initializer);

        compiler->expectedMapKeyType = savedKey;
        compiler->expectedMapValueType = savedVal;
        if (stmt->initializer->type == EXPR_LAMBDA) {
            compiledLambdaSig = compiler->lastLambdaFuncType;
            if (declaredSig && compiledLambdaSig && declaredSig != compiledLambdaSig) {
                emitDebug("emitVarStmt: function type annotation does not match lambda signature for %s\n", var);
            }
        } else if (stmt->initializer->type == EXPR_VARIABLE && valueType == compilerGetClosureType(compiler)) {
            VariableExpr* ve = (VariableExpr*)stmt->initializer;
            LLVMTypeRef baseSig = compilerFindClosureSig(compiler, ve->name.start, ve->name.length);
            if (baseSig) compiledLambdaSig = baseSig;
        }
        initValue = castIfNeeded(compiler, initValue, valueType);
        if (shouldBox) {
            LLVMValueRef mallocFn = getOrCreateMalloc(compiler);
            LLVMValueRef sizeV = LLVMSizeOf(valueType);
            LLVMValueRef raw = LLVMBuildCall2(
                compiler->builder,
                LLVMGlobalGetValueType(mallocFn),
                mallocFn,
                &sizeV,
                1,
                "malloc"
            );
            LLVMValueRef cell = LLVMBuildBitCast(compiler->builder, raw, boxPtrType, "cell");
            if (initValue) LLVMBuildStore(compiler->builder, initValue, cell);
            LLVMBuildStore(compiler->builder, cell, slot);
        } else {
            if (initValue) LLVMBuildStore(compiler->builder, initValue, slot);
        }
    } else if (shouldBox) {
        LLVMValueRef mallocFn = getOrCreateMalloc(compiler);
        LLVMValueRef sizeV = LLVMSizeOf(valueType);
        LLVMValueRef raw = LLVMBuildCall2(
            compiler->builder,
            LLVMGlobalGetValueType(mallocFn),
            mallocFn,
            &sizeV,
            1,
            "malloc"
        );
        LLVMValueRef cell = LLVMBuildBitCast(compiler->builder, raw, boxPtrType, "cell");
        LLVMBuildStore(compiler->builder, LLVMConstNull(valueType), cell);
        LLVMBuildStore(compiler->builder, cell, slot);
    } else {
        // Make `map` locals safely default to null when uninitialized.
        if (valueType == compilerGetMapType(compiler)) {
            LLVMBuildStore(compiler->builder, LLVMConstNull(valueType), slot);
        }
    }
    Block * block = compiler->current;
    VariableRef * variable = malloc(sizeof(VariableRef));
    variable->name = var;
    variable->length = stmt->name.length;
    variable->value = slot;
    variable->type = valueType;
    if (stmt->type && stmt->type->kind == TYPE_NAMED) {
        StructInfo* info = compilerResolveStructByToken(compiler, &stmt->type->name);
        if (info) {
            variable->typeName = info->name;
            variable->typeNameLength = info->nameLength;
        } else {
            variable->typeName = stmt->type->name.start;
            variable->typeNameLength = stmt->type->name.length;
        }
    } else if (stmt->type && stmt->type->kind == TYPE_REF && stmt->type->inner && stmt->type->inner->kind == TYPE_NAMED) {
        StructInfo* info = compilerResolveStructByToken(compiler, &stmt->type->inner->name);
        if (info) {
            variable->typeName = info->name;
            variable->typeNameLength = info->nameLength;
        } else {
            variable->typeName = stmt->type->inner->name.start;
            variable->typeNameLength = stmt->type->inner->name.length;
        }
    } else if (stmt->initializer && stmt->initializer->type == EXPR_CALL) {
        CallExpr* call = (CallExpr*)stmt->initializer;
        if (call->callee && call->callee->type == EXPR_VARIABLE) {
            VariableExpr* callee = (VariableExpr*)call->callee;
            StructInfo* info = compilerResolveStructByToken(compiler, &callee->name);
            if (info) {
                variable->typeName = info->name;
                variable->typeNameLength = info->nameLength;
            } else {
                variable->typeName = NULL;
                variable->typeNameLength = 0;
            }
        } else if (call->callee && call->callee->type == EXPR_GET) {
            // Namespace-qualified struct constructor: `import "m" as ns; let v = ns.User(...)`
            GetExpr* get = (GetExpr*)call->callee;
            if (get->object && get->object->type == EXPR_VARIABLE) {
                VariableExpr* ns = (VariableExpr*)get->object;
                SymbolAlias* a = compilerFindAlias(compiler, ns->name.start, ns->name.length);
                if (a && a->kind == ALIAS_MODULE) {
                    int ql = 0;
                    char* q = mangleRawAndToken(a->qualified, a->qualifiedLen, &get->name, &ql);
                    StructInfo* info = compilerFindStruct(compiler, q, ql);
                    free(q);
                    if (info) {
                        variable->typeName = info->name;
                        variable->typeNameLength = info->nameLength;
                    } else {
                        variable->typeName = NULL;
                        variable->typeNameLength = 0;
                    }
                } else {
                    variable->typeName = NULL;
                    variable->typeNameLength = 0;
                }
            } else {
                variable->typeName = NULL;
                variable->typeNameLength = 0;
            }
        } else {
            variable->typeName = NULL;
            variable->typeNameLength = 0;
        }
    } else if (stmt->initializer && stmt->initializer->type == EXPR_UNARY) {
        UnaryExpr* un = (UnaryExpr*)stmt->initializer;
        if (un->operator.type == TOKEN_AMP && un->right && un->right->type == EXPR_VARIABLE) {
            VariableRef base = findVariableExpr(compiler, un->right);
            if (base.value && base.typeName) {
                variable->typeName = base.typeName;
                variable->typeNameLength = base.typeNameLength;
            } else {
                variable->typeName = NULL;
                variable->typeNameLength = 0;
            }
        } else {
            variable->typeName = NULL;
            variable->typeNameLength = 0;
        }
    } else if (stmt->initializer && stmt->initializer->type == EXPR_VARIABLE) {
        VariableRef base = findVariableExpr(compiler, stmt->initializer);
        if (base.value && base.typeName) {
            variable->typeName = base.typeName;
            variable->typeNameLength = base.typeNameLength;
        } else {
            variable->typeName = NULL;
            variable->typeNameLength = 0;
        }
    } else {
        variable->typeName = NULL;
        variable->typeNameLength = 0;
    }
    variable->isConst = stmt->isConst ? 1 : 0;
    variable->isGlobal = 0;
    variable->isBoxed = shouldBox ? 1 : 0;
    variable->boxPtrType = shouldBox ? boxPtrType : NULL;
    variable->isTypedMap = 0;
    variable->mapKeyType = NULL;
    variable->mapValueType = NULL;

    if (stmt->type && stmt->type->kind == TYPE_NAMED &&
        stmt->type->name.length == 3 && memcmp(stmt->type->name.start, "map", 3) == 0 &&
        stmt->type->typeArgs && stmt->type->typeArgs->length == 2) {
        Type* kAst = (Type*)stmt->type->typeArgs->head->data;
        Type* vAst = (Type*)stmt->type->typeArgs->head->next->data;
        int okKey = kAst && (kAst->kind == TYPE_STRING || kAst->kind == TYPE_INT || kAst->kind == TYPE_LONG);
        int okVal = vAst && (vAst->kind == TYPE_STRING || vAst->kind == TYPE_INT || vAst->kind == TYPE_LONG ||
                             vAst->kind == TYPE_DOUBLE || vAst->kind == TYPE_BOOL);
        if (!okKey) {
            error("map<K,V> key type must be string/int/long for now\n");
        } else if (!okVal) {
            error("map<K,V> value type must be int/long/double/bool/string for now\n");
        } else {
            variable->isTypedMap = 1;
            variable->mapKeyType = toLLVMType(compiler, kAst);
            variable->mapValueType = toLLVMType(compiler, vAst);
        }
    }

    if (!stmt->type && inferredTypedMap && valueType == compilerGetMapType(compiler) && inferredKeyTy && inferredValTy) {
        variable->isTypedMap = 1;
        variable->mapKeyType = inferredKeyTy;
        variable->mapValueType = inferredValTy;
    }
    listAppend(block->variables, variable);

    LLVMTypeRef finalSig = compiledLambdaSig ? compiledLambdaSig : declaredSig;
    if (finalSig) compilerRegisterClosureSig(compiler, variable->name, variable->length, finalSig);
}
