#ifndef _DEBUG_H_
#define _DEBUG_H_
#include "parser.h"


#ifdef DEBUG
#define debug(...) printf(__VA_ARGS__)
#else
#define debug(...)
#endif

const char* stmtTypeToString(StmtType type);


// Helper functions
static void printIndent(int indent);
static const char* typeToString(Type* type);

// Expression printing functions
static void printExpr(Expr* expr);
static void printBinaryExpr(BinaryExpr* expr);
static void printUnaryExpr(UnaryExpr* expr);
static void printLiteralExpr(LiteralExpr* expr);
static void printVariableExpr(VariableExpr* expr);
static void printAssignExpr(AssignExpr* expr);
static void printCallExpr(CallExpr* expr);
static void printGroupingExpr(GroupingExpr* expr);
static void printPostfixExpr(PostfixExpr* expr);
static void printPrefixExpr(PrefixExpr* expr);


// Statement printing functions
static void printStmt(Stmt* stmt, int indent);
static void printExprStmt(ExprStmt* stmt, int indent);
static void printVarStmt(VarStmt* stmt, int indent);
static void printIfStmt(IfStmt* stmt, int indent);
void printForStmt(ForStmt* stmt, int indent);
// static void printForInStmt(ForInStmt* stmt, int indent);
static void printFuncStmt(FuncStmt* stmt, int indent);
static void printReturnStmt(ReturnStmt* stmt, int indent);
static void printBlockStmt(BlockStmt* stmt, int indent);

// Debug output functions
#endif