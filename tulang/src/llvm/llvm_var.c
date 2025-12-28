#include "llvm.h"
#include "compiler.h"
#include "debug.h"

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

    if (initializer->type == EXPR_VARIABLE) {
        VariableRef ref = findVariableExpr(compiler, initializer);
        if (ref.value && ref.type) return ref.type;
    }

    if (initializer->type == EXPR_CALL) {
        CallExpr* call = (CallExpr*)initializer;
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

static LLVMValueRef castIfNeeded(Compiler* compiler, LLVMValueRef value, LLVMTypeRef targetType) {
    if (!value) return NULL;
    LLVMTypeRef srcType = LLVMTypeOf(value);
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
        LLVMValueRef initValue = compileExpr(compiler, stmt->initializer);
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
    listAppend(block->variables, variable);

    LLVMTypeRef finalSig = compiledLambdaSig ? compiledLambdaSig : declaredSig;
    if (finalSig) compilerRegisterClosureSig(compiler, variable->name, variable->length, finalSig);
}
