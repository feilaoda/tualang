#ifndef _LLVM_H_
#define _LLVM_H_
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "list.h"
#include "compiler.h"

#include <llvm-c/Core.h>
#include <llvm-c/Analysis.h>

#define emitDebug(...) debug(__VA_ARGS__)


typedef struct VariableRef {
    const char* name;
    int length;
    LLVMValueRef value;
    LLVMTypeRef type;
    const char* typeName;
    int typeNameLength;
    int isConst;
    int isGlobal;
}VariableRef;


VariableRef findVariable(List* variables, const char* name);
VariableRef findVariableExpr(Compiler* compiler, Expr* expr);
VariableRef findVariableWithLength(List* variables, const char* name, int length);

LLVMValueRef llvmCoerceToBool(Compiler* compiler, LLVMValueRef value);

void emitVarStmt(Compiler* compiler, VarStmt* stmt);
void emitForStmt(Compiler* compiler, ForStmt* stmt);
void emitIfStmt(Compiler* compiler, IfStmt* stmt);
// LLVMValueRef emitExpr(Compiler* compiler, Expr* expr);
LLVMValueRef emitCallExpr(Compiler* compiler, CallExpr* expr);
LLVMValueRef emitLiteralExpr(Compiler* compiler, LiteralExpr* expr);
LLVMValueRef emitAssignExpr(Compiler* compiler, AssignExpr* expr);
LLVMValueRef emitUnaryExpr(Compiler* compiler, UnaryExpr* expr);
LLVMValueRef emitVariableExpr(Compiler* compiler, VariableExpr* expr);
LLVMValueRef emitBinaryExpr(Compiler* compiler, BinaryExpr* expr);
LLVMValueRef emitGetExpr(Compiler* compiler, GetExpr* expr);
LLVMValueRef emitSetExpr(Compiler* compiler, SetExpr* expr);
LLVMValueRef emitPrefixExpr(Compiler* compiler, PrefixExpr* expr);
LLVMValueRef emitPostfixExpr(Compiler* compiler, PostfixExpr* expr);

#endif
