#include "llvm.h"
#include "compiler.h"
#include "debug.h"

#include "tuac_alloc.h"

static int tokenEquals(const Token* token, const char* s) {
    if (!token || !s) return 0;
    size_t len = strlen(s);
    if ((size_t)token->length != len) return 0;
    return memcmp(token->start, s, len) == 0;
}

static LLVMTypeRef tuaBoxDropFnType(Compiler* compiler) {
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef params[1] = { i8ptr };
    return LLVMFunctionType(LLVMVoidTypeInContext(compiler->context), params, 1, 0);
}

static LLVMValueRef getOrCreateTuaBoxAlloc(Compiler* compiler) {
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

LLVMValueRef llvmCoerceToBool(Compiler* compiler, LLVMValueRef value) {
    if (!value) return NULL;

    LLVMTypeRef type = LLVMTypeOf(value);
    LLVMTypeKind kind = LLVMGetTypeKind(type);

    if (kind == LLVMIntegerTypeKind && LLVMGetIntTypeWidth(type) == 1) {
        return value;
    }

    if (kind == LLVMIntegerTypeKind) {
        LLVMValueRef zero = LLVMConstInt(type, 0, 0);
        return LLVMBuildICmp(compiler->builder, LLVMIntNE, value, zero, "to_bool");
    }

    if (kind == LLVMFloatTypeKind || kind == LLVMDoubleTypeKind) {
        LLVMValueRef zero = LLVMConstReal(type, 0.0);
        return LLVMBuildFCmp(compiler->builder, LLVMRealONE, value, zero, "to_bool");
    }

    if (kind == LLVMPointerTypeKind) {
        LLVMValueRef zero = LLVMConstNull(type);
        return LLVMBuildICmp(compiler->builder, LLVMIntNE, value, zero, "to_bool");
    }

    if (kind == LLVMStructTypeKind && compiler && type == compilerGetTuaValueType(compiler)) {
        // nil -> false; bool -> payload!=0; others -> true
        LLVMValueRef tag = LLVMBuildExtractValue(compiler->builder, value, 0, "tag");
        LLVMValueRef payload = LLVMBuildExtractValue(compiler->builder, value, 1, "payload");
        LLVMValueRef zeroTag = LLVMConstInt(LLVMTypeOf(tag), 0, 0);
        LLVMValueRef isNil = LLVMBuildICmp(compiler->builder, LLVMIntEQ, tag, zeroTag, "isnil");

        LLVMValueRef boolTag = LLVMConstInt(LLVMTypeOf(tag), 4, 0);
        LLVMValueRef isBool = LLVMBuildICmp(compiler->builder, LLVMIntEQ, tag, boolTag, "isbool");

        LLVMValueRef payloadNonZero = LLVMBuildICmp(
            compiler->builder,
            LLVMIntNE,
            payload,
            LLVMConstInt(LLVMTypeOf(payload), 0, 0),
            "p_nz"
        );

        // truthy = !isNil && (!isBool || payloadNonZero)
        LLVMValueRef notNil = LLVMBuildNot(compiler->builder, isNil, "notnil");
        LLVMValueRef boolOk = LLVMBuildOr(compiler->builder, LLVMBuildNot(compiler->builder, isBool, "notbool"), payloadNonZero, "boolok");
        return LLVMBuildAnd(compiler->builder, notNil, boolOk, "truthy");
    }

    if (kind == LLVMStructTypeKind && LLVMCountStructElementTypes(type) == 2) {
        // Option<T>: treat as truthy when isSome == true.
        LLVMTypeRef f0 = LLVMStructGetTypeAtIndex(type, 0);
        if (LLVMGetTypeKind(f0) == LLVMIntegerTypeKind && LLVMGetIntTypeWidth(f0) == 1) {
            return LLVMBuildExtractValue(compiler->builder, value, 0, "opt_ok");
        }
    }

    emitDebug("Unsupported condition type in if\n");
    return NULL;
}

void emitIfStmt(Compiler* compiler, IfStmt* stmt) {
    emitDebug("emitIfStmt\n");

    LLVMValueRef function = compiler->current->func;
    LLVMBuilderRef builder = compiler->builder;

    LLVMValueRef condValue = compileExpr(compiler, stmt->condition);
    condValue = llvmCoerceToBool(compiler, condValue);
    if (!condValue) {
        emitDebug("Failed to compile if condition\n");
        return;
    }

    LLVMBasicBlockRef thenBlock = LLVMAppendBasicBlock(function, "if.then");
    LLVMBasicBlockRef elseBlock = stmt->elseBranch ? LLVMAppendBasicBlock(function, "if.else") : NULL;
    LLVMBasicBlockRef mergeBlock = LLVMAppendBasicBlock(function, "if.end");

    LLVMBuildCondBr(builder, condValue, thenBlock, elseBlock ? elseBlock : mergeBlock);

    // Then
    LLVMPositionBuilderAtEnd(builder, thenBlock);
    compileStmt(compiler, stmt->thenBranch);
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(builder))) {
        LLVMBuildBr(builder, mergeBlock);
    }

    // Else
    if (elseBlock) {
        LLVMPositionBuilderAtEnd(builder, elseBlock);
        compileStmt(compiler, stmt->elseBranch);
        if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(builder))) {
            LLVMBuildBr(builder, mergeBlock);
        }
    }

    // Merge
    LLVMPositionBuilderAtEnd(builder, mergeBlock);
}

void emitIfLetStmt(Compiler* compiler, IfLetStmt* stmt) {
    if (!compiler || !stmt) return;
    LLVMBuilderRef builder = compiler->builder;
    LLVMValueRef function = compiler->current->func;

    LLVMValueRef optVal = compileExpr(compiler, stmt->value);
    if (!optVal) {
        compilerErrorAt(compiler, stmt->name.line, "failed to compile if-let value expression");
        return;
    }
    LLVMTypeRef optTy = LLVMTypeOf(optVal);
    if (LLVMGetTypeKind(optTy) != LLVMStructTypeKind || LLVMCountStructElementTypes(optTy) != 2) {
        compilerErrorAt(compiler, stmt->name.line, "if-let expects Option<T> on the right-hand side");
        return;
    }
    LLVMTypeRef okTy = LLVMStructGetTypeAtIndex(optTy, 0);
    if (LLVMGetTypeKind(okTy) != LLVMIntegerTypeKind || LLVMGetIntTypeWidth(okTy) != 1) {
        compilerErrorAt(compiler, stmt->name.line, "if-let expects Option<T> on the right-hand side");
        return;
    }

    LLVMValueRef ok = LLVMBuildExtractValue(builder, optVal, 0, "opt_ok");
    LLVMValueRef payload = LLVMBuildExtractValue(builder, optVal, 1, "opt_v");
    LLVMTypeRef valueType = LLVMStructGetTypeAtIndex(optTy, 1);

    LLVMBasicBlockRef thenBlock = LLVMAppendBasicBlock(function, "iflet.then");
    LLVMBasicBlockRef elseBlock = stmt->elseBranch ? LLVMAppendBasicBlock(function, "iflet.else") : NULL;
    LLVMBasicBlockRef mergeBlock = LLVMAppendBasicBlock(function, "iflet.end");

    LLVMBuildCondBr(builder, ok, thenBlock, elseBlock ? elseBlock : mergeBlock);

    // Then: bind name to payload and compile body in a nested scope.
    LLVMPositionBuilderAtEnd(builder, thenBlock);
    Block* saved = compiler->current;
    Block* scoped = (Block*)calloc(1, sizeof(Block));
    scoped->parent = saved;
    scoped->func = saved ? saved->func : NULL;
    scoped->variables = listNew();
    scoped->labels = saved ? saved->labels : listNew();
    compiler->current = scoped;

    int nameLen = stmt->name.length;
    char* name = (char*)malloc((size_t)nameLen + 1);
    memcpy(name, stmt->name.start, (size_t)nameLen);
    name[nameLen] = '\0';

    int shouldBox = compilerShouldBoxLocal(compiler, stmt->name.start, stmt->name.length);
    LLVMTypeRef boxPtrType = shouldBox ? LLVMPointerType(valueType, 0) : NULL;
    LLVMTypeRef slotElemType = shouldBox ? boxPtrType : valueType;
    LLVMValueRef slot = LLVMBuildAlloca(builder, slotElemType, name);

    if (shouldBox) {
        LLVMValueRef boxAlloc = getOrCreateTuaBoxAlloc(compiler);
        LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
        LLVMTypeRef i64 = LLVMInt64TypeInContext(compiler->context);
        LLVMValueRef sizeV = LLVMSizeOf(valueType);
        LLVMValueRef size64 = LLVMTypeOf(sizeV) == i64 ? sizeV : LLVMBuildZExt(builder, sizeV, i64, "bsz");
        LLVMTypeRef dropFnPtrTy = LLVMPointerType(tuaBoxDropFnType(compiler), 0);
        LLVMValueRef dropArg = LLVMConstNull(dropFnPtrTy);
        LLVMTypeRef allocTy = LLVMGlobalGetValueType(boxAlloc);
        LLVMValueRef args2[2] = { size64, dropArg };
        LLVMValueRef raw = LLVMBuildCall2(builder, allocTy, boxAlloc, args2, 2, "box");
        LLVMValueRef cell = LLVMBuildBitCast(builder, raw, boxPtrType, "cell");
        LLVMBuildStore(builder, payload, cell);
        LLVMBuildStore(builder, cell, slot);
        (void)i8ptr;
    } else {
        LLVMBuildStore(builder, payload, slot);
    }

    VariableRef* vr = (VariableRef*)calloc(1, sizeof(VariableRef));
    vr->name = name;
    vr->length = stmt->name.length;
    vr->value = slot;
    vr->type = valueType;
    vr->pointeeType = NULL;
    vr->typeKind = TYPE_ANY;
    vr->typeName = NULL;
    vr->typeNameLength = 0;
    vr->isConst = 0;
    vr->isBorrowed = 0;
    vr->isGlobal = 0;
    vr->isBoxed = shouldBox ? 1 : 0;
    vr->boxOwns = shouldBox ? 1 : 0;
    vr->boxPtrType = shouldBox ? boxPtrType : NULL;

    // If the RHS is `m.get(...)` / `m.getMut(...)`, treat the binding as an element reference:
    // - mark as borrowed (non-owning)
    // - set pointeeType and struct name for field/method resolution
    Expr* rhs = stmt->value;
    while (rhs && rhs->type == EXPR_GROUPING) rhs = ((GroupingExpr*)rhs)->expression;
    if (rhs && rhs->type == EXPR_CALL) {
        CallExpr* c = (CallExpr*)rhs;
        if (c->callee && c->callee->type == EXPR_GET) {
            GetExpr* g = (GetExpr*)c->callee;
            if ((tokenEquals(&g->name, "get") || tokenEquals(&g->name, "getMut")) &&
                g->object && g->object->type == EXPR_VARIABLE) {
                VariableRef mv = findVariableExpr(compiler, g->object);
                LLVMTypeRef vt = compilerGetTuaValueType(compiler);
                vr->isBorrowed = 1;
                vr->pointeeType = vt;
                if (mv.value && mv.isTypedMap && mv.mapValueType && mv.mapValueType != vt) {
                    vr->pointeeType = mv.mapValueType;
                    if (mv.mapValueTypeName) {
                        vr->typeName = mv.mapValueTypeName;
                        vr->typeNameLength = mv.mapValueTypeNameLength;
                    }
                }
            }
        }
    }

    listAppend(scoped->variables, vr);

    compileStmt(compiler, stmt->thenBranch);
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(builder))) {
        compilerEmitDropForBlockVars(compiler, scoped);
        LLVMBuildBr(builder, mergeBlock);
    }
    compiler->current = saved;

    // Else
    if (elseBlock) {
        LLVMPositionBuilderAtEnd(builder, elseBlock);
        compileStmt(compiler, stmt->elseBranch);
        if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(builder))) {
            LLVMBuildBr(builder, mergeBlock);
        }
    }

    LLVMPositionBuilderAtEnd(builder, mergeBlock);
}
