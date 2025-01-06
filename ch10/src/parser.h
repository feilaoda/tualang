#ifndef PARSER_H
#define PARSER_H

#include "lexer.h"
typedef struct {
    Lexer* lexer;
    Token current;
    Token previous;
    bool hadError;
    bool panicMode;
} Parser;


typedef enum {
    EXPR_BINARY,
    EXPR_UNARY,
    EXPR_LITERAL,
    EXPR_VARIABLE,
    EXPR_GROUPING,
    EXPR_CALL,
    EXPR_POSTFIX,
    EXPR_ASSIGN
} ExprType;

typedef enum {
    STMT_EXPR,
    STMT_VAR,
    STMT_IF,
    STMT_FOR,
    STMT_FOR_IN,
    STMT_FUNC,
    STMT_RETURN,
    STMT_BLOCK
} StmtType;



// Type definitions
typedef enum {
    TYPE_INT,
    TYPE_STRING,
    TYPE_VOID
} TypeKind;

typedef struct Type {
    TypeKind kind;
} Type;

// List structure for parameters and blocks
typedef struct ListNode {
    void* data;
    struct ListNode* next;
} ListNode;

typedef struct List {
    ListNode* head;
    ListNode* tail;
    int length;
} List;

// Statement base structure
typedef struct Stmt {
    StmtType type;
} Stmt;

// Expression base structure
typedef struct Expr {
    ExprType type;
    Token token;
} Expr;

typedef struct BinaryExpr {
    Expr base;
    Expr *left;
    Expr *right;
    Token operator;
}BinaryExpr;

// Unary Expression
typedef struct {
    Expr base;
    Token operator;
    Expr* right;
} UnaryExpr;

// Literal Expression
typedef struct {
    Expr base;
    Token value;
} LiteralExpr;

// Variable Expression
typedef struct {
    Expr base;
    Token name;
} VariableExpr;

// Grouping Expression
typedef struct {
    Expr base;
    Expr* expression;
} GroupingExpr;

typedef struct {
    Expr base;
    Expr* operand;
    Token operator;
} PostfixExpr;

typedef struct {
    Expr base;
    Token name;
    Expr* value;
} AssignExpr;

typedef struct {
    Expr base;
    Expr* callee;
    List* arguments;
} CallExpr;

// Variable statement structure
typedef struct {
    Stmt base;
    Token name;
    Type* type;
    Expr* initializer;
    bool isConst;
} VarStmt;

// Function statement structure 
typedef struct {
    Stmt base;
    Token name;
    List* params;
    Type* returnType;
    List* body;
} FuncStmt;

// If statement structure
typedef struct {
    Stmt base;
    Expr* condition;
    Stmt* thenBranch;
    Stmt* elseBranch;
} IfStmt;

// For statement structure
typedef struct {
    Stmt base;
    Stmt* initializer;
    Expr* condition;
    Expr* increment;
    Stmt* body;
} ForStmt;

typedef struct {
    Stmt base;
    Token loopVar;
    Expr* range;
    Stmt* body;
} ForInStmt;
typedef struct {
    Stmt base;
    Expr* value;
    Token keyword;
} ReturnStmt;
typedef struct {
    Stmt base;
    List* statements;
} BlockStmt;

typedef struct {
    Stmt base;
    Expr* expression;
} ExprStmt;

typedef struct {
    Token name;
    Type* type;
} Parameter;

void initParser(Parser* parser, Lexer *lexer);
bool parse(Parser* parser, List** statements);
static Stmt* parseStatement(Parser* parser);
static Expr* parseExpression(Parser* parser);
static Parameter* parseFunctionParameter(Parser* parser);
static Stmt* parseIfStatement(Parser* parser);
static Stmt* parseForStatement(Parser* parser);
static Stmt* parseBlockStatement(Parser* parser);
static Stmt* parseReturnStatement(Parser* parser);
static Stmt* parseExpressionStatement(Parser* parser);

static Stmt* parseVarDeclaration(Parser* parser, bool identifierConsumed);
static Stmt* parseFunctionDeclaration(Parser* parser);

static Type* parseType(Parser* parser);
static Expr* parseUnaryExpr(Parser* parser);
static Expr* parseBinaryExpr(Parser* parser, int minPrec);
static Expr* parsePrimaryExpr(Parser* parser);
static List* parseBlock(Parser* parser);
static int getOperatorPrecedence(TokenType type);

static Expr* newBinaryExpr(Expr* left, Token operator, Expr* right);
static Expr* newUnaryExpr(Token operator, Expr* right);
static Expr* newLiteralExpr(Token value);
static Expr* newVariableExpr(Token value);
static Expr* newGroupingExpr(Expr* expression);
static Expr* newPostfixExpr(Expr* operand, Token operator);
static Expr* finishCall(Parser* parser, Expr* callee);

static Stmt* newExpressionStmt(Expr* expression);
static Stmt* newVarStmt(Token name, Type* type, Expr* initializer, bool isConst);
static Stmt* newFuncStmt(Token name, List* params, Type* returnType, List* body);
static Stmt* newIfStmt(Expr* condition, Stmt* thenBranch, Stmt* elseBranch);
static Stmt* newForStmt(Stmt* initializer, Expr* condition, Expr* increment, Stmt* body);
static Stmt* newBlockStmt(List* statements);
static Stmt* newReturnStmt(Token keyword, Expr* value);

static void errorAt(Parser* parser,Token* token, const char* message);
static void errorPrint(Parser* parser,const char* message);
void errorAtCurrent(Parser* parser, const char* message);
static Token consume(Parser* parser, TokenType type, const char* message);
// List operations
List* listNew(void);
List* listAppend(List* list, void* data);
void listFree(List* list);

#endif