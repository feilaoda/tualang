#ifndef _LLVM_H_
#define _LLVM_H_
#include "compiler.h"


typedef struct VariableRef {
    const char* name;
    int length;
    LLVMValueRef value; 
}VariableRef;


VariableRef findVariable(List* variables, const char* name);
void emitVarStmt(Compiler* compiler, VarStmt* stmt);
void emitForStmt(Compiler* compiler, ForStmt* stmt);
LLVMValueRef emitExpr(Compiler* compiler, Expr* expr);
LLVMValueRef emitCallExpr(Compiler* compiler, CallExpr* expr);
#endif