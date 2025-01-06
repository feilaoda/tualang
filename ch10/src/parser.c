#include "parser.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include "debug.h"

// Debug trace helpers
int traceId = 0;
#define parserDebugStart(x) traceId++;for(int a=0;a<traceId;a++){ printf("===="); } fprintf(stdout," <%s(%d) [%.*s],",x,__LINE__,parser->current.length, parser->current.start); debug("token:%d,%s\n",parser->current.type, tokenToString(parser->current.type))
#define parserDebugEnd(x) for(int a=0;a<traceId;a++) {printf("====");} traceId--; fprintf(stdout," />%s(%d) [%.*s],",x,__LINE__,parser->current.length, parser->current.start); debug("token:%d,%s\n",parser->current.type, tokenToString(parser->current.type))
#define parserDebug(...) for(int a=0;a<traceId;a++) {printf("====");} printf(" "); debug(__VA_ARGS__)
#define error(p,msg) fprintf(stderr,"ERROR: %d", __LINE__); errorPrint(p,msg)

// Parser initialization
void initParser(Parser* parser, Lexer* lexer) {
    parser->lexer = lexer;
    parser->hadError = false;
    parser->panicMode = false;
    parser->previous.type = TOKEN_ERROR;
    parser->current.type = TOKEN_ERROR;
}

// Error handling
void errorAtCurrent(Parser* parser, const char* message) {
    if (parser->panicMode) return;
    parser->panicMode = true;
    fprintf(stderr, "[line %d] Error at '%.*s': %s\n",
            parser->current.line,
            parser->current.length,
            parser->current.start,
            message);
    parser->hadError = true;
}



static void errorPrint(Parser* parser, const char* message) {
    if (parser->panicMode) return;
    parser->panicMode = true;
    fprintf(stderr, "***********[line %d]******** Error: %s\n", 
            parser->current.line, message);
    parser->hadError = true;
}

// Basic parser functions
static void advance(Parser* parser) {
    parser->previous = parser->current;
    parser->current = scanToken(parser->lexer);
    parserDebug("advance scantoken: [%s]\n", tokenToString(parser->current.type));
    if (parser->current.type == TOKEN_ERROR) {
        errorAtCurrent(parser, parser->current.start);
    }
}

static bool check(Parser* parser, TokenType type) {
    return parser->current.type == type;
}

static bool match(Parser* parser, TokenType type) {
    if (!check(parser, type)) return false;
    advance(parser);
    return true;
}

static Token consume(Parser* parser, TokenType type, const char* message) {
    if (check(parser, type)) {
        Token token = parser->current;
        advance(parser);
        return token;
    }
    error(parser, message);
    return (Token){TOKEN_ERROR, NULL, 0, 0};
}

static void synchronize(Parser* parser) {
    parser->panicMode = false;
    while (parser->current.type != TOKEN_EOF) {
        if (parser->previous.type == TOKEN_SEMICOLON) return;
        
        switch (parser->current.type) {
            case TOKEN_FUNC:
            case TOKEN_LET:
            case TOKEN_CONST:
            case TOKEN_IF:
            case TOKEN_FOR:
            case TOKEN_RETURN:
                return;
            default:
                advance(parser);
        }
    }
}

// Operator precedence
static int getOperatorPrecedence(TokenType type) {
    switch (type) {
        case TOKEN_ASSIGN: return 1;      // =
        case TOKEN_OR: return 2;          // ||
        case TOKEN_AND: return 3;         // &&
        case TOKEN_EQ: 
        case TOKEN_NEQ: return 4;         // ==, !=
        case TOKEN_LT: 
        case TOKEN_GT: return 5;          // <, >
        case TOKEN_PLUS: 
        case TOKEN_MINUS: return 6;       // +, -
        case TOKEN_STAR: 
        case TOKEN_SLASH: return 7;       // *, /
        case TOKEN_INC:
        case TOKEN_DEC: return 8;         // ++, --
        default: return 0;
    }
}

// AST Node Constructors
static Stmt* newVarStmt(Token name, Type* type, Expr* initializer, bool isConst) {
    VarStmt* stmt = malloc(sizeof(VarStmt));
    stmt->base.type = STMT_VAR;
    stmt->name = name;
    stmt->type = type;
    stmt->initializer = initializer;
    stmt->isConst = isConst;
    return (Stmt*)stmt;
}

static Stmt* newFuncStmt(Token name, List* params, Type* returnType, List* body) {
    FuncStmt* stmt = malloc(sizeof(FuncStmt));
    stmt->base.type = STMT_FUNC;
    stmt->name = name;
    stmt->params = params;
    stmt->returnType = returnType;
    stmt->body = body;
    return (Stmt*)stmt;
}

static Stmt* newIfStmt(Expr* condition, Stmt* thenBranch, Stmt* elseBranch) {
    IfStmt* stmt = malloc(sizeof(IfStmt));
    stmt->base.type = STMT_IF;
    stmt->condition = condition;
    stmt->thenBranch = thenBranch;
    stmt->elseBranch = elseBranch;
    return (Stmt*)stmt;
}

static Expr* newPostfixExpr(Expr* operand, Token operator) {
    PostfixExpr* expr = malloc(sizeof(PostfixExpr));
    expr->base.type = EXPR_POSTFIX;
    expr->operand = operand;
    expr->operator = operator;
    return (Expr*)expr;
}

static Expr* newBinaryExpr(Expr* left, Token operator, Expr* right) {
    BinaryExpr* expr = malloc(sizeof(BinaryExpr));
    expr->base.type = EXPR_BINARY;
    expr->left = left;
    expr->operator = operator;
    expr->right = right;
    return (Expr*)expr;
}

static Expr* newUnaryExpr(Token operator, Expr* right) {
    UnaryExpr* expr = malloc(sizeof(UnaryExpr));
    expr->base.type = EXPR_UNARY;
    expr->operator = operator;
    expr->right = right;
    return (Expr*)expr;
}

static Expr* newLiteralExpr(Token value) {
    LiteralExpr* expr = malloc(sizeof(LiteralExpr));
    expr->base.type = EXPR_LITERAL;
    expr->value = value;
    return (Expr*)expr;
}

static Expr* newVariableExpr(Token name) {
    VariableExpr* expr = malloc(sizeof(VariableExpr));
    expr->base.type = EXPR_VARIABLE;
    expr->name = name;
    return (Expr*)expr;
}

static Expr* newGroupingExpr(Expr* expression) {
    GroupingExpr* expr = malloc(sizeof(GroupingExpr));
    expr->base.type = EXPR_GROUPING;
    expr->expression = expression;
    return (Expr*)expr;
}

static Expr* newAssignExpr(Token name, Expr* value) {
    AssignExpr* expr = malloc(sizeof(AssignExpr));
    expr->base.type = EXPR_ASSIGN;
    expr->name = name;
    expr->value = value;
    return (Expr*)expr;
}

static Parameter* newParameter(Token name, Type* type) {
    Parameter* param = malloc(sizeof(Parameter));
    param->name = name;
    param->type = type;
    return param;
}



// Expression parsing
static Expr* parseExpression(Parser* parser) {
    parserDebugStart("parseExpression");
    
    Expr* expr = parseBinaryExpr(parser, 0);
    
    if (match(parser, TOKEN_ASSIGN)) {
        Token equals = parser->previous;
        Expr* value = parseExpression(parser);
        
        if (expr->type == EXPR_VARIABLE) {
            return newAssignExpr(((VariableExpr*)expr)->name, value);
        }
        
        error(parser, "Invalid assignment target.");
    }
    
    parserDebugEnd("parseExpression");
    return expr;
}

static Expr* parseBinaryExpr(Parser* parser, int minPrec) {
    parserDebugStart("parseBinaryExpr start");
    Expr* left = parseUnaryExpr(parser);
    
    while (true) {
        TokenType op = parser->current.type;
        int prec = getOperatorPrecedence(op);
        
        if (prec == 0 || prec < minPrec) break;
        
        if (match(parser, TOKEN_LPAREN)) {
            left = finishCall(parser, left);
            continue;
        }
        
        if ((op == TOKEN_INC || op == TOKEN_DEC) && prec >= minPrec) {
            advance(parser);
            left = newPostfixExpr(left, parser->previous);
            continue;
        }
        
        Token operator = parser->current;
        advance(parser);
        Expr* right = parseBinaryExpr(parser, prec + 1);
        left = newBinaryExpr(left, operator, right);
    }
    parserDebugEnd("parseBinaryExpr");
    return left;
}

static Expr* parseUnaryExpr(Parser* parser) {
    parserDebugStart("parseUnaryExpr");
    
    if (match(parser, TOKEN_INC) || 
        match(parser, TOKEN_DEC) ||
        match(parser, TOKEN_MINUS) || 
        match(parser, TOKEN_NOT)) {
        Token operator = parser->previous;
        Expr* right = parseUnaryExpr(parser);
        parserDebugEnd("parseUnaryExpr");
        return newUnaryExpr(operator, right);
    }
    
    Expr* expr = parsePrimaryExpr(parser);
    parserDebugEnd("parseUnaryExpr");
    return expr;
}

static Expr* parsePrimaryExpr(Parser* parser) {
    parserDebugStart("parsePrimaryExpr");
    
    if (match(parser, TOKEN_NUMBER) || 
        match(parser, TOKEN_STRING_LITERAL)) {
        Expr* expr = newLiteralExpr(parser->previous);
        parserDebugEnd("parsePrimaryExpr");
        return expr;
    }
    
    if (match(parser, TOKEN_IDENTIFIER)) {
        Expr* expr = newVariableExpr(parser->previous);
        
        if (match(parser, TOKEN_LPAREN)) {
            expr = finishCall(parser, expr);
        }
        
        parserDebugEnd("parsePrimaryExpr");
        return expr;
    }
    
    if (match(parser, TOKEN_LPAREN)) {
        Expr* expr = parseExpression(parser);
        consume(parser, TOKEN_RPAREN, "Expect ')' after expression");
        parserDebugEnd("parsePrimaryExpr");
        return newGroupingExpr(expr);
    }
    
    error(parser, "Expected expression");
    parserDebugEnd("parsePrimaryExpr");
    return NULL;
}

static Expr* finishCall(Parser* parser, Expr* callee) {
    parserDebugStart("finishCall");
    List* arguments = listNew();
    
    if (!check(parser, TOKEN_RPAREN)) {
        do {
            listAppend(arguments, parseExpression(parser));
        } while (match(parser, TOKEN_COMMA));
    }
    
    consume(parser, TOKEN_RPAREN, "Expect ')' after arguments");
    
    CallExpr* expr = malloc(sizeof(CallExpr));
    expr->base.type = EXPR_CALL;
    expr->callee = callee;
    expr->arguments = arguments;
    parserDebugEnd("finishCall");
    return (Expr*)expr;
}

static Stmt* parseStatement(Parser* parser) {
    parserDebugStart("parseStatement");
    Stmt* stmt = NULL;
    
    if (match(parser, TOKEN_IF)) {
        stmt = parseIfStatement(parser);
    } else if (match(parser, TOKEN_FOR)) {
        stmt = parseForStatement(parser);
    } else if (match(parser, TOKEN_RETURN)) {
        stmt = parseReturnStatement(parser);
    } else if (match(parser, TOKEN_LBRACE)) {
        stmt = parseBlockStatement(parser);
    } else {
        stmt = parseExpressionStatement(parser);
    }
    
    parserDebugEnd("parseStatement");
    return stmt;
}

static Stmt* parseIfStatement(Parser* parser) {
    consume(parser, TOKEN_LPAREN, "Expect '(' after 'if'");
    Expr* condition = parseExpression(parser);
    consume(parser, TOKEN_RPAREN, "Expect ')' after condition");
    
    Stmt* thenBranch = parseStatement(parser);
    Stmt* elseBranch = NULL;
    
    if (match(parser, TOKEN_ELSE)) {
        elseBranch = parseStatement(parser);
    }
    
    return newIfStmt(condition, thenBranch, elseBranch);
}

static Stmt* parseForStatement(Parser* parser) {
    parserDebugStart("parseForStatement");
    bool hasParen = false;
    
    if (check(parser, TOKEN_LPAREN)) {
        consume(parser, TOKEN_LPAREN, "Expect '(' after 'for'");
        hasParen = true;
    }
    
    Stmt* initializer = NULL;
    Expr* rangeExpr = NULL;
    
    if (match(parser, TOKEN_SEMICOLON)) {
        initializer = NULL;
    } else if (match(parser, TOKEN_LET)) {
        initializer = parseVarDeclaration(parser, false);
    } else {
        if (check(parser, TOKEN_IDENTIFIER)) {
            Token current = parser->current;
            advance(parser);
            parserDebug("parseForStatement0: in [%.*s],[%s]\n", parser->current.length,parser->current.start, tokenToString(parser->current.type));
            
            if (check(parser, TOKEN_COLON)) {
                initializer = parseVarDeclaration(parser, true);
            } else if (check(parser, TOKEN_IN)) {
                parserDebug("parseForStatement1: in [%s]\n", tokenToString(parser->current.type));
                advance(parser); // consume 'in'
                parserDebug("parseForStatement2: in [%s]\n", tokenToString(parser->current.type));
                rangeExpr = parseExpression(parser);
                
                if (hasParen) {
                    consume(parser, TOKEN_RPAREN, "Expect ')' after range");
                }
                
                Stmt* body = parseStatement(parser);
                ForInStmt* stmt = malloc(sizeof(ForInStmt));
                stmt->base.type = STMT_FOR_IN;
                stmt->loopVar = current;
                stmt->range = rangeExpr;
                stmt->body = body;
                
                parserDebugEnd("parseForStatement");
                return (Stmt*)stmt;
            } else {
                parser->current = current;
                initializer = parseExpressionStatement(parser);
            }
        } else {
            initializer = parseExpressionStatement(parser);
        }
    }
    
    // Standard for loop
    Expr* condition = NULL;
    if (!check(parser, TOKEN_SEMICOLON)) {
        condition = parseExpression(parser);
    }
    consume(parser, TOKEN_SEMICOLON, "Expect ';' after loop condition");
    
    Expr* increment = NULL;
    if (!check(parser, TOKEN_RPAREN) && !check(parser, TOKEN_LBRACE)) {
        increment = parseExpression(parser);
    }
    
    if (hasParen) {
        consume(parser, TOKEN_RPAREN, "Expect ')' after for clauses");
    }
    
    Stmt* body = parseStatement(parser);
    
    ForStmt* stmt = malloc(sizeof(ForStmt));
    stmt->base.type = STMT_FOR;
    stmt->initializer = initializer;
    stmt->condition = condition;
    stmt->increment = increment;
    stmt->body = body;
    
    parserDebugEnd("parseForStatement");
    return (Stmt*)stmt;
}

static Stmt* parseBlockStatement(Parser* parser) {
    List* statements = listNew();
    
    while (!check(parser, TOKEN_RBRACE) && !check(parser, TOKEN_EOF)) {
        listAppend(statements, parseStatement(parser));
    }
    
    consume(parser, TOKEN_RBRACE, "Expect '}' after block");
    
    BlockStmt* stmt = malloc(sizeof(BlockStmt));
    stmt->base.type = STMT_BLOCK;
    stmt->statements = statements;
    return (Stmt*)stmt;
}

static Stmt* parseReturnStatement(Parser* parser) {
    Token keyword = parser->previous;
    Expr* value = NULL;
    
    if (!check(parser, TOKEN_SEMICOLON)) {
        value = parseExpression(parser);
    }
    
    consume(parser, TOKEN_SEMICOLON, "Expect ';' after return value");
    
    ReturnStmt* stmt = malloc(sizeof(ReturnStmt));
    stmt->base.type = STMT_RETURN;
    stmt->keyword = keyword;
    stmt->value = value;
    return (Stmt*)stmt;
}

static Stmt* parseExpressionStatement(Parser* parser) {
    Expr* expr = parseExpression(parser);
    consume(parser, TOKEN_SEMICOLON, "Expect ';' after expression");
    
    ExprStmt* stmt = malloc(sizeof(ExprStmt));
    stmt->base.type = STMT_EXPR;
    stmt->expression = expr;
    return (Stmt*)stmt;
}
static Stmt* parseVarDeclaration(Parser* parser, bool identifierConsumed) {
    parserDebugStart("parseVarDeclaration");
    bool isConst = parser->previous.type == TOKEN_CONST;
    bool hasLet = parser->previous.type == TOKEN_LET;
    
    Token name;
    if (identifierConsumed) {
        name = parser->previous;
    } else {
        name = consume(parser, TOKEN_IDENTIFIER, "Expect variable name");
    }
    
    Type* type = NULL;
    if (match(parser, TOKEN_COLON)) {
        type = parseType(parser);
    }
    
    Expr* initializer = NULL;
    if (match(parser, TOKEN_ASSIGN)) {
        initializer = parseExpression(parser);
    }
    
    consume(parser, TOKEN_SEMICOLON, "Expect ';' after variable declaration");
    
    parserDebugEnd("parseVarDeclaration");
    return newVarStmt(name, type, initializer, isConst);
}
// Block parsing
static List* parseBlock(Parser* parser) {
    List* statements = listNew();
    
    consume(parser, TOKEN_LBRACE, "Expect '{' before block");
    
    while (!check(parser, TOKEN_RBRACE) && !check(parser, TOKEN_EOF)) {
        Stmt* stmt = parseStatement(parser);
        if (stmt != NULL) {
            listAppend(statements, stmt);
        }
    }
    
    consume(parser, TOKEN_RBRACE, "Expect '}' after block");
    return statements;
}

static Stmt* parseFunctionDeclaration(Parser* parser) {
    Token name = consume(parser, TOKEN_IDENTIFIER, "Expect function name");
    consume(parser, TOKEN_LPAREN, "Expect '(' after function name");
    
    List* parameters = listNew();
    if (!check(parser, TOKEN_RPAREN)) {
        do {
            Token param = consume(parser, TOKEN_IDENTIFIER, "Expect parameter name");
            Type* type = NULL;
            if (match(parser, TOKEN_COLON)) {
                type = parseType(parser);
            }
            listAppend(parameters, newParameter(param, type));
        } while (match(parser, TOKEN_COMMA));
    }
    consume(parser, TOKEN_RPAREN, "Expect ')' after parameters");
    
    Type* returnType = NULL;
    if (match(parser, TOKEN_ARROW)) {
        returnType = parseType(parser);
    }
    
    consume(parser, TOKEN_LBRACE, "Expect '{' before function body");
    List* body = parseBlock(parser);
    
    return newFuncStmt(name, parameters, returnType, body);
}
static Type* parseType(Parser* parser) {
    if (match(parser, TOKEN_INT)) {
        Type* type = malloc(sizeof(Type));
        type->kind = TYPE_INT;
        return type;
    }
    if (match(parser, TOKEN_STRING)) {
        Type* type = malloc(sizeof(Type));
        type->kind = TYPE_STRING;
        return type;
    }
    error(parser, "Expect type name");
    return NULL;
}

static Stmt* declaration(Parser* parser) {
    if (match(parser, TOKEN_FUNC)) {
        return parseFunctionDeclaration(parser);
    }
    
    if (match(parser, TOKEN_LET) || match(parser, TOKEN_CONST)) {
        return parseVarDeclaration(parser, false);
    }
    
    if (check(parser, TOKEN_IDENTIFIER)) {
        Token current = parser->current;
        advance(parser);
        
        if (check(parser, TOKEN_COLON)) {
            return parseVarDeclaration(parser, true);
        }
        
        parser->current = current;
    }
    
    return parseStatement(parser);
}

bool parse(Parser* parser, List** statements) {
    parser->hadError = false;
    parser->panicMode = false;
    *statements = listNew();
    
    advance(parser);
    while (!match(parser, TOKEN_EOF)) {
        Stmt* stmt = declaration(parser);
        if (stmt != NULL) {
            listAppend(*statements, stmt);
        }
        
        if (parser->panicMode) {
            synchronize(parser);
        }
    }
    
    return !parser->hadError;
}

List* listNew() {
    List* list = malloc(sizeof(List));
    list->head = list->tail = NULL;
    list->length = 0;
    return list;
}

List* listAppend(List* list, void* data) {
    ListNode* node = malloc(sizeof(ListNode));
    node->data = data;
    node->next = NULL;
    
    if (list->tail == NULL) {
        list->head = list->tail = node;
    } else {
        list->tail->next = node;
        list->tail = node;
    }
    list->length++;
    return list;
}
