#include "parser.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include "debug.h"

int traceId = 0;
#define parserDebugStart(x) traceId++;for(int a=0;a<traceId;a++){ printf("===="); } fprintf(stdout," <%s(%d) [%.*s],",x,__LINE__,parser->current.length, parser->current.start); debug("token:%d,%s\n",parser->current.type, tokenToString(parser->current.type))
#define parserDebugEnd(x) for(int a=0;a<traceId;a++) {printf("====");} traceId--; fprintf(stdout," />%s(%d) [%.*s],",x,__LINE__,parser->current.length, parser->current.start); debug("token:%d,%s\n",parser->current.type, tokenToString(parser->current.type))
#define parserDebug(...) for(int a=0;a<traceId;a++) {printf("====");} printf(" "); debug(__VA_ARGS__)


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
static void advance(Parser* parser) {
    parser->previous = parser->current;
    parser->current = scanToken(parser->lexer);
    parserDebug("advance scantoken: [%s]\n", tokenToString(parser->current.type));
    if (parser->current.type == TOKEN_ERROR) {
        errorAtCurrent(parser, parser->current.start);
    }
}


static bool check(Parser* parser, TokenType type) {
    // printf("check curr:%d, %s\n", parser->current.line, tokenToString(parser->current.type));
    return parser->current.type == type;
}



static bool match(Parser* parser, TokenType type) {
    if (!check(parser, type)) return false;
    advance(parser);
    return true;
}


static void errorAt(Parser* parser, Token* token, const char* message) {
    if (parser->panicMode) return;
    parser->panicMode = true;
    fprintf(stderr, "[line %d] Error", token->line);
    
    if (token->type == TOKEN_EOF) {
        fprintf(stderr, " at end");
    } else if (token->type == TOKEN_ERROR) {
        // Nothing
    } else {
        fprintf(stderr, " at '%.*s' type(%s)", token->length, token->start, tokenToString(token->type));
    }
    
    fprintf(stderr, ": %s\n", message);
    parser->hadError = true;
}

static Type* parseType(Parser* parser) {
    Type* type = (Type*)malloc(sizeof(Type));
    
    if (match(parser, TOKEN_INT)) {
        type->kind = TYPE_INT;
    } else if (match(parser, TOKEN_STRING)) {
        type->kind = TYPE_STRING;
    } else {
        errorAt(parser, &parser->current, "Expected type name");
        return NULL;
    }
    
    return type;
}

static Expr* parseExpression(Parser* parser) {
    parserDebugStart("parseExpression start");
    Expr *expr = parseBinaryExpr(parser, 0);
    parserDebugEnd("parseExpression end");
    return expr;
}

static Expr* parseBinaryExpr(Parser* parser, int minPrec) {
    parserDebugStart("parseBinaryExpr start");
    Expr* left = parseUnaryExpr(parser);
    parserDebug("parseBinaryExpr left end\n");
    while (true) {
        TokenType op = parser->current.type;
        int prec = getOperatorPrecedence(parser->current.type);
        parserDebug("parseBinaryExpr current op\n");
        parserDebug("op: %s, prec:%d, minPrec:%d\n", tokenToString(op), prec, minPrec);
        if (prec == 0 || prec < minPrec) break;
         // Handle post-increment/decrement

        if (op == TOKEN_INC || op == TOKEN_DEC) {
            advance(parser); // consume ++ or --
            parserDebug("parseBinaryExpr post-increment/decrement");
            left = newUnaryExpr(parser->previous, left);
            continue;
        }
        Token token = parser->current;
        parserDebug("parseBinaryExpr token\n");
        // debug("<> token: %s\n",tokenToString(token.type));
        advance(parser);
        parserDebug("parseBinaryExpr right start\n");
        Expr* right = parseBinaryExpr(parser, prec + 1);
        parserDebug("parseBinaryExpr right end, left & right\n");
        // debug("expr: %d %d/%s %d, %s\n" , left->token.type, token.type, tokenToString(token.type), right->token.type, tokenToString(parser->current.type));
        left = newBinaryExpr(left, token, right);
    }
    parserDebugEnd("parseBinaryExpr end");
    return left;
}

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
        default: return 0;                // Non-operators
    }
}

static Expr* parseUnaryExpr(Parser* parser) {
    parserDebugStart("parseUnaryExpr start");
    // Handle prefix operators: ++, --, -, !
    if (match(parser, TOKEN_INC) || 
        match(parser, TOKEN_DEC) ||
        match(parser, TOKEN_MINUS) || 
        match(parser, TOKEN_NOT)) {
        Token operator = parser->previous;
        Expr* right = parseUnaryExpr(parser);
        parserDebugEnd("parseUnaryExpr end ++--");
        return newUnaryExpr(operator, right);
    }
    
    // Parse primary expression
    Expr* expr = parsePrimaryExpr(parser);
    
    // Handle postfix operators: ++, --
    // if (match(parser, TOKEN_INC) || match(parser, TOKEN_DEC)) {
    //     Token operator = parser->previous;
    //     parserDebug("parseUnaryExpr newPostfixExpr start");
    //     return newPostfixExpr(expr, operator);
    // }
    parserDebugEnd("parseUnaryExpr end");
    return expr;
}

static Expr* parsePrimaryExpr(Parser* parser) {
    // parserDebugStart("parsePrimaryExpr start");
    if (match(parser, TOKEN_NUMBER)) {
        return newLiteralExpr(parser->previous);
    }
    
    if (match(parser, TOKEN_STRING_LITERAL)) {
        return newLiteralExpr(parser->previous);
    }
    
    if (match(parser, TOKEN_IDENTIFIER)) {
        Expr* expr = newVariableExpr(parser->previous);
        
        // Handle postfix ++ and --
        // if (match(parser, TOKEN_INC) || match(parser, TOKEN_DEC)) {
        //     Token operator = parser->previous;
        //     parserDebug("parsePrimaryExpr newPostfixExpr start");
        //     return newPostfixExpr(expr, operator);
        // }
        
        return expr;
    }
    
    if (match(parser, TOKEN_LPAREN)) {
        Expr* expr = parseExpression(parser);
        consume(parser, TOKEN_RPAREN, "Expect ')' after expression");
        return newGroupingExpr(expr);
    }

    // Handle prefix ++ and --
    if (match(parser, TOKEN_INC) || match(parser, TOKEN_DEC)) {
        Token operator = parser->previous;
        Expr* right = parsePrimaryExpr(parser);
        return newUnaryExpr(operator, right);
    }
    
    errorAt(parser, &parser->current, "Expected expression");
    return NULL;
}


// List operations
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

// AST Node constructors
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


static Stmt* parseVarDeclaration(Parser* parser, bool identifierConsumed) {
    parserDebugStart("parseVarDeclaration start");
    bool isConst = parser->previous.type == TOKEN_CONST;
    bool hasLet = parser->previous.type == TOKEN_LET;
    parserDebug("parseVarDeclaration isConst:%d, hasLet:%d token:%s\n", isConst, hasLet, tokenToString(parser->current.type));  
    Token name;
    if(identifierConsumed) {
        name = parser->previous;
    }else {
        Token name = parser->current;
        advance(parser);
    }
    

    parserDebug("parseVarDeclaration2 isConst:%d, hasLet:%d token:%s\n", isConst, hasLet, tokenToString(parser->current.type));  
    
    // Handle type annotation if present
    Type* type = NULL;
    if (match(parser, TOKEN_COLON)) {
        type = parseType(parser);
    }
    
    Expr* initializer = NULL;
    if (match(parser, TOKEN_ASSIGN)) {
        initializer = parseExpression(parser);
    }
    
    consume(parser, TOKEN_SEMICOLON, "Expect ';' after variable declaration");
    parserDebugEnd("parseVarDeclaration end");
    return newVarStmt(name, type, initializer, isConst);
}

static Stmt* parseFunctionDeclaration(Parser* parser) {
    Token name = consume(parser, TOKEN_IDENTIFIER, "Expect function name");
    consume(parser, TOKEN_LPAREN, "Expect '(' after function name");
    
    // Parse parameters
    List *params = listNew();
    if (!check(parser, TOKEN_RPAREN)) {
        do {
            params = listAppend(params, parseFunctionParameter(parser));
        } while (match(parser, TOKEN_COMMA));
    }
    printf("start parse func TOKEN_RPAREN )\n");

    consume(parser, TOKEN_RPAREN, "Expect ')' after parameters");
    
    // Parse return type if present
    printf("start parse func arraw->\n");
    Type* returnType = NULL;
    if (match(parser, TOKEN_ARROW)) {
        returnType = parseType(parser);
    }
    
    // Parse function body
    consume(parser, TOKEN_LBRACE, "Expect '{' before function body");
    List *body = parseBlock(parser);
    
    return newFuncStmt(name, params, returnType, body);
}

// Block parsing
static List* parseBlock(Parser* parser) {
    List* statements = listNew();
    
    consume(parser, TOKEN_LBRACE, "Expect '{' before block");
    
    while (!check(parser, TOKEN_RBRACE) && !check(parser, TOKEN_EOF)) {
        listAppend(statements, parseStatement(parser));
    }
    
    consume(parser, TOKEN_RBRACE, "Expect '}' after block");
    return statements;
}

// Function parameter parsing
static Parameter* parseFunctionParameter(Parser* parser) {
    Parameter* param = malloc(sizeof(Parameter));
    param->name = consume(parser, TOKEN_IDENTIFIER, "Expect parameter name");
    
    consume(parser, TOKEN_COLON, "Expect ':' after parameter name");
    param->type = parseType(parser);
    printf("parseFunctionParameter end\n");
    return param;
}

// Statement parsing
static Stmt* parseStatement(Parser* parser) {
    parserDebugStart("parseStatement start");
    Stmt* stmt = NULL;
    if (match(parser, TOKEN_IF)) stmt = parseIfStatement(parser);
    else 
    if (match(parser, TOKEN_FOR)) stmt = parseForStatement(parser);
    else 

    if (match(parser, TOKEN_RETURN)) stmt = parseReturnStatement(parser);
    else 

    if (match(parser, TOKEN_LBRACE)) stmt = parseBlockStatement(parser);
    else 

    stmt = parseExpressionStatement(parser);
    parserDebugEnd("parseStatement end");
    return stmt;
}

static Stmt* parseIfStatement(Parser* parser) {
    consume(parser, TOKEN_LPAREN, "Expect '(' after 'if'");
    Expr* condition = parseExpression(parser);
    consume(parser, TOKEN_RPAREN, "Expect ')' after if condition");
    
    Stmt* thenBranch = parseStatement(parser);
    Stmt* elseBranch = NULL;
    
    if (match(parser, TOKEN_ELSE)) {
        elseBranch = parseStatement(parser);
    }
    
    return newIfStmt(condition, thenBranch, elseBranch);
}

static Stmt* parseForPrepareStatement(Parser* parser) {
    return NULL;
}

static Stmt* parseForStatement(Parser* parser) {
    parserDebugStart("parseForStatement start");
    bool hasParen = false;
    if(check(parser, TOKEN_LPAREN)) {
        consume(parser, TOKEN_LPAREN, "Expect '(' after 'for'");
        hasParen = true;
    }
    
    // Parse initializer or loop variable
    Stmt* initializer = NULL;
    Expr* rangeExpr = NULL;
    
    if (match(parser, TOKEN_SEMICOLON)) {
        initializer = NULL;
    } else if (match(parser, TOKEN_LET)) {
        initializer = parseVarDeclaration(parser, false);
    } else {
        // Check for identifier:type form or for-in loop
        if (check(parser, TOKEN_IDENTIFIER)) {
            Token current = parser->current;
            advance(parser);
            
            if (check(parser, TOKEN_COLON)) {
                // Variable declaration with type
                initializer = parseVarDeclaration(parser, true);
            } else if (check(parser, TOKEN_IN)) {
                // For-in loop
                advance(parser); // consume 'in'
                rangeExpr = parseExpression(parser);
                if (hasParen) {
                    consume(parser, TOKEN_RPAREN, "Expect ')' after range expression");
                }
                Stmt* body = parseStatement(parser);
                
                ForInStmt* stmt = malloc(sizeof(ForInStmt));
                stmt->base.type = STMT_FOR_IN;
                stmt->loopVar = current;
                stmt->range = rangeExpr;
                stmt->body = body;
                parserDebugEnd("parseForStatement end (for-in)");
                return (Stmt*)stmt;
            } else {
                // Expression statement
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
    
    parserDebugEnd("parseForStatement end (standard)");
    return (Stmt*)stmt;
}

static Stmt* parseForStatement2(Parser* parser) {
    parserDebugStart("parseForStatement start");
    bool hasParen = false;
    if(check(parser, TOKEN_LPAREN)) {
        consume(parser, TOKEN_LPAREN, "Expect '(' after 'for'");
        hasParen = true;
    }
    Stmt* initializer;
    Expr* rangeExpr;
    if (match(parser, TOKEN_SEMICOLON)) {
        initializer = NULL;
    } else if (match(parser, TOKEN_LET)) {
        initializer = parseVarDeclaration(parser,false);
    } else {
         // Check for identifier:type form
        if (check(parser, TOKEN_IDENTIFIER)) {
            // Save current position
            parserDebug("parseForStatement check identifier\n");
            Token current = parser->current;
            advance(parser); // Look at next token
            
            if (check(parser, TOKEN_COLON)) {
                // This is a variable declaration
                // Restore position
                initializer = parseVarDeclaration(parser,true);
            }else if(check(parser, TOKEN_IN)) {
                Expr* rangeExpr = parseExpression(parser);
            }else {
                parser->current = current;
                initializer = parseExpressionStatement(parser);
            }
            // Not a variable declaration, restore and parse as statement
        }else {
            initializer = parseExpressionStatement(parser);
        }
    }
    parserDebug("parseForStatement initializer end\n");
    Expr* condition = NULL;
    if (!check(parser, TOKEN_SEMICOLON)) {
        condition = parseExpression(parser);
    }
    consume(parser, TOKEN_SEMICOLON, "Expect ';' after loop condition");
    
    Expr* increment = NULL;
    if (!check(parser, TOKEN_RPAREN)) {
        increment = parseExpression(parser);
    }
    if(hasParen) {
        consume(parser, TOKEN_RPAREN, "Expect ')' after for clauses");
    }
    parserDebug("parseForBodyStatement start\n");
    
    Stmt* body = parseStatement(parser);
    parserDebugEnd("parseForBodyStatement end");
    return newForStmt(initializer, condition, increment, body);
}

static Stmt* parseReturnStatement(Parser* parser) {
    Token keyword = parser->previous;
    
    Expr* value = NULL;
    if (!check(parser, TOKEN_SEMICOLON)) {
        value = parseExpression(parser);
    }
    
    consume(parser, TOKEN_SEMICOLON, "Expect ';' after return value");
    return newReturnStmt(keyword, value);
}



static Stmt* parseBlockStatement(Parser* parser) {
    parserDebug("parseBlockStatement start");
    List* statements = listNew();
    
    while (!check(parser, TOKEN_RBRACE) && !check(parser, TOKEN_EOF)) {
        listAppend(statements, parseStatement(parser));
    }
    
    consume(parser, TOKEN_RBRACE, "Expect '}' after block");
    parserDebug("parseBlockStatement end\n");
    return newBlockStmt(statements);
}

static Stmt* parseExpressionStatement(Parser* parser) {
    parserDebugStart("parseExpressionStatement start");

    Type*type = NULL;

    Expr* expr = parseExpression(parser);
    parserDebug("parseExpressionStatement ended, [%s]\n", tokenToString(parser->current.type));
    consume(parser, TOKEN_SEMICOLON, "Expect ';' after expression");
    parserDebug("parseExpressionStatement consume ended, [%s]\n", tokenToString(parser->current.type));
    
    Stmt* stmt = newExpressionStmt(expr);
    parserDebugEnd("parseExpressionStatement end");
    return stmt;
}

static Stmt* newReturnStmt(Token keyword, Expr* value) {
    ReturnStmt* stmt = malloc(sizeof(ReturnStmt));
    stmt->base.type = STMT_RETURN;
    stmt->keyword = keyword;
    stmt->value = value;
    return (Stmt*)stmt;
}


static Stmt* newBlockStmt(List* statements) {
    BlockStmt* stmt = malloc(sizeof(BlockStmt));
    stmt->base.type = STMT_BLOCK;
    stmt->statements = statements;
    return (Stmt*)stmt;
}



static Stmt* newExpressionStmt(Expr* expression) {
    ExprStmt* stmt = malloc(sizeof(ExprStmt));
    stmt->base.type = STMT_EXPR;
    stmt->expression = expression;
    return (Stmt*)stmt;
}

static Expr* newBinaryExpr(Expr* left, Token operator, Expr* right) {
    BinaryExpr* expr = malloc(sizeof(BinaryExpr));
    expr->base.type = EXPR_BINARY;
    expr->base.token = operator;
    expr->left = left;
    expr->operator = operator;
    expr->right = right;
    return (Expr*)expr;
}

static Expr* newUnaryExpr(Token operator, Expr* right) {
    UnaryExpr* expr = malloc(sizeof(UnaryExpr));
    expr->base.type = EXPR_UNARY;
    expr->base.token = operator;
    expr->operator = operator;
    expr->right = right;
    return (Expr*)expr;
}

static Expr* newLiteralExpr(Token value) {
    LiteralExpr* expr = malloc(sizeof(LiteralExpr));
    expr->base.type = EXPR_LITERAL;
    expr->base.token = value;
    expr->value = value;
    return (Expr*)expr;
}

static Expr* newVariableExpr(Token name) {
    VariableExpr* expr = malloc(sizeof(VariableExpr));
    expr->base.type = EXPR_VARIABLE;
    expr->base.token = name;
    expr->name = name;
    return (Expr*)expr;
}

static Expr* newGroupingExpr(Expr* expression) {
    GroupingExpr* expr = malloc(sizeof(GroupingExpr));
    expr->base.type = EXPR_GROUPING;
    expr->expression = expression;
    return (Expr*)expr;
}

static Expr* newPostfixExpr(Expr* operand, Token operator) {
    PostfixExpr* expr = malloc(sizeof(PostfixExpr));
    expr->base.type = EXPR_POSTFIX;
    expr->base.token = operator;
    expr->operand = operand;
    expr->operator = operator;
    return (Expr*)expr;
}

static Stmt* newIfStmt(Expr* condition, Stmt* thenBranch, Stmt* elseBranch) {
    IfStmt* stmt = malloc(sizeof(IfStmt));
    stmt->base.type = STMT_IF;
    stmt->condition = condition;
    stmt->thenBranch = thenBranch;
    stmt->elseBranch = elseBranch;
    return (Stmt*)stmt;
}
static Stmt* newForStmt(Stmt* initializer, Expr* condition, Expr* increment, Stmt* body) {
    ForStmt* stmt = malloc(sizeof(ForStmt));
    stmt->base.type = STMT_FOR;
    stmt->initializer = initializer;
    stmt->condition = condition;
    stmt->increment = increment;
    stmt->body = body;
    return (Stmt*)stmt;
}








static Stmt* declaration(Parser* parser) {
    if (match(parser, TOKEN_FUNC)) {
        return parseFunctionDeclaration(parser);
    } else if (match(parser, TOKEN_LET) || match(parser, TOKEN_CONST)) {
        return parseVarDeclaration(parser,false);
    } else {
        return parseStatement(parser);
    }
}
static void synchronize(Parser* parser) {
    printf("synchronize start\n");
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
            default: ; // Do nothing
        }
        
        advance(parser);
    }
}

bool parse(Parser* parser, List** statements) {
    parser->hadError = false;
    parser->panicMode = false;
    
    // Create list to store statements
    *statements = listNew();
    
    // Prime the parser with first token
    advance(parser);
    
    // Parse declarations until EOF
    while (!match(parser, TOKEN_EOF)) {
        printf("parse start, current:%d line:%d\n", parser->current.type, parser->current.line);
        Stmt* stmt = declaration(parser);
        if (stmt != NULL) {
            listAppend(*statements, stmt);
        }
        
        // Error recovery
        if (parser->panicMode) {
            // synchronize(parser);
            return false;
        }
        printf("parse end, line:%d\n", parser->current.line);
    }
    
    return !parser->hadError;
}

static Token consume(Parser* parser, TokenType type, const char* message) {
    if (parser->current.type == type) {
        parserDebug("consume current type:%d, %s-%s\n", parser->current.type, tokenToString(parser->current.type),tokenToString(type));
        Token token = parser->current;
        advance(parser);
        return token;
    }
    
    errorAt(parser, &parser->current, message);
    return (Token){TOKEN_ERROR, NULL, 0, 0};
}
void initParser(Parser* parser, Lexer *lexer) {
    parser->lexer = lexer;
    parser->hadError = false;
    parser->panicMode = false;
    parser->previous.type = TOKEN_ERROR;
    parser->current.type = TOKEN_ERROR;
}