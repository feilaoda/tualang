#include "parser.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include "debug.h"
#include "list.h"

// Debug trace helpers
#ifdef DEBUG
int traceId = 0;
#define parserDebugStart(x) traceId++;for(int a=0;a<traceId;a++){ printf("===="); } printf("+");fprintf(stdout,"<%s(%d) [%.*s],",x,__LINE__,parser->current.length, parser->current.start); debug("token:%d,%s\n",parser->current.type, tokenToString(parser->current.type))
#define parserDebugEnd(x) for(int a=0;a<traceId;a++) {printf("====");} traceId--; printf("-");fprintf(stdout,"/>%s(%d) [%.*s],",x,__LINE__,parser->current.length, parser->current.start); debug("token:%d,%s\n",parser->current.type, tokenToString(parser->current.type))
#define parserDebug(...) for(int a=0;a<traceId;a++) {printf("====");} printf(" %s:%d ", __FILE__, __LINE__); debug(__VA_ARGS__)
#else
#define parserDebugStart(x) ((void)0)
#define parserDebugEnd(x) ((void)0)
#define parserDebug(...) ((void)0)
#endif

#define printError(p,msg) fprintf(stderr,"ERROR: %d", __LINE__); errorPrint(p,msg)


const char* exprTypeToString(ExprType type) {
    switch (type) {
        case EXPR_BINARY: return "Binary";
        case EXPR_UNARY: return "Unary";
        case EXPR_LITERAL: return "Literal";
        case EXPR_VARIABLE: return "Variable";
        case EXPR_GROUPING: return "Grouping";
        case EXPR_CALL: return "Call";
        case EXPR_POSTFIX: return "Postfix";
        case EXPR_PREFIX: return "Prefix";
        case EXPR_ASSIGN: return "Assign";
        default: return "Unknown";
    }
}

// Parser initialization
void initParser(Parser* parser, Lexer* lexer) {
    parser->lexer = lexer;
    parser->hadError = false;
    parser->panicMode = false;
    parser->previous.type = TOKEN_ERROR;
    parser->current.type = TOKEN_ERROR;
}

static Stmt* declaration(Parser* parser);

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
    parserDebug("parser advance scanToken code:[%.*s],token: [%s]\n", parser->current.length,parser->current.start, tokenToString(parser->current.type));
    if (parser->current.type == TOKEN_ERROR) {
        errorAtCurrent(parser, parser->current.start);
    }
}

static bool check(Parser* parser, TokenType type) {
    return parser->current.type == type;
}

static bool match(Parser* parser, TokenType type) {
    if (!check(parser, type)) return false;
    parserDebug("match current type:%d, %s-%s\n", parser->current.type, tokenToString(parser->current.type),tokenToString(type));
    advance(parser);
    parserDebug("match current type:%d, %s-%s\n", parser->current.type, tokenToString(parser->current.type),tokenToString(type));
    return true;
}

static Token consume(Parser* parser, TokenType type, const char* message) {
    if (check(parser, type)) {
        Token token = parser->current;
        advance(parser);
        return token;
    }
    printError(parser, message);
    return (Token){TOKEN_ERROR, NULL, 0, 0};
}

static void synchronize(Parser* parser) {
    parser->panicMode = false;
    while (parser->current.type != TOKEN_EOF) {
        if (parser->previous.type == TOKEN_SEMICOLON) return;
        
        switch (parser->current.type) {
            case TOKEN_FUNC:
            case TOKEN_VAR:
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
        case TOKEN_GT:
        case TOKEN_LE:                    // 添加 <= 运算符
        case TOKEN_GE: return 5;          // 添加 >= 运算符
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


static Stmt* newExpressionStmt(Expr* expression) {
    ExprStmt* stmt = malloc(sizeof(ExprStmt));
    stmt->base.type = STMT_EXPR;
    stmt->expression = expression;
    return (Stmt*)stmt;
}


// Expression parsing
static Expr* parseExpression(Parser* parser) {
    parserDebugStart("parseExpression");
    
    Expr* expr = parseBinaryExpr(parser, 0);
    parserDebug("parseExpression first type:%d\n", expr->type);
#ifdef DEBUG
    printExpr(expr);
#endif
    parserDebug("parser->current.type = %s\n", tokenToString(parser->current.type));
    if (match(parser, TOKEN_ASSIGN)) {
        Token equals = parser->previous;
        Expr* value = parseExpression(parser);
        
        if (expr->type == EXPR_VARIABLE) {
            return newAssignExpr(((VariableExpr*)expr)->name, value);
        }
        
        printError(parser, "Invalid assignment target.");
    }
    
    parserDebug("parseExpression end type:%d\n", expr->type);
    parserDebugEnd("parseExpression");
    return expr;
}

static Expr* parseBinaryExpr(Parser* parser, int minPrec) {
    parserDebugStart("parseBinaryExpr start");
    Expr* left = parseUnaryExpr(parser);
    parserDebug("parseBinaryExpr: left type:%d\n", left->type);
    while (true) {
        TokenType op = parser->current.type;
        int prec = getOperatorPrecedence(op);
        parserDebug("parseBinaryExpr: current token: %s/%d, prec: %d, minPrec: %d\n", 
                   tokenToString(op),op, prec, minPrec);
        
        if (op == TOKEN_ASSIGN && minPrec <= 1) {
            break;
        }

         if (prec == 0 || prec < minPrec) {
            parserDebug("parseBinaryExpr: breaking with current token: %s\n", 
                       tokenToString(parser->current.type));
            break;
        }
        if (match(parser, TOKEN_LPAREN)) {
            left = finishCall(parser, left);
            continue;
        }
        
        if ((op == TOKEN_INC || op == TOKEN_DEC) && prec >= minPrec) {
            advance(parser);
            left = newPostfixExpr(left, parser->previous);
            parserDebug("parseBinaryExpr: inc||dec left type:%d\n", left->type);
            continue;
        }
        
        Token operator = parser->current;
        advance(parser);
        parserDebug("parseBinaryExpr: parse right expr start, op type:%d\n", operator.type);
        Expr* right = parseBinaryExpr(parser, prec + 1);
        left = newBinaryExpr(left, operator, right);
        TokenType op2 = parser->current.type;
        parserDebug("parseBinaryExpr last: left type:%d, operator:%.*s/%s right type:%d, parser current token:%s/%d\n", left->type, operator.length, operator.start, tokenToString(operator.type), right->type, tokenToString(op2),op2);
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
    Expr* expr = NULL;
        
    parserDebug("parsePrimaryExpr: prev1 code:[%.*s],[%s][%s]\n", parser->current.length,parser->current.start, tokenToString(parser->previous.type), tokenToString(parser->current.type));

    if (match(parser, TOKEN_INT) ||
        match(parser, TOKEN_LONG) ||
        match(parser, TOKEN_DOUBLE) ||
        match(parser, TOKEN_STRING_LITERAL) ||
        match(parser, TOKEN_TRUE) ||
        match(parser, TOKEN_FALSE)) {
        expr = newLiteralExpr(parser->previous);
    } else if (match(parser, TOKEN_IDENTIFIER)) {
        parserDebug("parsePrimaryExpr: current code:[%.*s],[%s]\n", parser->current.length,parser->current.start, tokenToString(parser->previous.type));
        expr = newVariableExpr(parser->previous);
        
        // Function call: identifier followed by '('
        if (match(parser, TOKEN_LPAREN)) {

            expr = finishCall(parser, expr);
        }
    } else if (match(parser, TOKEN_LPAREN)) {
        expr = parseExpression(parser);
        consume(parser, TOKEN_RPAREN, "Expect ')' after expression");
        expr = newGroupingExpr(expr);
    } else if (match(parser, TOKEN_PRINTLN)){
        expr = newVariableExpr(parser->previous);
        if (match(parser, TOKEN_LPAREN)) {
            expr = finishCall(parser, expr);
        }
    } else {
        printError(parser, "parsePrimaryExpr Expected expression");
        expr = NULL;
    }

    parserDebugEnd("parsePrimaryExpr");
    return expr;
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
    bool hasParen = false;
    if (match(parser, TOKEN_LPAREN)) {
        hasParen = true;
    }
    Expr* condition = parseExpression(parser);
    if (hasParen) {
        consume(parser, TOKEN_RPAREN, "Expect ')' after condition");
    }
    
    Stmt* thenBranch = parseStatement(parser);
    Stmt* elseBranch = NULL;
    
    if (match(parser, TOKEN_ELSE)) {
        elseBranch = parseStatement(parser);
    }
    
    return newIfStmt(condition, thenBranch, elseBranch);
}

// static Stmt* parseForStatement2(Parser* parser) {
//     parserDebugStart("parseForStatement");
//     bool hasParen = false;
    
//     if (check(parser, TOKEN_LPAREN)) {
//         consume(parser, TOKEN_LPAREN, "Expect '(' after 'for'");
//         hasParen = true;
//     }
    
//     Stmt* initializer = NULL;
//     Expr* rangeExpr = NULL;
    
//     if (match(parser, TOKEN_SEMICOLON)) {
//         initializer = NULL;
//     } else if (match(parser, TOKEN_LET)) {
//         initializer = parseVarDeclaration(parser, false);
//     } else {
//         if (check(parser, TOKEN_IDENTIFIER)) {
//             Token current = parser->current;
//             advance(parser);
//             parserDebug("parseForStatement0: in [%.*s],[%s]\n", parser->current.length,parser->current.start, tokenToString(parser->current.type));
            
//             if (check(parser, TOKEN_COLON)) {
//                 initializer = parseVarDeclaration(parser, true);
//             } else if (check(parser, TOKEN_IN)) {
//                 parserDebug("parseForStatement1: in [%s]\n", tokenToString(parser->current.type));
//                 advance(parser); // consume 'in'
//                 parserDebug("parseForStatement2: in [%s]\n", tokenToString(parser->current.type));
//                 rangeExpr = parseExpression(parser);
                
//                 if (hasParen) {
//                     consume(parser, TOKEN_RPAREN, "Expect ')' after range");
//                 }
                
//                 Stmt* body = parseStatement(parser);
//                 ForInStmt* stmt = malloc(sizeof(ForInStmt));
//                 stmt->base.type = STMT_FOR_IN;
//                 stmt->loopVar = current;
//                 stmt->range = rangeExpr;
//                 stmt->body = body;
                
//                 parserDebugEnd("parseForStatement");
//                 return (Stmt*)stmt;
//             } else if(check(parser, TOKEN_ASSIGN)) {
//                 parserDebug("parseForStatement3: in [%s]\n", tokenToString(parser->current.type));
//                 advance(parser); // consume '='
//                 initializer = newVarStmt(current, NULL, parseExpression(parser), false);
//             } else {
//                 parser->current = current;
//                 initializer = parseExpressionStatement(parser);
//             }
//         } else {
//             initializer = parseExpressionStatement(parser);
//         }
//     }
    
//     // Standard for loop
//     Expr* condition = NULL;
//     if (!check(parser, TOKEN_SEMICOLON)) {
//         condition = parseExpression(parser);
//     }
//     consume(parser, TOKEN_SEMICOLON, "Expect ';' after loop condition");
    
//     Expr* increment = NULL;
//     if (!check(parser, TOKEN_RPAREN) && !check(parser, TOKEN_LBRACE)) {
//         increment = parseExpression(parser);
//     }
    
//     if (hasParen) {
//         consume(parser, TOKEN_RPAREN, "Expect ')' after for clauses");
//     }
    
//     Stmt* body = parseStatement(parser);
    
//     ForStmt* stmt = malloc(sizeof(ForStmt));
//     stmt->base.type = STMT_FOR;
//     stmt->initializer = initializer;
//     stmt->condition = condition;
//     stmt->increment = increment;
//     stmt->body = body;
    
//     parserDebugEnd("parseForStatement");
//     return (Stmt*)stmt;
// }

static Stmt* parseForStatement(Parser* parser) {
    parserDebugStart("parseForStatement");
    
    bool hasParen = false;
    if (check(parser, TOKEN_LPAREN)) {
        consume(parser, TOKEN_LPAREN, "Expect '(' after 'for'");
        hasParen = true;
    }
    Stmt* initializer = NULL;
    // Parse initializer/iterator
    if (match(parser, TOKEN_VAR)) {
        // Case: for var i=0; i<10; i++ {}
        //for var i:int =0; i<10; i++ {}
        initializer = parseVarDeclaration(parser, false);
    } else if (check(parser, TOKEN_IDENTIFIER)) {
        Token current = parser->current;
        advance(parser);
        
        if (check(parser, TOKEN_IN)) {
            // Case: for i in range(1,10) {}
            advance(parser); // consume 'in'
            Expr* rangeExpr = parseExpression(parser);
            if (hasParen) {
                consume(parser, TOKEN_RPAREN, "Expect ')' after range");
            }
            consume(parser, TOKEN_LBRACE, "Expect '{' before loop body");
            Stmt* body = parseBlockStatement(parser);
            
            ForInStmt* stmt = malloc(sizeof(ForInStmt));
            stmt->base.type = STMT_FOR_IN;
            stmt->loopVar = current;
            stmt->range = rangeExpr;
            stmt->body = body;
            return (Stmt*)stmt;
        } 
        // else if (check(parser, TOKEN_COLON)) {
        //     // Case: for i:int=0; i<10; i++ {}
        //     initializer = parseVarDeclaration(parser, true);
        // } 
        else if (check(parser, TOKEN_ASSIGN)) {
            // Case: for i=0; i<10; i++ {}
            advance(parser); // consume '='
            Expr* value = parseExpression(parser);
            consume(parser, TOKEN_SEMICOLON, "Expect ';' after initializer");
            initializer = newVarStmt(current, NULL, value, false);
        } else {
            parser->current = current;
            initializer = parseExpressionStatement(parser);
        }
    }
    
    // Parse condition
    Expr* condition = NULL;
    if (!check(parser, TOKEN_SEMICOLON)) {
        condition = parseExpression(parser);
    }
    consume(parser, TOKEN_SEMICOLON, "Expect ';' after loop condition");
    
    // Parse increment
    Expr* increment = NULL;
    if (!check(parser, TOKEN_RPAREN) && !check(parser, TOKEN_LBRACE)) {
        parserDebug("parseForStatement: increment\n");
        increment = parseExpression(parser);
        parserDebug("parseForStatement: increment type:%d\n", increment->type);

    }
    
    if (hasParen) {
        consume(parser, TOKEN_RPAREN, "Expect ')' after for clauses");
    }
    
    consume(parser, TOKEN_LBRACE, "Expect '{' before loop body");
    Stmt* body = parseBlockStatement(parser);
    
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
        listAppend(statements, declaration(parser));
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
    
    if (check(parser, TOKEN_SEMICOLON)) {
        consume(parser, TOKEN_SEMICOLON, "Expect ';' after return value");
    }
    
    ReturnStmt* stmt = malloc(sizeof(ReturnStmt));
    stmt->base.type = STMT_RETURN;
    stmt->keyword = keyword;
    stmt->value = value;
    return (Stmt*)stmt;
}

static Stmt* parseExpressionStatement(Parser* parser) {
    parserDebugStart("parseExpressionStatement");
    Expr* expr = parseExpression(parser);
    parserDebug("expr type: [%d]\n", expr->type);
    if(check(parser, TOKEN_SEMICOLON)){
        consume(parser, TOKEN_SEMICOLON, "Expect ';' after expression");
    }
    ExprStmt* stmt = malloc(sizeof(ExprStmt));
    stmt->base.type = STMT_EXPR;
    stmt->expression = expr;
    parserDebugEnd("parseExpressionStatement");
    return (Stmt*)stmt;
}
// static Stmt* parseVarDeclaration(Parser* parser, bool identifierConsumed) {
//     parserDebugStart("parseVarDeclaration");
//     bool isConst = parser->previous.type == TOKEN_CONST;
//     bool hasVar = parser->previous.type == TOKEN_VAR;
    
//     Token name;
//     if (identifierConsumed) {
//         name = parser->previous;
//     } else {
//         name = consume(parser, TOKEN_IDENTIFIER, "Expect variable name");
//     }
    
//     Type* type = NULL;
//     if (match(parser, TOKEN_COLON)) {
//         type = parseType(parser);
//     }
    
//     Expr* initializer = NULL;
//     if (match(parser, TOKEN_ASSIGN)) {
//         initializer = parseExpression(parser);
//     }
    
//     consume(parser, TOKEN_SEMICOLON, "Expect ';' after variable declaration");
    
//     parserDebugEnd("parseVarDeclaration");
//     return newVarStmt(name, type, initializer, isConst);
// }
static Stmt* parseVarDeclaration(Parser* parser, bool identifierConsumed) {
    parserDebugStart("parseVarDeclaration");
    
    // Create list for multiple declarations
    List* declarations = listNew();
    bool isConst = parser->previous.type == TOKEN_CONST;
    bool hasVar = parser->previous.type == TOKEN_VAR;

    bool first = true;
    do {
        // Parse variable name
        Token name;
        if (first && identifierConsumed) {
            name = parser->previous;
        } else {
            name = consume(parser, TOKEN_IDENTIFIER, "Expect variable name");
        }
        first = false;
        
        bool hasType = false;
        // Parse optional type annotation
        Type* type = NULL;
        if (match(parser, TOKEN_COLON)) {
            type = parseType(parser);
            hasType = true;
        }
        
        // Parse initializer if present
        Expr* initializer = NULL;
        if (match(parser, TOKEN_ASSIGN)) {
            initializer = parseExpression(parser);
            hasType = true;
        }

        if (hasType == false)
        {
            /* throw error */
        }
        
        
        // Create var statement
        Stmt* varStmt = newVarStmt(name, type, initializer, isConst);
        // VarStmt* varStmt = malloc(sizeof(VarStmt));
        // varStmt->base.type = STMT_VAR;
        // varStmt->name = name;
        // varStmt->type = type;
        // varStmt->initializer = initializer;
        // varStmt->isConst = isConst;
        
        // Add to declarations list
        listAppend(declarations, varStmt);
        
    } while (match(parser, TOKEN_COMMA));
    
    if(check(parser, TOKEN_SEMICOLON)){
        consume(parser, TOKEN_SEMICOLON, "Expect ';' after variable declaration");
    }
    
    // If only one declaration, return it directly
    if (declarations->length == 1) {
        VarStmt* stmt = (VarStmt*)declarations->head->data;
        free(declarations);
        parserDebugEnd("parseVarDeclaration");
        return (Stmt*)stmt;
    }
    
    // Create block statement for multiple declarations
    BlockStmt* block = malloc(sizeof(BlockStmt));
    block->base.type = STMT_BLOCK;
    block->statements = declarations;
    
    parserDebugEnd("parseVarDeclaration");
    return (Stmt*)block;
}
// Block parsing
static List* parseBlock(Parser* parser) {
    List* statements = listNew();
    
    consume(parser, TOKEN_LBRACE, "Expect '{' before block");
    
    while (!check(parser, TOKEN_RBRACE) && !check(parser, TOKEN_EOF)) {
        Stmt* stmt = declaration(parser);
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
    } else if (check(parser, TOKEN_INT) ||
               check(parser, TOKEN_LONG) ||
               check(parser, TOKEN_DOUBLE) ||
               check(parser, TOKEN_STRING) ||
               check(parser, TOKEN_BOOL)) {
        // Support `fn demo(...) int {}` in addition to `fn demo(...) -> int {}`
        returnType = parseType(parser);
    }
    
    List* body = parseBlock(parser);
    
    return newFuncStmt(name, parameters, returnType, body);
}

static Stmt* parseStructDeclaration(Parser* parser) {
    parserDebugStart("parseStructDeclaration");
    
    // Parse struct name
    Token name = consume(parser, TOKEN_IDENTIFIER, "Expect struct name");
    
    // Parse opening brace
    consume(parser, TOKEN_LBRACE, "Expect '{' before struct body");
    
    // Create lists for fields and methods
    List* fields = listNew();
    List* methods = listNew();
    
    // Parse fields and methods
    while (!check(parser, TOKEN_RBRACE) && !check(parser, TOKEN_EOF)) {
        if (match(parser, TOKEN_FUNC)) {
            // Parse method
            FuncStmt* method = (FuncStmt*)parseFunctionDeclaration(parser);
            listAppend(methods, method);
        } else {
            // Parse field
            Token fieldName = consume(parser, TOKEN_IDENTIFIER, "Expect field name");
            consume(parser, TOKEN_COLON, "Expect ':' after field name");
            Type* fieldType = parseType(parser);
            if(check(parser, TOKEN_COMMA)) {
            consume(parser, TOKEN_COMMA, "Expect ',' after field type");
            }
            // Create field declaration
            FieldDeclaration* field = malloc(sizeof(FieldDeclaration));
            field->name = fieldName;
            field->type = fieldType;
            listAppend(fields, field);
        }
    }
    
    consume(parser, TOKEN_RBRACE, "Expect '}' after struct body");
    
    // Create struct declaration
    StructStmt* stmt = malloc(sizeof(StructStmt));
    stmt->base.type = STMT_STRUCT;
    stmt->name = name;
    stmt->fields = fields;
    stmt->methods = methods;
    
    parserDebugEnd("parseStructDeclaration");
    return (Stmt*)stmt;
}

static Type* parseType(Parser* parser) {
    if (match(parser, TOKEN_INT)) {
        Type* type = malloc(sizeof(Type));
        type->kind = TYPE_INT;
        return type;
    }
    if (match(parser, TOKEN_LONG)) {
        Type* type = malloc(sizeof(Type));
        type->kind = TYPE_LONG;
        return type;
    }
    if (match(parser, TOKEN_DOUBLE)) {
        Type* type = malloc(sizeof(Type));
        type->kind = TYPE_DOUBLE;
        return type;
    }
    if (match(parser, TOKEN_STRING)) {
        Type* type = malloc(sizeof(Type));
        type->kind = TYPE_STRING;
        return type;
    }
    if (match(parser, TOKEN_BOOL)) {
        Type* type = malloc(sizeof(Type));
        type->kind = TYPE_BOOL;
        return type;
    }
    printError(parser, "Expect type name");
    return NULL;
}

static Stmt* declaration(Parser* parser) {
    if (match(parser, TOKEN_FUNC)) {
        return parseFunctionDeclaration(parser);
    }
    
    if (match(parser, TOKEN_VAR) || match(parser, TOKEN_CONST)) {
        return parseVarDeclaration(parser, false);
    }

    if (match(parser, TOKEN_STRUCT))
    {
        /* code */
        return parseStructDeclaration(parser);
    }
    
    
    // if (check(parser, TOKEN_IDENTIFIER)) {
    //     Token current = parser->current;
    //     advance(parser);
        
    //     if (check(parser, TOKEN_COLON)) {
    //         return parseVarDeclaration(parser, true);
    //     }
    //     parser->current = current;
    // }

     if (check(parser, TOKEN_IDENTIFIER)) {
        // Save current state
        Token current = parser->current;
        const char* lexerStart = parser->lexer->start;
        const char* lexerCurrent = parser->lexer->current;
        
        advance(parser);
        
        if (check(parser, TOKEN_COLON)) {
            // Variable declaration with type annotation
            return parseVarDeclaration(parser, true);
        }else if(check(parser, TOKEN_LPAREN)){
            // Function call
            parser->current = current;
            parser->lexer->start = lexerStart;
            parser->lexer->current = lexerCurrent;
            return parseExpressionStatement(parser);
        }
        
        // Restore state and parse as normal statement 
        // (could be function call or other expression)
        parser->current = current;
        parser->lexer->start = lexerStart;
        parser->lexer->current = lexerCurrent;
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
