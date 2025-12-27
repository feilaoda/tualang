#include "llvm.h"
#include "compiler.h"
#include "debug.h"


void emitVarStmt(Compiler* compiler, VarStmt* stmt) {
    emitDebug("emitVarStmt\n");
    // int a = 10;
    char *var = malloc(stmt->name.length + 1);
    // char var[255] = {0};
    memcpy(var, stmt->name.start, stmt->name.length);
    var[stmt->name.length] = '\0';
    emitDebug("emitVarStmt var name:%s\n", var);
    LLVMValueRef a = LLVMBuildAlloca(compiler->builder, LLVMInt32TypeInContext(compiler->context), var);
    if(stmt->initializer != NULL) {
        emitDebug("emitVarStmt: init %.*s type:%d\n", stmt->name.length, stmt->name.start, stmt->initializer->type);
        if(stmt->initializer->type == EXPR_LITERAL) {
            LiteralExpr * intExpr = (LiteralExpr *)stmt->initializer;
            emitDebug("emitVarStmt LiteralExpr type:%d value:%d\n", intExpr->value.type,tokenToValue(intExpr->value).as.i);
            if(intExpr->value.type == TOKEN_INT) {
                emitDebug("emitVarStmt TOKEN_INT type:%d value:%d\n", intExpr->value.type,tokenToValue(intExpr->value).as.i);
                LLVMValueRef ten = LLVMConstInt(LLVMInt32TypeInContext(compiler->context), tokenToValue(intExpr->value).as.i, 0);
                LLVMBuildStore(compiler->builder, ten, a);
            }
        }
    }
    Block * block = compiler->current;
    VariableRef * variable = malloc(sizeof(VariableRef));
    variable->name = var;
    variable->length = stmt->name.length;
    variable->value = a;
    listAppend(block->variables, variable);
}
