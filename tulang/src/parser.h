#ifndef PARSER_H
#define PARSER_H

#include "lexer.h"
#include "list.h"
typedef struct {
    Lexer* lexer;
    Token current;
    Token previous;
    bool hadError;
    bool panicMode;
} Parser;


typedef enum {
    EXPR_BINARY=0,
    EXPR_UNARY,
    EXPR_LITERAL,
    EXPR_VARIABLE,
    EXPR_GROUPING,
    EXPR_CALL,
    EXPR_POSTFIX,
    EXPR_PREFIX,
    EXPR_ASSIGN,
    EXPR_GET,
    EXPR_SET
} ExprType;

typedef enum {
    STMT_EXPR,
    STMT_VAR,
    STMT_IF,
    STMT_FOR,
    STMT_FOR_IN,
    STMT_FUNC,
    STMT_RETURN,
    STMT_BLOCK,
    STMT_STRUCT,
    STMT_OBJECT,
    STMT_ENUM
} StmtType;



// Type definitions
typedef enum {
    TYPE_INT,
    TYPE_LONG,
    TYPE_DOUBLE,
    TYPE_BOOL,
    TYPE_STRING,
    TYPE_VOID,
    TYPE_ANY,
    TYPE_NAMED,
    TYPE_REF
} TypeKind;

typedef struct Type {
    TypeKind kind;
    Token name; // for TYPE_NAMED
    struct Type* inner; // for TYPE_REF
} Type;

// List structure for parameters and blocks

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
    Expr* operand;
    Token operator;
} PrefixExpr;

typedef struct AssignExpr{
    Expr base;
    Token name;
    Expr* value;
} AssignExpr;

typedef struct {
    Expr base;
    Expr* callee;
    Expr* caller;
    List* arguments;
} CallExpr;

typedef struct {
    Expr base;
    Expr* object;
    Token name;
} GetExpr;

typedef struct {
    Expr base;
    Expr* object;
    Token name;
    Expr* value;
} SetExpr;

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

typedef struct StructStmt {
    Stmt base;
    Token name;
    List* fields;
    List* methods;
} StructStmt;

typedef struct ObjectStmt {
    Stmt base;
    Token name;
    List* methods;
} ObjectStmt;

typedef struct EnumStmt {
    Stmt base;
    Token name;
    List* variants; // List<EnumVariantDecl*>
} EnumStmt;

typedef enum EnumVariantValueKind {
    ENUM_VALUE_NONE = 0,
    ENUM_VALUE_INT,
    ENUM_VALUE_STRING
} EnumVariantValueKind;

typedef struct EnumVariantDecl {
    Token name;
    EnumVariantValueKind valueKind;
    Token value; // TOKEN_INT/TOKEN_LONG/TOKEN_STRING_LITERAL depending on valueKind
} EnumVariantDecl;

typedef struct {
    Stmt base;
    Expr* expression;
} ExprStmt;

typedef struct {
    Token name;
    Type* type;
} Parameter;

typedef struct FieldDeclaration {
    Token name;
    Type* type;
    Expr* initializer;
    bool isConst;
} FieldDeclaration;

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

const char* exprTypeToString(ExprType type);
#endif
