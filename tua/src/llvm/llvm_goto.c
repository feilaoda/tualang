#include "llvm.h"
#include "compiler.h"
#include "debug.h"

#include "tuac_alloc.h"

static LabelInfo* findLabel(Block* block, const char* name, int length) {
    if (!block || !block->labels) return NULL;
    for (int i = 0; i < block->labels->length; i++) {
        LabelInfo* info = listGet(block->labels, i);
        if (!info) continue;
        if (info->length != length) continue;
        if (memcmp(info->name, name, (size_t)length) == 0) return info;
    }
    return NULL;
}

static LabelInfo* getOrCreateLabel(Compiler* compiler, Token name) {
    Block* block = compiler->current;
    if (!block) return NULL;

    LabelInfo* existing = findLabel(block, name.start, name.length);
    if (existing) return existing;

    char* labelName = malloc((size_t)name.length + 1);
    memcpy(labelName, name.start, (size_t)name.length);
    labelName[name.length] = '\0';

    LLVMValueRef function = block->func;
    LLVMBasicBlockRef labelBlock = LLVMAppendBasicBlock(function, labelName);

    LabelInfo* info = malloc(sizeof(LabelInfo));
    info->name = labelName;
    info->length = name.length;
    info->block = labelBlock;
    info->isDefined = 0;
    listAppend(block->labels, info);
    return info;
}

void emitLabelStmt(Compiler* compiler, LabelStmt* stmt) {
    if (!compiler || !stmt) return;
    LabelInfo* info = getOrCreateLabel(compiler, stmt->name);
    if (!info) return;
    if (info->isDefined) {
        error("Duplicate label: %.*s\n", stmt->name.length, stmt->name.start);
        return;
    }
    info->isDefined = 1;

    LLVMBuilderRef builder = compiler->builder;
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(builder))) {
        LLVMBuildBr(builder, info->block);
    }
    LLVMPositionBuilderAtEnd(builder, info->block);
}

void emitGotoStmt(Compiler* compiler, GotoStmt* stmt) {
    if (!compiler || !stmt) return;
    LabelInfo* info = getOrCreateLabel(compiler, stmt->name);
    if (!info) return;

    LLVMBuilderRef builder = compiler->builder;
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(builder))) {
        LLVMBuildBr(builder, info->block);
    }

    // Continue emitting into a fresh block (unreachable unless jumped into).
    LLVMBasicBlockRef cont = LLVMAppendBasicBlock(compiler->current->func, "after.goto");
    LLVMPositionBuilderAtEnd(builder, cont);
}
