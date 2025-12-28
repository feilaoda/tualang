#include "llvm.h"
#include "compiler.h"
#include "debug.h"

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

    if (kind == LLVMDoubleTypeKind) {
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
