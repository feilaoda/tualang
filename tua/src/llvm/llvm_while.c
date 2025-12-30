#include "llvm.h"
#include "compiler.h"
#include "debug.h"

void emitWhileStmt(Compiler* compiler, WhileStmt* stmt) {
    if (!compiler || !stmt) return;

    LLVMValueRef function = compiler->current->func;
    LLVMBuilderRef builder = compiler->builder;

    LLVMBasicBlockRef condBlock = LLVMAppendBasicBlock(function, "while.cond");
    LLVMBasicBlockRef bodyBlock = LLVMAppendBasicBlock(function, "while.body");
    LLVMBasicBlockRef endBlock = LLVMAppendBasicBlock(function, "while.end");

    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(builder))) {
        LLVMBuildBr(builder, condBlock);
    }

    LLVMPositionBuilderAtEnd(builder, condBlock);
    LLVMValueRef cond = compileExpr(compiler, stmt->condition);
    if (!cond) {
        error("Failed to compile while condition\n");
        LLVMBuildBr(builder, endBlock);
    } else {
        LLVMBuildCondBr(builder, cond, bodyBlock, endBlock);
    }

    LLVMPositionBuilderAtEnd(builder, bodyBlock);
    llvmPushLoop(compiler, endBlock, condBlock);
    compileStmt(compiler, stmt->body);
    llvmPopLoop(compiler);
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(builder))) {
        LLVMBuildBr(builder, condBlock);
    }

    LLVMPositionBuilderAtEnd(builder, endBlock);
}

void emitDoWhileStmt(Compiler* compiler, DoWhileStmt* stmt) {
    if (!compiler || !stmt) return;

    LLVMValueRef function = compiler->current->func;
    LLVMBuilderRef builder = compiler->builder;

    LLVMBasicBlockRef bodyBlock = LLVMAppendBasicBlock(function, "do.body");
    LLVMBasicBlockRef condBlock = LLVMAppendBasicBlock(function, "do.cond");
    LLVMBasicBlockRef endBlock = LLVMAppendBasicBlock(function, "do.end");

    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(builder))) {
        LLVMBuildBr(builder, bodyBlock);
    }

    LLVMPositionBuilderAtEnd(builder, bodyBlock);
    llvmPushLoop(compiler, endBlock, condBlock);
    compileStmt(compiler, stmt->body);
    llvmPopLoop(compiler);
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(builder))) {
        LLVMBuildBr(builder, condBlock);
    }

    LLVMPositionBuilderAtEnd(builder, condBlock);
    LLVMValueRef cond = compileExpr(compiler, stmt->condition);
    if (!cond) {
        error("Failed to compile do-while condition\n");
        LLVMBuildBr(builder, endBlock);
    } else {
        LLVMBuildCondBr(builder, cond, bodyBlock, endBlock);
    }

    LLVMPositionBuilderAtEnd(builder, endBlock);
}

