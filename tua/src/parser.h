#ifndef PARSER_H
#define PARSER_H

#include "lexer.h"
#include "list.h"
#include <stdint.h>
typedef struct {
    Lexer* lexer;
    const char* currentFilePath;
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
    EXPR_CAST,
    EXPR_POSTFIX,
    EXPR_PREFIX,
    EXPR_ASSIGN,
    EXPR_GET,
    EXPR_SET,
    EXPR_LAMBDA,
    EXPR_MAP_LITERAL,
    EXPR_ARRAY_LITERAL,
    EXPR_BRACE_LITERAL,
    EXPR_INDEX,
    EXPR_INDEX_SET,
    EXPR_STRUCT_INIT
} ExprType;

typedef enum {
    STMT_EXPR,
    STMT_VAR,
    STMT_IF,
    STMT_FOR,
    STMT_FOR_IN,
    STMT_WHILE,
    STMT_DO_WHILE,
    STMT_BREAK,
    STMT_CONTINUE,
    STMT_LABEL,
    STMT_GOTO,
    STMT_IMPORT,
    STMT_FROM_IMPORT,
    STMT_PRIVATE,
    STMT_FUNC,
    STMT_RETURN,
    STMT_BLOCK,
    STMT_STRUCT,
    STMT_OBJECT,
    STMT_ENUM,
    STMT_DESTRUCTURE,
    STMT_IMPL
} StmtType;



// Type definitions
typedef enum {
    TYPE_ANY,
    TYPE_VOID,
    TYPE_BOOL,
    TYPE_STRING,
    // Raw pointer for runtime/FFI: lowered as i8* in LLVM.
    TYPE_PTR,

    // Signed integers
    TYPE_I8,
    TYPE_I16,
    TYPE_INT,
    TYPE_LONG,
    TYPE_ISIZE,

    // Unsigned integers
    TYPE_U8,
    TYPE_U16,
    TYPE_U32,
    TYPE_U64,
    TYPE_USIZE,
    TYPE_BYTE, // alias of u8 (semantic: unsigned)

    // Floats
    TYPE_F8,   // reserved (storage-only for now)
    TYPE_F16,
    TYPE_FLOAT,  // f32
    TYPE_DOUBLE, // f64
    TYPE_BF8,  // reserved (storage-only for now)
    TYPE_BF16,

    TYPE_NAMED,
    TYPE_REF,
    TYPE_ARRAY,
    // Function type syntax: `(args...) -> ret` or `(args...) -> (ret, ret2, ...)`
    TYPE_FUNC
} TypeKind;

typedef struct Type {
    TypeKind kind;
    Token name; // for TYPE_NAMED
    struct Type* inner; // for TYPE_REF
    List* typeArgs;     // List<Type*>, for TYPE_NAMED generic args (e.g. map<K,V>, Option<T>)
    List* paramTypes;   // List<Type*>, for TYPE_FUNC
    List* returnTypes;  // List<Type*>, for TYPE_FUNC
    int64_t arrayLen;   // for TYPE_ARRAY: -1 => dynamic (T[]), >=0 => fixed (T[N])
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
    // Best-effort inferred type kind (populated by analyzer). Defaults to TYPE_ANY.
    TypeKind inferredType;
} Expr;

typedef struct BinaryExpr {
    Expr base;
    Expr *left;
    Expr *right;
    Token operator;
}BinaryExpr;

// Cast expression:
// - Unchecked: `(T)expr`
// - Checked: `expr as T` (returns Option<T>)
typedef struct CastExpr {
    Expr base;
    Expr* value;
    Type* targetType;
    int isChecked; // 0 => unchecked cast `(T)expr`, 1 => checked cast `expr as T`
} CastExpr;

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

typedef struct MapEntry {
    Token key;     // TOKEN_INT/TOKEN_LONG/TOKEN_STRING_LITERAL only
    Expr* value;   // any expression
} MapEntry;

typedef struct {
    Expr base;
    List* entries; // List<MapEntry*>
} MapLiteralExpr;

typedef struct {
    Expr base;
    List* elements; // List<Expr*>
} ArrayLiteralExpr;

// Ambiguous empty brace literal: `{}`.
// At parse time it may be a map or an array; resolver/codegen decides using context.
typedef struct {
    Expr base;
    Token lbrace;
} BraceLiteralExpr;

typedef struct {
    Expr base;
    Expr* object;
    Expr* index;
} IndexExpr;

typedef struct {
    Expr base;
    Expr* object;
    Expr* index;
    Expr* value;
} IndexSetExpr;

typedef struct StructFieldInit {
    Token name;
    Expr* value;
} StructFieldInit;

typedef struct {
    Expr base;
    Expr* callee;  // struct name expr: `Foo` or `ns.Foo`
    List* fields;  // List<StructFieldInit*>
} StructInitExpr;

// Lambda expression (closure literal)
// Syntax: `fn (params...) -> T[,U...] { ... }`
typedef struct {
    Expr base;
    Token keyword;
    List* params;      // List<Parameter*>
    Type* returnType;  // single return (legacy)
    List* returnTypes; // List<Type*> for multi-return
    List* body;        // List<Stmt*>
} LambdaExpr;

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
    List* returnTypes; // List<Type*>, NULL or empty => void
    List* body;
    Token externAlias; // only for `extern fn ... as <alias>`; otherwise {0}
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
    Token valueVar; // optional second binding for map iteration: for k,v in m {}
    int hasValueVar;
    Expr* range;
    Stmt* body;
} ForInStmt;
typedef struct {
    Stmt base;
    Expr* value;
    List* values; // List<Expr*>, if NULL treat as single value
    Token keyword;
} ReturnStmt;

// Destructuring statement:
// - Declaration: `let a,b = f()` / `const a,b = f()`
// - Assignment:  `a,b = f()`
typedef struct {
    Stmt base;
    Token keyword;  // TOKEN_VAR / TOKEN_CONST for declaration, TOKEN_ERROR for assignment
    List* names;    // List<Token*>
    List* types;    // List<Type*> (may contain NULL entries)
    Expr* value;    // RHS expression
    bool isConst;
    bool isDeclaration;
} DestructureStmt;
typedef struct {
    Stmt base;
    List* statements;
} BlockStmt;

typedef struct {
    Stmt base;
    Expr* condition;
    Stmt* body;
} WhileStmt;

typedef struct {
    Stmt base;
    Expr* condition;
    Stmt* body;
} DoWhileStmt;

typedef struct {
    Stmt base;
    Token keyword;
} BreakStmt;

typedef struct {
    Stmt base;
    Token keyword;
} ContinueStmt;

typedef struct {
    Stmt base;
    Token name;
} LabelStmt;

typedef struct {
    Stmt base;
    Token name;
    Token keyword;
} GotoStmt;

typedef struct {
    Token name;
    Token alias;
    bool hasAlias;
} ImportName;

typedef struct {
    Stmt base;
    Token path; // string literal token
    Token keyword;
    Token alias; // identifier token, for `import "path" as ns`
    bool hasAlias;
} ImportStmt;

typedef struct {
    Stmt base;
    Token path; // string literal token
    List* names; // List<ImportName*>
    Token keywordFrom;
    Token keywordImport;
} FromImportStmt;

typedef struct {
    Stmt base;
    Stmt* inner;
    Token keyword;
} PrivateStmt;

typedef struct StructStmt {
    Stmt base;
    Token name;
    List* fields;
    List* methods;
} StructStmt;

typedef struct ImplStmt {
    Stmt base;
    Token name;     // target struct name
    List* methods;  // List<FuncStmt*>
} ImplStmt;

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
    // Parameter mode:
    // - default/const => shared readonly borrow
    // - let           => exclusive mutable borrow
    // - move          => take ownership
    int mode;
} Parameter;

typedef enum {
    PARAM_CONST = 0,
    PARAM_LET = 1,
    PARAM_MOVE = 2,
} ParamMode;

typedef struct FieldDeclaration {
    Token name;
    Type* type;
    Expr* initializer;
    bool isConst;
} FieldDeclaration;

void initParser(Parser* parser, Lexer *lexer, const char* currentFilePath);
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
static Expr* newMapLiteralExpr(Token lbrace, List* entries);
static Expr* newIndexExpr(Expr* object, Expr* index);
static Expr* newIndexSetExpr(Expr* object, Expr* index, Expr* value);


static Stmt* newExpressionStmt(Expr* expression);
static Stmt* newVarStmt(Token name, Type* type, Expr* initializer, bool isConst);
static Stmt* newFuncStmt(Token name, List* params, Type* returnType, List* returnTypes, List* body);
static Stmt* newIfStmt(Expr* condition, Stmt* thenBranch, Stmt* elseBranch);
static Stmt* newForStmt(Stmt* initializer, Expr* condition, Expr* increment, Stmt* body);
static Stmt* newBlockStmt(List* statements);
static Stmt* newReturnStmt(Token keyword, Expr* value, List* values);

static void errorAt(Parser* parser,Token* token, const char* message);
static void errorPrint(Parser* parser,const char* message);
void errorAtCurrent(Parser* parser, const char* message);
static Token consume(Parser* parser, TokenType type, const char* message);

const char* exprTypeToString(ExprType type);
#endif
