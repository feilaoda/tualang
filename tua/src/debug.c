#include "debug.h"
#include <stdlib.h>
#include <stdio.h> 

static const char* StmtTypeNames[] = {
    "EXPR",
    "VAR",
    "IF",
    "IF_LET",
    "FOR",
    "FOR_IN",
    "WHILE",
    "DO_WHILE",
    "BREAK",
    "CONTINUE",
    "LABEL",
    "GOTO",
    "IMPORT",
    "FROM_IMPORT",
    "PRIVATE",
    "FUNC",
    "RETURN",
    "BLOCK",
    "STRUCT",
    "OBJECT",
    "ENUM",
    "DESTRUCTURE",
    "IMPL",
    "TRAIT",
    "TRAIT_IMPL"
};

const char* stmtTypeToString(StmtType type) {
    if (type < 0 || type >= sizeof(StmtTypeNames) / sizeof(StmtTypeNames[0])) {
        return "UNKNOWN";
    }
    return StmtTypeNames[type];
}


static const char* typeToString(Type* type) {
    if (!type) return "any";
    switch (type->kind) {
        case TYPE_INT:    return "int";
        case TYPE_LONG:   return "long";
        case TYPE_DOUBLE: return "double";
        case TYPE_FLOAT:  return "float";
        case TYPE_STRING: return "string";
        case TYPE_BOOL:   return "bool";
        case TYPE_NAMED:  return "named";
        case TYPE_FUNC:   return "fn";
        default:          return "unknown";
    }
}

static void printIndent(int indent) {
    for (int i = 0; i < indent; i++) printf(" ");
}

static void printGuardExpr(GuardExpr* expr);

void printExpr(Expr* expr) {
    if (!expr) return;
    printf("exprType-%d{", expr->type);
    switch (expr->type) {
        case EXPR_BINARY:
            printBinaryExpr((BinaryExpr*)expr);
            break;
        case EXPR_UNARY:
            printUnaryExpr((UnaryExpr*)expr);
            break;
        case EXPR_LITERAL:
            printLiteralExpr((LiteralExpr*)expr);
            break;
        case EXPR_VARIABLE:
            printVariableExpr((VariableExpr*)expr);
            break;
        case EXPR_ASSIGN:
            printAssignExpr((AssignExpr*)expr);
            break;
        case EXPR_CALL:
            printCallExpr((CallExpr*)expr);
            break;
        case EXPR_GUARD:
            printGuardExpr((GuardExpr*)expr);
            break;
        case EXPR_POSTFIX:
            printPostfixExpr((PostfixExpr*)expr);
            break;
        case EXPR_PREFIX:
            printPrefixExpr((PrefixExpr*)expr);
            break;
        default:
            printf("Unknown expression type, %d\n", expr->type);
            break;
    }
    printf("}");
}

static void printPostfixExpr(PostfixExpr* expr) {
    printExpr(expr->operand);
    printf("%.*s", expr->operator.length, expr->operator.start);
}

static void printPrefixExpr(PrefixExpr* expr) {
    printExpr(expr->operand);
    printf("%.*s", expr->operator.length, expr->operator.start);
}

static void printBinaryExpr(BinaryExpr* expr) {
    printf("(");
    printExpr(expr->left);
    printf(" %.*s ", expr->operator.length, expr->operator.start);
    printExpr(expr->right);
    printf(")");
}

static void printUnaryExpr(UnaryExpr* expr) {
    printf("(%.*s", expr->operator.length, expr->operator.start);
    printExpr(expr->right);
    printf(")");
}

static void printLiteralExpr(LiteralExpr* expr) {
    printf("LITERAL->%d %.*s", expr->value.type, expr->value.length, expr->value.start);

}

static void printVariableExpr(VariableExpr* expr) {
    printf("%.*s", expr->name.length, expr->name.start);
}

static void printAssignExpr(AssignExpr* expr) {
    printf("%.*s = ", expr->name.length, expr->name.start);
    printExpr(expr->value);
}

static void printCallExpr(CallExpr* expr) {
    printExpr(expr->callee);
    printf("(");
    ListNode* arg = expr->arguments->head;
    while (arg != NULL) {
        printExpr((Expr*)arg->data);
        if (arg->next != NULL) printf(", ");
        arg = arg->next;
    }
    printf(")");
}

static void printGuardExpr(GuardExpr* expr) {
    printExpr(expr->call);
    printf(" ? { ... }");
}

void printStmt(Stmt* stmt, int indent) {
    if (!stmt) return;
    
    switch (stmt->type) {
        case STMT_EXPR:
            printExprStmt((ExprStmt*)stmt, indent);
            break;
        case STMT_VAR:
            printVarStmt((VarStmt*)stmt, indent);
            break;
        case STMT_IF:
            printIfStmt((IfStmt*)stmt, indent);
            break;
        case STMT_IF_LET: {
            IfLetStmt* s = (IfLetStmt*)stmt;
            printIndent(indent);
            printf("if let Some(%.*s) = ", s->name.length, s->name.start);
            printExpr(s->value);
            printf(" {\n");
            printStmt(s->thenBranch, indent + 2);
            if (s->elseBranch) {
                printIndent(indent);
                printf("} else {\n");
                printStmt(s->elseBranch, indent + 2);
            }
            printIndent(indent);
            printf("}\n");
            break;
        }
        case STMT_FOR:
            printForStmt((ForStmt*)stmt, indent);
            break;
        case STMT_FUNC:
            printFuncStmt((FuncStmt*)stmt, indent);
            break;
        case STMT_RETURN:
            printReturnStmt((ReturnStmt*)stmt, indent);
            break;
        case STMT_BLOCK:
            printBlockStmt((BlockStmt*)stmt, indent);
            break;
        default:    
            printf("Unknown statement type, %d\n", stmt->type);
            break;
    }
}

void printExprStmt(ExprStmt* stmt, int indent) {
    printIndent(indent);
    printExpr(stmt->expression);
    printf(";\n");
}

static void printVarStmt(VarStmt* stmt, int indent) {
    printIndent(indent);
    printf("%s %.*s", stmt->isConst ? "const" : "let", 
           stmt->name.length, stmt->name.start);
    if (stmt->type) {
        printf(":%s", typeToString(stmt->type));
    }
    if (stmt->initializer) {
        printf(" = ");
        printExpr(stmt->initializer);
    }
    printf(";\n");
}

static void printIfStmt(IfStmt* stmt, int indent) {
    printIndent(indent);
    printf("if (");
    printExpr(stmt->condition);
    printf(") {\n");
    printStmt(stmt->thenBranch, indent + 2);
    if (stmt->elseBranch) {
        printIndent(indent);
        printf("} else {\n");
        printStmt(stmt->elseBranch, indent + 2);
    }
    printIndent(indent);
    printf("}\n");
}

static void printBlockStmt(BlockStmt* stmt, int indent) {
    printIndent(indent);
    printf("{\n");
    ListNode* node = stmt->statements->head;
    while (node != NULL) {
        printStmt((Stmt*)node->data, indent + 2);
        node = node->next;
    }
    printIndent(indent);
    printf("}\n");
}

static void printFuncStmt(FuncStmt* stmt, int indent) {
    printIndent(indent);
    printf("func %.*s(", stmt->name.length, stmt->name.start);
    ListNode* param = stmt->params->head;
    while (param != NULL) {
        Parameter* p = (Parameter*)param->data;
        printf("%.*s", p->name.length, p->name.start);
        if (p->type) printf(":%s", typeToString(p->type));
        if (param->next != NULL) printf(", ");
        param = param->next;
    }
    printf(")");
    if (stmt->returnType) {
        printf(" -> %s", typeToString(stmt->returnType));
    }
    printf(" {\n");
    printBlockStmt((BlockStmt*)stmt->body, indent + 2);
    printIndent(indent);
    printf("}\n");
}

static void printReturnStmt(ReturnStmt* stmt, int indent) {
    printIndent(indent);
    printf("return");
    if (stmt->value) {
        printf(" ");
        printExpr(stmt->value);
    }
    printf(";\n");
}

void printForStmt(ForStmt* stmt, int indent) {
    printIndent(indent);
    printf("for ");
    
    // Print initializer if exists
    if (stmt->initializer) {
        if (stmt->initializer->type == STMT_VAR) {
            VarStmt* var = (VarStmt*)stmt->initializer;
            if (var->type) {
                printf("%.*s:%s=", var->name.length, var->name.start, 
                       typeToString(var->type));
            } else {
                printf("%.*s=", var->name.length, var->name.start);
            }
            printExpr(var->initializer);
        } else {
            printStmt(stmt->initializer, 0);
        }
    }
    
    printf("; ");
    
    // Print condition
    if (stmt->condition) {
        printExpr(stmt->condition);
    }
    printf("; ");
    
    // Print increment
    if (stmt->increment) {
        printExpr(stmt->increment);
    }else {
        printf("inc null;");
    }
    
    printf(" {\n");
    
    // Print body with increased indent
    if (stmt->body) {
        printStmt(stmt->body, indent + 2);
    }
    
    printIndent(indent);
    printf("}\n");
}
