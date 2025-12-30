#include "llvm.h"
#include "compiler.h"
#include "debug.h"

static LoopTarget* currentLoop(Compiler* compiler) {
    if (!compiler || !compiler->loopStack || compiler->loopStack->length == 0) return NULL;
    return (LoopTarget*)compiler->loopStack->tail->data;
}

static void pushLoop(Compiler* compiler, LLVMBasicBlockRef breakTarget, LLVMBasicBlockRef continueTarget) {
    LoopTarget* t = malloc(sizeof(LoopTarget));
    t->breakTarget = breakTarget;
    t->continueTarget = continueTarget;
    listAppend(compiler->loopStack, t);
}

static void popLoop(Compiler* compiler) {
    if (!compiler || !compiler->loopStack || compiler->loopStack->length == 0) return;
    LoopTarget* t = (LoopTarget*)listPop(compiler->loopStack);
    free(t);
}

void emitBreakStmt(Compiler* compiler) {
    LoopTarget* t = currentLoop(compiler);
    if (!t || !t->breakTarget) {
        error("break used outside of loop\n");
        return;
    }
    if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(compiler->builder))) return;
    LLVMBuildBr(compiler->builder, t->breakTarget);
}

void emitContinueStmt(Compiler* compiler) {
    LoopTarget* t = currentLoop(compiler);
    if (!t || !t->continueTarget) {
        error("continue used outside of loop\n");
        return;
    }
    if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(compiler->builder))) return;
    LLVMBuildBr(compiler->builder, t->continueTarget);
}

// Expose loop stack helpers for loop emitters in this translation unit.
void llvmPushLoop(Compiler* compiler, LLVMBasicBlockRef breakTarget, LLVMBasicBlockRef continueTarget) {
    pushLoop(compiler, breakTarget, continueTarget);
}

void llvmPopLoop(Compiler* compiler) {
    popLoop(compiler);
}
