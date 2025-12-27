#include "llvm.h"
#include "compiler.h"
#include "debug.h"

static LLVMValueRef coerceToBool(Compiler* compiler, LLVMValueRef value) {
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

    emitDebug("Unsupported condition type in if\n");
    return NULL;
}

void emitIfStmt(Compiler* compiler, IfStmt* stmt) {
    emitDebug("emitIfStmt\n");

    LLVMValueRef function = compiler->current->func;
    LLVMBuilderRef builder = compiler->builder;

    LLVMValueRef condValue = compileExpr(compiler, stmt->condition);
    condValue = coerceToBool(compiler, condValue);
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

