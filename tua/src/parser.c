#include "parser.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

#define printError(p,msg) errorPrint((p),(msg))


const char* exprTypeToString(ExprType type) {
    switch (type) {
        case EXPR_BINARY: return "Binary";
        case EXPR_UNARY: return "Unary";
        case EXPR_LITERAL: return "Literal";
        case EXPR_VARIABLE: return "Variable";
        case EXPR_GROUPING: return "Grouping";
        case EXPR_CALL: return "Call";
        case EXPR_CAST: return "Cast";
        case EXPR_POSTFIX: return "Postfix";
        case EXPR_PREFIX: return "Prefix";
        case EXPR_ASSIGN: return "Assign";
        case EXPR_GET: return "Get";
        case EXPR_SET: return "Set";
        case EXPR_LAMBDA: return "Lambda";
        case EXPR_MAP_LITERAL: return "MapLiteral";
        case EXPR_ARRAY_LITERAL: return "ArrayLiteral";
        case EXPR_BRACE_LITERAL: return "BraceLiteral";
        case EXPR_INDEX: return "Index";
        case EXPR_INDEX_SET: return "IndexSet";
        default: return "Unknown";
    }
}

// Parser initialization
void initParser(Parser* parser, Lexer* lexer, const char* currentFilePath) {
    parser->lexer = lexer;
    parser->currentFilePath = currentFilePath;
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
    const char* file = parser->currentFilePath;
    int line = parser->current.line;
    int col = parser->current.col;
    if (file && line > 0 && col > 0) {
        fprintf(stderr, "%s:%d:%d: error: at '%.*s': %s\n", file, line, col, parser->current.length, parser->current.start, message);
    } else if (file && line > 0) {
        fprintf(stderr, "%s:%d: error: at '%.*s': %s\n", file, line, parser->current.length, parser->current.start, message);
    } else if (line > 0 && col > 0) {
        fprintf(stderr, "error:%d:%d: at '%.*s': %s\n", line, col, parser->current.length, parser->current.start, message);
    } else if (line > 0) {
        fprintf(stderr, "error:%d: at '%.*s': %s\n", line, parser->current.length, parser->current.start, message);
    } else {
        fprintf(stderr, "error: at '%.*s': %s\n", parser->current.length, parser->current.start, message);
    }
    parser->hadError = true;
}



static void errorPrint(Parser* parser, const char* message) {
    if (parser->panicMode) return;
    parser->panicMode = true;
    const char* file = parser->currentFilePath;
    int line = parser->current.line;
    int col = parser->current.col;
    if (file && line > 0 && col > 0) {
        fprintf(stderr, "%s:%d:%d: error: %s\n", file, line, col, message);
    } else if (file && line > 0) {
        fprintf(stderr, "%s:%d: error: %s\n", file, line, message);
    } else if (line > 0 && col > 0) {
        fprintf(stderr, "error:%d:%d: %s\n", line, col, message);
    } else if (line > 0) {
        fprintf(stderr, "error:%d: %s\n", line, message);
    } else {
        fprintf(stderr, "error: %s\n", message);
    }
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
    return (Token){TOKEN_ERROR, NULL, 0, 0, 0, 0};
}

static void synchronize(Parser* parser) {
    parser->panicMode = false;
    bool advancedOnce = false;
    while (parser->current.type != TOKEN_EOF) {
        if (advancedOnce && parser->previous.type == TOKEN_SEMICOLON) return;
        
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
                advancedOnce = true;
        }
    }
}

// Operator precedence
static int getOperatorPrecedence(TokenType type) {
    switch (type) {
        case TOKEN_ASSIGN: return 1;      // =
        case TOKEN_COALESCE: return 2;    // ??
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

static Stmt* newFuncStmt(Token name, List* params, Type* returnType, List* returnTypes, List* body) {
    FuncStmt* stmt = malloc(sizeof(FuncStmt));
    stmt->base.type = STMT_FUNC;
    stmt->name = name;
    stmt->params = params;
    stmt->returnType = returnType;
    stmt->returnTypes = returnTypes;
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
    expr->base.token = operator;
    expr->base.inferredType = TYPE_ANY;
    expr->operand = operand;
    expr->operator = operator;
    return (Expr*)expr;
}

static Expr* newBinaryExpr(Expr* left, Token operator, Expr* right) {
    BinaryExpr* expr = malloc(sizeof(BinaryExpr));
    expr->base.type = EXPR_BINARY;
    expr->base.token = operator;
    expr->base.inferredType = TYPE_ANY;
    expr->left = left;
    expr->operator = operator;
    expr->right = right;
    return (Expr*)expr;
}

static Expr* newCastExpr(Token asToken, Expr* value, Type* targetType, int isChecked) {
    CastExpr* expr = malloc(sizeof(CastExpr));
    expr->base.type = EXPR_CAST;
    expr->base.token = asToken;
    expr->base.inferredType = TYPE_ANY;
    expr->value = value;
    expr->targetType = targetType;
    expr->isChecked = isChecked;
    return (Expr*)expr;
}

static int isCastTypeToken(TokenType t) {
    switch (t) {
        case TOKEN_INT:
        case TOKEN_LONG:
        case TOKEN_I8:
        case TOKEN_I16:
        case TOKEN_ISIZE:
        case TOKEN_U8:
        case TOKEN_U16:
        case TOKEN_U32:
        case TOKEN_U64:
        case TOKEN_USIZE:
        case TOKEN_BYTE:
        case TOKEN_FLOAT:
        case TOKEN_DOUBLE:
        case TOKEN_F8:
        case TOKEN_F16:
        case TOKEN_F32:
        case TOKEN_F64:
        case TOKEN_BF8:
        case TOKEN_BF16:
        case TOKEN_BOOL:
        case TOKEN_STRING:
            return 1;
        default:
            return 0;
    }
}

static int isExprStartToken(TokenType t) {
    switch (t) {
        case TOKEN_INT:
        case TOKEN_LONG:
        case TOKEN_DOUBLE:
        case TOKEN_STRING_LITERAL:
        case TOKEN_NULL:
        case TOKEN_TRUE:
        case TOKEN_FALSE:
        case TOKEN_IDENTIFIER:
        case TOKEN_LPAREN:
        case TOKEN_LBRACE:
        case TOKEN_LBRACKET:
        case TOKEN_FUNC:
        case TOKEN_THIS:
        case TOKEN_INC:
        case TOKEN_DEC:
        case TOKEN_MINUS:
        case TOKEN_NOT:
        case TOKEN_AMP:
            return 1;
        default:
            return 0;
    }
}

static int isParenCastStart(Parser* parser) {
    if (!parser || !parser->lexer) return 0;
    if (!check(parser, TOKEN_LPAREN)) return 0;

    Lexer tmp = *parser->lexer;
    Token t1 = scanToken(&tmp);
    if (!isCastTypeToken(t1.type)) return 0;
    Token t2 = scanToken(&tmp);
    if (t2.type != TOKEN_RPAREN) return 0;
    Token t3 = scanToken(&tmp);
    return isExprStartToken(t3.type);
}

static Expr* newUnaryExpr(Token operator, Expr* right) {
    UnaryExpr* expr = malloc(sizeof(UnaryExpr));
    expr->base.type = EXPR_UNARY;
    expr->base.token = operator;
    expr->base.inferredType = TYPE_ANY;
    expr->operator = operator;
    expr->right = right;
    return (Expr*)expr;
}

static Expr* newLiteralExpr(Token value) {
    LiteralExpr* expr = malloc(sizeof(LiteralExpr));
    expr->base.type = EXPR_LITERAL;
    expr->base.token = value;
    expr->base.inferredType = TYPE_ANY;
    expr->value = value;
    return (Expr*)expr;
}

static Expr* newVariableExpr(Token name) {
    VariableExpr* expr = malloc(sizeof(VariableExpr));
    expr->base.type = EXPR_VARIABLE;
    expr->base.token = name;
    expr->base.inferredType = TYPE_ANY;
    expr->name = name;
    return (Expr*)expr;
}

static Expr* newGroupingExpr(Expr* expression) {
    GroupingExpr* expr = malloc(sizeof(GroupingExpr));
    expr->base.type = EXPR_GROUPING;
    if (expression) expr->base.token = expression->token;
    expr->base.inferredType = TYPE_ANY;
    expr->expression = expression;
    return (Expr*)expr;
}

static Expr* newAssignExpr(Token name, Expr* value) {
    AssignExpr* expr = malloc(sizeof(AssignExpr));
    expr->base.type = EXPR_ASSIGN;
    expr->base.token = name;
    expr->base.inferredType = TYPE_ANY;
    expr->name = name;
    expr->value = value;
    return (Expr*)expr;
}

static Expr* newGetExpr(Expr* object, Token name) {
    GetExpr* expr = malloc(sizeof(GetExpr));
    expr->base.type = EXPR_GET;
    expr->base.token = name;
    expr->base.inferredType = TYPE_ANY;
    expr->object = object;
    expr->name = name;
    return (Expr*)expr;
}

static Expr* newSetExpr(Expr* object, Token name, Expr* value) {
    SetExpr* expr = malloc(sizeof(SetExpr));
    expr->base.type = EXPR_SET;
    expr->base.token = name;
    expr->base.inferredType = TYPE_ANY;
    expr->object = object;
    expr->name = name;
    expr->value = value;
    return (Expr*)expr;
}

static Expr* newMapLiteralExpr(Token lbrace, List* entries) {
    MapLiteralExpr* expr = malloc(sizeof(MapLiteralExpr));
    expr->base.type = EXPR_MAP_LITERAL;
    expr->base.token = lbrace;
    expr->base.inferredType = TYPE_ANY;
    expr->entries = entries;
    return (Expr*)expr;
}

static Expr* newArrayLiteralExpr(Token lbracket, List* elements) {
    ArrayLiteralExpr* expr = malloc(sizeof(ArrayLiteralExpr));
    expr->base.type = EXPR_ARRAY_LITERAL;
    expr->base.token = lbracket;
    expr->base.inferredType = TYPE_ANY;
    expr->elements = elements;
    return (Expr*)expr;
}

static Expr* newBraceLiteralExpr(Token lbrace) {
    BraceLiteralExpr* expr = malloc(sizeof(BraceLiteralExpr));
    expr->base.type = EXPR_BRACE_LITERAL;
    expr->base.token = lbrace;
    expr->base.inferredType = TYPE_ANY;
    expr->lbrace = lbrace;
    return (Expr*)expr;
}

static Expr* newIndexExpr(Expr* object, Expr* index) {
    IndexExpr* expr = malloc(sizeof(IndexExpr));
    expr->base.type = EXPR_INDEX;
    if (object) expr->base.token = object->token;
    else if (index) expr->base.token = index->token;
    expr->base.inferredType = TYPE_ANY;
    expr->object = object;
    expr->index = index;
    return (Expr*)expr;
}

static Expr* newIndexSetExpr(Expr* object, Expr* index, Expr* value) {
    IndexSetExpr* expr = malloc(sizeof(IndexSetExpr));
    expr->base.type = EXPR_INDEX_SET;
    if (object) expr->base.token = object->token;
    else if (index) expr->base.token = index->token;
    expr->base.inferredType = TYPE_ANY;
    expr->object = object;
    expr->index = index;
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
        if (expr->type == EXPR_GET) {
            GetExpr* get = (GetExpr*)expr;
            return newSetExpr(get->object, get->name, value);
        }
        if (expr->type == EXPR_INDEX) {
            IndexExpr* idx = (IndexExpr*)expr;
            return newIndexSetExpr(idx->object, idx->index, value);
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
        if (check(parser, TOKEN_DOT)) {
            advance(parser); // consume '.'
            Token member = consume(parser, TOKEN_IDENTIFIER, "Expect member name after '.'");
            left = newGetExpr(left, member);
            if (match(parser, TOKEN_LPAREN)) {
                left = finishCall(parser, left);
            }
            continue;
        }
        // Call chaining: `callee(args...)(args...)`
        if (match(parser, TOKEN_LPAREN)) {
            left = finishCall(parser, left);
            continue;
        }
        // Checked cast: `expr as T` (returns Option<T>)
        if (match(parser, TOKEN_AS)) {
            Token asTok = parser->previous;
            if (match(parser, TOKEN_QMARK)) {
                errorAtCurrent(parser, "`as?` is deprecated; use `(T)expr` for unchecked cast and `expr as T` for checked cast");
                return NULL;
            }
            Type* target = parseType(parser);
            left = newCastExpr(asTok, left, target, 1);
            continue;
        }
        // Indexing: `obj[expr]`
        if (match(parser, TOKEN_LBRACKET)) {
            Expr* index = parseExpression(parser);
            consume(parser, TOKEN_RBRACKET, "Expect ']' after index expression");
            left = newIndexExpr(left, index);
            continue;
        }
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
        if ((op == TOKEN_INC || op == TOKEN_DEC) && prec >= minPrec) {
            advance(parser);
            left = newPostfixExpr(left, parser->previous);
            parserDebug("parseBinaryExpr: inc||dec left type:%d\n", left->type);
            continue;
        }
        
        Token operator = parser->current;
        advance(parser);
        parserDebug("parseBinaryExpr: parse right expr start, op type:%d\n", operator.type);
        // Right-associative for `??` so `a ?? b ?? c` parses as `a ?? (b ?? c)`.
        Expr* right = parseBinaryExpr(parser, operator.type == TOKEN_COALESCE ? prec : (prec + 1));
        left = newBinaryExpr(left, operator, right);
        TokenType op2 = parser->current.type;
        parserDebug("parseBinaryExpr last: left type:%d, operator:%.*s/%s right type:%d, parser current token:%s/%d\n", left->type, operator.length, operator.start, tokenToString(operator.type), right->type, tokenToString(op2),op2);
    }
    parserDebugEnd("parseBinaryExpr");
    return left;
}

static Expr* parseUnaryExpr(Parser* parser) {
    parserDebugStart("parseUnaryExpr");

    // Java-style cast: `(T)expr`
    if (isParenCastStart(parser)) {
        Token lparen = consume(parser, TOKEN_LPAREN, "Expect '(' for cast");
        Type* target = parseType(parser);
        consume(parser, TOKEN_RPAREN, "Expect ')' after cast type");
        Expr* rhs = parseUnaryExpr(parser);
        parserDebugEnd("parseUnaryExpr");
        return newCastExpr(lparen, rhs, target, 0);
    }
    
    if (match(parser, TOKEN_INC) || 
        match(parser, TOKEN_DEC) ||
        match(parser, TOKEN_MINUS) || 
        match(parser, TOKEN_NOT) ||
        match(parser, TOKEN_AMP)) {
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
        match(parser, TOKEN_NULL) ||
        match(parser, TOKEN_TRUE) ||
        match(parser, TOKEN_FALSE)) {
        expr = newLiteralExpr(parser->previous);
    } else if (match(parser, TOKEN_LBRACE)) {
        // Brace literal:
        // - Map:   { <constKey> : <expr> (, ...)? }
        // - Array: { <expr> (, ...)? }
        // Empty `{}` is ambiguous; keep as EXPR_BRACE_LITERAL and resolve later (default to map).
        Token lbrace = parser->previous;
        while (match(parser, TOKEN_SEMICOLON)) {}

        if (check(parser, TOKEN_RBRACE)) {
            consume(parser, TOKEN_RBRACE, "Expect '}' after brace literal");
            expr = newBraceLiteralExpr(lbrace);
        } else {
            // Parse first expression, then decide map vs array based on ':'.
            // This avoids committing early for cases like `{1 + 2}`.
            Expr* first = parseExpression(parser);
            if (match(parser, TOKEN_COLON)) {
                // Map literal.
                if (!first || first->type != EXPR_LITERAL) {
                    printError(parser, "Map key must be a constant literal (int/long/string)");
                    return NULL;
                }
                LiteralExpr* lit = (LiteralExpr*)first;
                Token keyTok = lit->value;
                if (!(keyTok.type == TOKEN_INT || keyTok.type == TOKEN_LONG || keyTok.type == TOKEN_STRING_LITERAL)) {
                    printError(parser, "Map key must be a constant literal (int/long/string)");
                    return NULL;
                }

                List* entries = listNew();
                Expr* value = parseExpression(parser);
                MapEntry* e = malloc(sizeof(MapEntry));
                e->key = keyTok;
                e->value = value;
                listAppend(entries, e);

                while (match(parser, TOKEN_COMMA)) {
                    while (match(parser, TOKEN_SEMICOLON)) {}
                    if (check(parser, TOKEN_RBRACE)) break; // allow trailing comma

                    Token key = (Token){0};
                    if (match(parser, TOKEN_INT) || match(parser, TOKEN_LONG) || match(parser, TOKEN_STRING_LITERAL)) {
                        key = parser->previous;
                    } else {
                        printError(parser, "Map key must be a constant literal (int/long/string)");
                        return NULL;
                    }
                    consume(parser, TOKEN_COLON, "Expect ':' after map key");
                    Expr* v = parseExpression(parser);
                    MapEntry* me = malloc(sizeof(MapEntry));
                    me->key = key;
                    me->value = v;
                    listAppend(entries, me);
                }
                consume(parser, TOKEN_RBRACE, "Expect '}' after map literal");
                expr = newMapLiteralExpr(lbrace, entries);
            } else {
                // Array literal.
                List* elements = listNew();
                listAppend(elements, first);
                while (match(parser, TOKEN_COMMA)) {
                    while (match(parser, TOKEN_SEMICOLON)) {}
                    if (check(parser, TOKEN_RBRACE)) break; // allow trailing comma
                    Expr* e = parseExpression(parser);
                    listAppend(elements, e);
                }
                consume(parser, TOKEN_RBRACE, "Expect '}' after array literal");
                expr = newArrayLiteralExpr(lbrace, elements);
            }
        }
    } else if (match(parser, TOKEN_LBRACKET)) {
        // Array literal: [] or [e1, e2, ...]
        Token lbracket = parser->previous;
        List* elements = listNew();
        while (match(parser, TOKEN_SEMICOLON)) {}
        if (!check(parser, TOKEN_RBRACKET)) {
            while (true) {
                Expr* e = parseExpression(parser);
                listAppend(elements, e);
                if (match(parser, TOKEN_COMMA)) {
                    while (match(parser, TOKEN_SEMICOLON)) {}
                    if (check(parser, TOKEN_RBRACKET)) break; // allow trailing comma
                    continue;
                }
                break;
            }
        }
        consume(parser, TOKEN_RBRACKET, "Expect ']' after array literal");
        expr = newArrayLiteralExpr(lbracket, elements);
    } else if (match(parser, TOKEN_THIS)) {
        expr = newVariableExpr(parser->previous);
    } else if (match(parser, TOKEN_IDENTIFIER)) {
        parserDebug("parsePrimaryExpr: current code:[%.*s],[%s]\n", parser->current.length,parser->current.start, tokenToString(parser->previous.type));
        expr = newVariableExpr(parser->previous);
        
        // Function call: identifier followed by '('
        if (match(parser, TOKEN_LPAREN)) {

            expr = finishCall(parser, expr);
        }

        // Member access / chained calls: A.B or A.B(...)
        while (match(parser, TOKEN_DOT)) {
            Token member = consume(parser, TOKEN_IDENTIFIER, "Expect member name after '.'");
            expr = newGetExpr(expr, member);
            if (match(parser, TOKEN_LPAREN)) {
                expr = finishCall(parser, expr);
            }
        }
    } else if (match(parser, TOKEN_LPAREN)) {
        expr = parseExpression(parser);
        consume(parser, TOKEN_RPAREN, "Expect ')' after expression");
        expr = newGroupingExpr(expr);
    } else if (match(parser, TOKEN_FUNC)) {
        // Lambda expression: fn (params...) -> T[,U...] { ... }
        Token keyword = parser->previous;
        consume(parser, TOKEN_LPAREN, "Expect '(' after 'fn' in lambda expression");

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
        List* returnTypes = NULL;

        // Support both:
        // - `fn(...) -> int {}` (preferred)
        // - `fn(...) int {}` (sugar)
        if (match(parser, TOKEN_ARROW) ||
            check(parser, TOKEN_INT) ||
            check(parser, TOKEN_LONG) ||
            check(parser, TOKEN_DOUBLE) ||
            check(parser, TOKEN_FLOAT) ||
            check(parser, TOKEN_I8) ||
            check(parser, TOKEN_I16) ||
            check(parser, TOKEN_ISIZE) ||
            check(parser, TOKEN_U8) ||
            check(parser, TOKEN_U16) ||
            check(parser, TOKEN_U32) ||
            check(parser, TOKEN_U64) ||
            check(parser, TOKEN_USIZE) ||
            check(parser, TOKEN_BYTE) ||
            check(parser, TOKEN_F8) ||
            check(parser, TOKEN_F16) ||
            check(parser, TOKEN_F32) ||
            check(parser, TOKEN_F64) ||
            check(parser, TOKEN_BF8) ||
            check(parser, TOKEN_BF16) ||
            check(parser, TOKEN_STRING) ||
            check(parser, TOKEN_BOOL) ||
            check(parser, TOKEN_IDENTIFIER) ||
            check(parser, TOKEN_AMP)) {
            returnTypes = listNew();
            returnType = parseType(parser);
            listAppend(returnTypes, returnType);
            while (match(parser, TOKEN_COMMA)) {
                Type* t = parseType(parser);
                listAppend(returnTypes, t);
            }
        }

        List* body = parseBlock(parser);

        LambdaExpr* lam = malloc(sizeof(LambdaExpr));
        lam->base.type = EXPR_LAMBDA;
        lam->base.token = keyword;
        lam->base.inferredType = TYPE_ANY;
        lam->keyword = keyword;
        lam->params = parameters;
        lam->returnType = returnType;
        lam->returnTypes = returnTypes;
        lam->body = body;
        expr = (Expr*)lam;

        // Allow immediate call: (fn(...) {...})(args)
        if (match(parser, TOKEN_LPAREN)) {
            expr = finishCall(parser, expr);
        }
    } else if (match(parser, TOKEN_PRINTLN) || match(parser, TOKEN_PRINT)){
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
    expr->base.token = parser->previous;
    expr->base.inferredType = TYPE_ANY;
    expr->callee = callee;
    expr->arguments = arguments;
    parserDebugEnd("finishCall");
    return (Expr*)expr;
}

static Stmt* parseStatement(Parser* parser) {
    parserDebugStart("parseStatement");
    Stmt* stmt = NULL;

    while (match(parser, TOKEN_SEMICOLON)) {
        // skip statement separators (explicit ';' or newline)
    }

    // Lua-style label: ::name::
    if (match(parser, TOKEN_COLON)) {
        consume(parser, TOKEN_COLON, "Expect ':' in label syntax '::name::'");
        Token name = consume(parser, TOKEN_IDENTIFIER, "Expect label name");
        consume(parser, TOKEN_COLON, "Expect ':' after label name");
        consume(parser, TOKEN_COLON, "Expect ':' after label name");
        LabelStmt* l = malloc(sizeof(LabelStmt));
        l->base.type = STMT_LABEL;
        l->name = name;
        parserDebugEnd("parseStatement");
        return (Stmt*)l;
    }
    
    if (match(parser, TOKEN_IF)) {
        stmt = parseIfStatement(parser);
    } else if (match(parser, TOKEN_FOR)) {
        stmt = parseForStatement(parser);
    } else if (match(parser, TOKEN_WHILE)) {
        bool hasParen = false;
        if (match(parser, TOKEN_LPAREN)) hasParen = true;
        Expr* condition = parseExpression(parser);
        if (hasParen) consume(parser, TOKEN_RPAREN, "Expect ')' after condition");
        Stmt* body = parseStatement(parser);

        WhileStmt* w = malloc(sizeof(WhileStmt));
        w->base.type = STMT_WHILE;
        w->condition = condition;
        w->body = body;
        stmt = (Stmt*)w;
    } else if (match(parser, TOKEN_DO)) {
        // do { ... } while cond
        while (match(parser, TOKEN_SEMICOLON)) {}
        Stmt* body = parseStatement(parser);
        while (match(parser, TOKEN_SEMICOLON)) {}
        consume(parser, TOKEN_WHILE, "Expect 'while' after 'do' body");
        bool hasParen = false;
        if (match(parser, TOKEN_LPAREN)) hasParen = true;
        Expr* condition = parseExpression(parser);
        if (hasParen) consume(parser, TOKEN_RPAREN, "Expect ')' after condition");
        if (check(parser, TOKEN_SEMICOLON)) consume(parser, TOKEN_SEMICOLON, "Expect statement separator after do-while");

        DoWhileStmt* dw = malloc(sizeof(DoWhileStmt));
        dw->base.type = STMT_DO_WHILE;
        dw->condition = condition;
        dw->body = body;
        stmt = (Stmt*)dw;
    } else if (match(parser, TOKEN_BREAK)) {
        BreakStmt* b = malloc(sizeof(BreakStmt));
        b->base.type = STMT_BREAK;
        b->keyword = parser->previous;
        if (check(parser, TOKEN_SEMICOLON)) consume(parser, TOKEN_SEMICOLON, "Expect statement separator after break");
        stmt = (Stmt*)b;
    } else if (match(parser, TOKEN_CONTINUE)) {
        ContinueStmt* c = malloc(sizeof(ContinueStmt));
        c->base.type = STMT_CONTINUE;
        c->keyword = parser->previous;
        if (check(parser, TOKEN_SEMICOLON)) consume(parser, TOKEN_SEMICOLON, "Expect statement separator after continue");
        stmt = (Stmt*)c;
    } else if (match(parser, TOKEN_GOTO)) {
        Token keyword = parser->previous;
        Token name = consume(parser, TOKEN_IDENTIFIER, "Expect label name after 'goto'");
        if (check(parser, TOKEN_SEMICOLON)) consume(parser, TOKEN_SEMICOLON, "Expect statement separator after goto");
        GotoStmt* g = malloc(sizeof(GotoStmt));
        g->base.type = STMT_GOTO;
        g->keyword = keyword;
        g->name = name;
        stmt = (Stmt*)g;
    } else if (match(parser, TOKEN_RETURN)) {
        stmt = parseReturnStatement(parser);
    } else if (match(parser, TOKEN_LBRACE)) {
        stmt = parseBlockStatement(parser);
    } else {
        // Try destructuring assignment: `a,b = expr`
        if (check(parser, TOKEN_IDENTIFIER)) {
            Parser snap = *parser;
            Lexer lexSnap = *parser->lexer;

            advance(parser); // consume first identifier
            Token firstName = parser->previous;

            if (match(parser, TOKEN_COMMA)) {
                List* names = listNew();
                List* types = listNew();

                Token* t0 = malloc(sizeof(Token));
                *t0 = firstName;
                listAppend(names, t0);
                listAppend(types, NULL);

                do {
                    Token name = consume(parser, TOKEN_IDENTIFIER, "Expect identifier in destructuring assignment");
                    Token* tp = malloc(sizeof(Token));
                    *tp = name;
                    listAppend(names, tp);
                    listAppend(types, NULL);
                } while (match(parser, TOKEN_COMMA));

                consume(parser, TOKEN_ASSIGN, "Expect '=' after destructuring targets");
                Expr* value = parseExpression(parser);
                if (check(parser, TOKEN_SEMICOLON)) consume(parser, TOKEN_SEMICOLON, "Expect statement separator after destructuring assignment");

                DestructureStmt* ds = malloc(sizeof(DestructureStmt));
                ds->base.type = STMT_DESTRUCTURE;
                ds->keyword = (Token){TOKEN_ERROR, NULL, 0, firstName.line, firstName.col, 0};
                ds->names = names;
                ds->types = types;
                ds->value = value;
                ds->isConst = false;
                ds->isDeclaration = false;
                parserDebugEnd("parseStatement");
                return (Stmt*)ds;
            }

            // Not destructuring assignment: restore and parse as expression statement.
            *parser = snap;
            *parser->lexer = lexSnap;
        }
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
    
    while (match(parser, TOKEN_SEMICOLON)) {
        // allow newline(s) before else / else-if
    }
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
        
        Token valueVar = (Token){0};
        int hasValueVar = 0;
        if (match(parser, TOKEN_COMMA)) {
            valueVar = consume(parser, TOKEN_IDENTIFIER, "Expect identifier after ',' in for-in loop");
            hasValueVar = 1;
        }

        if (check(parser, TOKEN_IN)) {
            // Case: for i in range(1,10) {}
            advance(parser); // consume 'in'
            Expr* rangeExpr = parseExpression(parser);
            if (hasParen) {
                consume(parser, TOKEN_RPAREN, "Expect ')' after range");
            }
            while (match(parser, TOKEN_SEMICOLON)) {
                // allow newline before '{'
            }
            consume(parser, TOKEN_LBRACE, "Expect '{' before loop body");
            Stmt* body = parseBlockStatement(parser);
            
            ForInStmt* stmt = malloc(sizeof(ForInStmt));
            stmt->base.type = STMT_FOR_IN;
            stmt->loopVar = current;
            stmt->valueVar = valueVar;
            stmt->hasValueVar = hasValueVar;
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
    
    while (match(parser, TOKEN_SEMICOLON)) {
        // allow newline before '{'
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
        Stmt* stmt = declaration(parser);
        if (stmt != NULL) {
            listAppend(statements, stmt);
        }
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
    List* values = NULL;
    if (!check(parser, TOKEN_SEMICOLON)) {
        values = listNew();
        value = parseExpression(parser);
        listAppend(values, value);
        while (match(parser, TOKEN_COMMA)) {
            Expr* v = parseExpression(parser);
            listAppend(values, v);
        }
    }

    if (check(parser, TOKEN_SEMICOLON)) {
        consume(parser, TOKEN_SEMICOLON, "Expect statement separator after return");
    }

    ReturnStmt* stmt = malloc(sizeof(ReturnStmt));
    stmt->base.type = STMT_RETURN;
    stmt->keyword = keyword;
    stmt->value = value;
    stmt->values = values;
    return (Stmt*)stmt;
}

static Stmt* parseExpressionStatement(Parser* parser) {
    parserDebugStart("parseExpressionStatement");
    Expr* expr = parseExpression(parser);
    if (!expr) {
        parserDebugEnd("parseExpressionStatement");
        return NULL;
    }
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

    Token declKeyword = parser->previous;
    bool isConst = (!identifierConsumed) && (declKeyword.type == TOKEN_CONST);

    // Identifier-consumed form: `name: Type = expr`
    if (identifierConsumed) {
        Token name = parser->previous;
        Type* type = NULL;
        if (match(parser, TOKEN_COLON)) {
            type = parseType(parser);
        }
        Expr* initializer = NULL;
        if (match(parser, TOKEN_ASSIGN)) {
            initializer = parseExpression(parser);
        }
        if (check(parser, TOKEN_SEMICOLON)) {
            consume(parser, TOKEN_SEMICOLON, "Expect statement separator after variable declaration");
        }
        parserDebugEnd("parseVarDeclaration");
        return newVarStmt(name, type, initializer, 0);
    }

    // Normal `let/const` form.
    Token firstName = consume(parser, TOKEN_IDENTIFIER, "Expect variable name");
    Type* firstType = NULL;
    if (match(parser, TOKEN_COLON)) {
        firstType = parseType(parser);
    }

    // If we have `let a,b,...` and no per-binding initializer, treat as:
    // - `let a,b = expr` destructuring declaration, OR
    // - `let a,b` multiple declarations without initializers.
    if (check(parser, TOKEN_COMMA)) {
        List* names = listNew();
        List* types = listNew();

        Token* t0 = malloc(sizeof(Token));
        *t0 = firstName;
        listAppend(names, t0);
        listAppend(types, firstType);

        while (match(parser, TOKEN_COMMA)) {
            Token name = consume(parser, TOKEN_IDENTIFIER, "Expect variable name");
            Type* type = NULL;
            if (match(parser, TOKEN_COLON)) {
                type = parseType(parser);
            }
            Token* tp = malloc(sizeof(Token));
            *tp = name;
            listAppend(names, tp);
            listAppend(types, type);
        }

        if (match(parser, TOKEN_ASSIGN)) {
            Expr* rhs = parseExpression(parser);
            if (check(parser, TOKEN_SEMICOLON)) {
                consume(parser, TOKEN_SEMICOLON, "Expect statement separator after destructuring declaration");
            }

            DestructureStmt* ds = malloc(sizeof(DestructureStmt));
            ds->base.type = STMT_DESTRUCTURE;
            ds->keyword = declKeyword;
            ds->names = names;
            ds->types = types;
            ds->value = rhs;
            ds->isConst = isConst;
            ds->isDeclaration = true;
            parserDebugEnd("parseVarDeclaration");
            return (Stmt*)ds;
        }

        // No `=`: treat as multiple declarations without initializers.
        if (check(parser, TOKEN_SEMICOLON)) {
            consume(parser, TOKEN_SEMICOLON, "Expect statement separator after variable declaration");
        }
        List* decls = listNew();
        for (ListNode* n = names->head, *t = types->head; n != NULL && t != NULL; n = n->next, t = t->next) {
            Token* nt = (Token*)n->data;
            Type* ty = (Type*)t->data;
            listAppend(decls, newVarStmt(*nt, ty, NULL, isConst));
        }
        BlockStmt* block = malloc(sizeof(BlockStmt));
        block->base.type = STMT_BLOCK;
        block->statements = decls;
        parserDebugEnd("parseVarDeclaration");
        return (Stmt*)block;
    }

    // Per-binding initializer form: `let a[:T] = expr, b[:U] = expr ...`
    List* declarations = listNew();
    Expr* firstInit = NULL;
    if (match(parser, TOKEN_ASSIGN)) {
        firstInit = parseExpression(parser);
    }
    listAppend(declarations, newVarStmt(firstName, firstType, firstInit, isConst));

    while (match(parser, TOKEN_COMMA)) {
        Token name = consume(parser, TOKEN_IDENTIFIER, "Expect variable name");
        Type* type = NULL;
        if (match(parser, TOKEN_COLON)) {
            type = parseType(parser);
        }
        Expr* init = NULL;
        if (match(parser, TOKEN_ASSIGN)) {
            init = parseExpression(parser);
        }
        listAppend(declarations, newVarStmt(name, type, init, isConst));
    }

    if (check(parser, TOKEN_SEMICOLON)) {
        consume(parser, TOKEN_SEMICOLON, "Expect statement separator after variable declaration");
    }

    if (declarations->length == 1) {
        VarStmt* stmt = (VarStmt*)declarations->head->data;
        free(declarations);
        parserDebugEnd("parseVarDeclaration");
        return (Stmt*)stmt;
    }

    BlockStmt* block = malloc(sizeof(BlockStmt));
    block->base.type = STMT_BLOCK;
    block->statements = declarations;
    parserDebugEnd("parseVarDeclaration");
    return (Stmt*)block;
}
// Block parsing
static List* parseBlock(Parser* parser) {
    List* statements = listNew();
    
    while (match(parser, TOKEN_SEMICOLON)) {
        // allow newline before '{'
    }
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
    List* returnTypes = NULL;
    if (match(parser, TOKEN_ARROW)) {
        returnTypes = listNew();
        returnType = parseType(parser);
        listAppend(returnTypes, returnType);
        while (match(parser, TOKEN_COMMA)) {
            Type* t = parseType(parser);
            listAppend(returnTypes, t);
        }
    } else if (check(parser, TOKEN_INT) ||
               check(parser, TOKEN_LONG) ||
               check(parser, TOKEN_DOUBLE) ||
               check(parser, TOKEN_FLOAT) ||
               check(parser, TOKEN_STRING) ||
               check(parser, TOKEN_BOOL)) {
        // Support `fn demo(...) int {}` in addition to `fn demo(...) -> int {}`
        returnTypes = listNew();
        returnType = parseType(parser);
        listAppend(returnTypes, returnType);
        while (match(parser, TOKEN_COMMA)) {
            Type* t = parseType(parser);
            listAppend(returnTypes, t);
        }
    }
    
    List* body = parseBlock(parser);
    
    return newFuncStmt(name, parameters, returnType, returnTypes, body);
}

static Stmt* parseExternFunctionDeclaration(Parser* parser) {
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
    List* returnTypes = NULL;
    if (match(parser, TOKEN_ARROW)) {
        returnTypes = listNew();
        returnType = parseType(parser);
        listAppend(returnTypes, returnType);
        while (match(parser, TOKEN_COMMA)) {
            Type* t = parseType(parser);
            listAppend(returnTypes, t);
        }
    } else if (check(parser, TOKEN_INT) ||
               check(parser, TOKEN_LONG) ||
               check(parser, TOKEN_DOUBLE) ||
               check(parser, TOKEN_FLOAT) ||
               check(parser, TOKEN_STRING) ||
               check(parser, TOKEN_BOOL) ||
               check(parser, TOKEN_IDENTIFIER) ||
               check(parser, TOKEN_AMP) ||
               check(parser, TOKEN_LPAREN)) {
        // Support `extern fn f(...) int` as sugar for `extern fn f(...) -> int`.
        returnTypes = listNew();
        returnType = parseType(parser);
        listAppend(returnTypes, returnType);
        while (match(parser, TOKEN_COMMA)) {
            Type* t = parseType(parser);
            listAppend(returnTypes, t);
        }
    }

    if (check(parser, TOKEN_SEMICOLON)) {
        consume(parser, TOKEN_SEMICOLON, "Expect statement separator after extern fn declaration");
    }

    return newFuncStmt(name, parameters, returnType, returnTypes, NULL);
}

static Stmt* parseStructDeclaration(Parser* parser) {
    parserDebugStart("parseStructDeclaration");
    
    // Parse struct name
    Token name = consume(parser, TOKEN_IDENTIFIER, "Expect struct name");
    
    // Parse opening brace
    while (match(parser, TOKEN_SEMICOLON)) {
        // allow newline before '{'
    }
    consume(parser, TOKEN_LBRACE, "Expect '{' before struct body");
    
    // Create lists for fields and methods
    List* fields = listNew();
    List* methods = listNew();
    
    // Parse fields and methods
    while (!check(parser, TOKEN_RBRACE) && !check(parser, TOKEN_EOF)) {
        while (match(parser, TOKEN_SEMICOLON)) {
            // skip separators/newlines
        }
        if (check(parser, TOKEN_RBRACE) || check(parser, TOKEN_EOF)) break;

        if (match(parser, TOKEN_PRIVATE)) {
            // ignore visibility for now
            consume(parser, TOKEN_FUNC, "Expect 'fn' after 'private'");
            FuncStmt* method = (FuncStmt*)parseFunctionDeclaration(parser);
            listAppend(methods, method);
            continue;
        }

        if (match(parser, TOKEN_FUNC)) {
            FuncStmt* method = (FuncStmt*)parseFunctionDeclaration(parser);
            listAppend(methods, method);
            continue;
        }

        if (match(parser, TOKEN_INIT) || match(parser, TOKEN_DEINIT)) {
            Token nameToken = parser->previous;
            consume(parser, TOKEN_LPAREN, "Expect '(' after init/deinit");

            // init/deinit currently don't take parameters in README examples
            consume(parser, TOKEN_RPAREN, "Expect ')' after init/deinit");

            Type* returnType = NULL;
            List* returnTypes = NULL;
            if (match(parser, TOKEN_ARROW)) {
                returnTypes = listNew();
                returnType = parseType(parser);
                listAppend(returnTypes, returnType);
                while (match(parser, TOKEN_COMMA)) {
                    Type* t = parseType(parser);
                    listAppend(returnTypes, t);
                }
            }

            List* body = parseBlock(parser);
            FuncStmt* method = (FuncStmt*)newFuncStmt(nameToken, listNew(), returnType, returnTypes, body);
            listAppend(methods, method);
            continue;
        }

        // Field: optional `const`, then `name: Type`, optional `= expr`
        bool isConst = false;
        if (match(parser, TOKEN_CONST)) {
            isConst = true;
        }

        Token fieldName = consume(parser, TOKEN_IDENTIFIER, "Expect field name");
        consume(parser, TOKEN_COLON, "Expect ':' after field name");
        Type* fieldType = parseType(parser);

        Expr* initializer = NULL;
        if (match(parser, TOKEN_ASSIGN)) {
            initializer = parseExpression(parser);
        }

        if (check(parser, TOKEN_COMMA)) {
            consume(parser, TOKEN_COMMA, "Expect ',' after field");
        }

        FieldDeclaration* field = malloc(sizeof(FieldDeclaration));
        field->name = fieldName;
        field->type = fieldType;
        field->initializer = initializer;
        field->isConst = isConst;
        listAppend(fields, field);
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

static Stmt* parseImplDeclaration(Parser* parser) {
    parserDebugStart("parseImplDeclaration");

    Token name = consume(parser, TOKEN_IDENTIFIER, "Expect struct name after 'impl'");
    while (match(parser, TOKEN_SEMICOLON)) {
        // allow newline before '{'
    }
    consume(parser, TOKEN_LBRACE, "Expect '{' before impl body");

    List* methods = listNew();
    while (!check(parser, TOKEN_RBRACE) && !check(parser, TOKEN_EOF)) {
        while (match(parser, TOKEN_SEMICOLON)) {}
        if (check(parser, TOKEN_RBRACE) || check(parser, TOKEN_EOF)) break;

        if (match(parser, TOKEN_PRIVATE)) {
            // ignore visibility for now
            consume(parser, TOKEN_FUNC, "Expect 'fn' after 'private'");
            FuncStmt* method = (FuncStmt*)parseFunctionDeclaration(parser);
            listAppend(methods, method);
            continue;
        }

        if (match(parser, TOKEN_FUNC)) {
            FuncStmt* method = (FuncStmt*)parseFunctionDeclaration(parser);
            listAppend(methods, method);
            continue;
        }

        if (match(parser, TOKEN_INIT) || match(parser, TOKEN_DEINIT)) {
            Token nameToken = parser->previous;
            consume(parser, TOKEN_LPAREN, "Expect '(' after init/deinit");
            consume(parser, TOKEN_RPAREN, "Expect ')' after init/deinit");

            Type* returnType = NULL;
            List* returnTypes = NULL;
            if (match(parser, TOKEN_ARROW)) {
                returnTypes = listNew();
                returnType = parseType(parser);
                listAppend(returnTypes, returnType);
                while (match(parser, TOKEN_COMMA)) {
                    Type* t = parseType(parser);
                    listAppend(returnTypes, t);
                }
            }

            List* body = parseBlock(parser);
            FuncStmt* method = (FuncStmt*)newFuncStmt(nameToken, listNew(), returnType, returnTypes, body);
            listAppend(methods, method);
            continue;
        }

        errorAtCurrent(parser, "Expect method declaration in impl block");
        break;
    }

    consume(parser, TOKEN_RBRACE, "Expect '}' after impl body");

    ImplStmt* stmt = malloc(sizeof(ImplStmt));
    stmt->base.type = STMT_IMPL;
    stmt->name = name;
    stmt->methods = methods;

    parserDebugEnd("parseImplDeclaration");
    return (Stmt*)stmt;
}

static Type* parseType(Parser* parser) {
    // Function type: `(T1, T2, ...) -> R` or `(T1, ...) -> (R1, R2, ...)`
    // Note: standalone tuple types like `(int, string)` are not a general type form yet.
    Type* base = NULL;
    if (match(parser, TOKEN_LPAREN)) {
        List* paramTypes = listNew();
        if (!check(parser, TOKEN_RPAREN)) {
            do {
                Type* t = parseType(parser);
                listAppend(paramTypes, t);
            } while (match(parser, TOKEN_COMMA));
        }
        consume(parser, TOKEN_RPAREN, "Expect ')' after function type parameters");
        consume(parser, TOKEN_ARROW, "Expect '->' after function type parameters");

        List* returnTypes = listNew();
        if (match(parser, TOKEN_LPAREN)) {
            if (!check(parser, TOKEN_RPAREN)) {
                do {
                    Type* t = parseType(parser);
                    listAppend(returnTypes, t);
                } while (match(parser, TOKEN_COMMA));
            }
            consume(parser, TOKEN_RPAREN, "Expect ')' after function type return types");
        } else {
            Type* t = parseType(parser);
            listAppend(returnTypes, t);
        }

        Type* type = malloc(sizeof(Type));
        type->kind = TYPE_FUNC;
        type->name = (Token){0};
        type->inner = NULL;
        type->typeArgs = NULL;
        type->paramTypes = paramTypes;
        type->returnTypes = returnTypes;
        type->arrayLen = 0;
        base = type;
    } else if (match(parser, TOKEN_AMP)) {
        Type* inner = parseType(parser);
        Type* type = malloc(sizeof(Type));
        type->kind = TYPE_REF;
        type->name = (Token){0};
        type->inner = inner;
        type->typeArgs = NULL;
        type->paramTypes = NULL;
        type->returnTypes = NULL;
        type->arrayLen = 0;
        base = type;
    } else if (match(parser, TOKEN_INT)) {
        Type* type = malloc(sizeof(Type));
        type->kind = TYPE_INT;
        type->name = (Token){0};
        type->inner = NULL;
        type->typeArgs = NULL;
        type->paramTypes = NULL;
        type->returnTypes = NULL;
        type->arrayLen = 0;
        base = type;
    } else if (match(parser, TOKEN_I8)) {
        Type* type = malloc(sizeof(Type));
        type->kind = TYPE_I8;
        type->name = (Token){0};
        type->inner = NULL;
        type->typeArgs = NULL;
        type->paramTypes = NULL;
        type->returnTypes = NULL;
        type->arrayLen = 0;
        base = type;
    } else if (match(parser, TOKEN_I16)) {
        Type* type = malloc(sizeof(Type));
        type->kind = TYPE_I16;
        type->name = (Token){0};
        type->inner = NULL;
        type->typeArgs = NULL;
        type->paramTypes = NULL;
        type->returnTypes = NULL;
        type->arrayLen = 0;
        base = type;
    } else if (match(parser, TOKEN_LONG)) {
        Type* type = malloc(sizeof(Type));
        type->kind = TYPE_LONG;
        type->name = (Token){0};
        type->inner = NULL;
        type->typeArgs = NULL;
        type->paramTypes = NULL;
        type->returnTypes = NULL;
        type->arrayLen = 0;
        base = type;
    } else if (match(parser, TOKEN_ISIZE)) {
        Type* type = malloc(sizeof(Type));
        type->kind = TYPE_ISIZE;
        type->name = (Token){0};
        type->inner = NULL;
        type->typeArgs = NULL;
        type->paramTypes = NULL;
        type->returnTypes = NULL;
        type->arrayLen = 0;
        base = type;
    } else if (match(parser, TOKEN_U8)) {
        Type* type = malloc(sizeof(Type));
        type->kind = TYPE_U8;
        type->name = (Token){0};
        type->inner = NULL;
        type->typeArgs = NULL;
        type->paramTypes = NULL;
        type->returnTypes = NULL;
        type->arrayLen = 0;
        base = type;
    } else if (match(parser, TOKEN_U16)) {
        Type* type = malloc(sizeof(Type));
        type->kind = TYPE_U16;
        type->name = (Token){0};
        type->inner = NULL;
        type->typeArgs = NULL;
        type->paramTypes = NULL;
        type->returnTypes = NULL;
        type->arrayLen = 0;
        base = type;
    } else if (match(parser, TOKEN_U32)) {
        Type* type = malloc(sizeof(Type));
        type->kind = TYPE_U32;
        type->name = (Token){0};
        type->inner = NULL;
        type->typeArgs = NULL;
        type->paramTypes = NULL;
        type->returnTypes = NULL;
        type->arrayLen = 0;
        base = type;
    } else if (match(parser, TOKEN_U64)) {
        Type* type = malloc(sizeof(Type));
        type->kind = TYPE_U64;
        type->name = (Token){0};
        type->inner = NULL;
        type->typeArgs = NULL;
        type->paramTypes = NULL;
        type->returnTypes = NULL;
        type->arrayLen = 0;
        base = type;
    } else if (match(parser, TOKEN_USIZE)) {
        Type* type = malloc(sizeof(Type));
        type->kind = TYPE_USIZE;
        type->name = (Token){0};
        type->inner = NULL;
        type->typeArgs = NULL;
        type->paramTypes = NULL;
        type->returnTypes = NULL;
        type->arrayLen = 0;
        base = type;
    } else if (match(parser, TOKEN_BYTE)) {
        Type* type = malloc(sizeof(Type));
        type->kind = TYPE_BYTE;
        type->name = (Token){0};
        type->inner = NULL;
        type->typeArgs = NULL;
        type->paramTypes = NULL;
        type->returnTypes = NULL;
        type->arrayLen = 0;
        base = type;
    } else if (match(parser, TOKEN_DOUBLE)) {
        Type* type = malloc(sizeof(Type));
        type->kind = TYPE_DOUBLE;
        type->name = (Token){0};
        type->inner = NULL;
        type->typeArgs = NULL;
        type->paramTypes = NULL;
        type->returnTypes = NULL;
        type->arrayLen = 0;
        base = type;
    } else if (match(parser, TOKEN_FLOAT)) {
        Type* type = malloc(sizeof(Type));
        type->kind = TYPE_FLOAT;
        type->name = (Token){0};
        type->inner = NULL;
        type->typeArgs = NULL;
        type->paramTypes = NULL;
        type->returnTypes = NULL;
        type->arrayLen = 0;
        base = type;
    } else if (match(parser, TOKEN_F32)) {
        Type* type = malloc(sizeof(Type));
        type->kind = TYPE_FLOAT;
        type->name = (Token){0};
        type->inner = NULL;
        type->typeArgs = NULL;
        type->paramTypes = NULL;
        type->returnTypes = NULL;
        type->arrayLen = 0;
        base = type;
    } else if (match(parser, TOKEN_F64)) {
        Type* type = malloc(sizeof(Type));
        type->kind = TYPE_DOUBLE;
        type->name = (Token){0};
        type->inner = NULL;
        type->typeArgs = NULL;
        type->paramTypes = NULL;
        type->returnTypes = NULL;
        type->arrayLen = 0;
        base = type;
    } else if (match(parser, TOKEN_F8)) {
        Type* type = malloc(sizeof(Type));
        type->kind = TYPE_F8;
        type->name = (Token){0};
        type->inner = NULL;
        type->typeArgs = NULL;
        type->paramTypes = NULL;
        type->returnTypes = NULL;
        type->arrayLen = 0;
        base = type;
    } else if (match(parser, TOKEN_F16)) {
        Type* type = malloc(sizeof(Type));
        type->kind = TYPE_F16;
        type->name = (Token){0};
        type->inner = NULL;
        type->typeArgs = NULL;
        type->paramTypes = NULL;
        type->returnTypes = NULL;
        type->arrayLen = 0;
        base = type;
    } else if (match(parser, TOKEN_BF8)) {
        Type* type = malloc(sizeof(Type));
        type->kind = TYPE_BF8;
        type->name = (Token){0};
        type->inner = NULL;
        type->typeArgs = NULL;
        type->paramTypes = NULL;
        type->returnTypes = NULL;
        type->arrayLen = 0;
        base = type;
    } else if (match(parser, TOKEN_BF16)) {
        Type* type = malloc(sizeof(Type));
        type->kind = TYPE_BF16;
        type->name = (Token){0};
        type->inner = NULL;
        type->typeArgs = NULL;
        type->paramTypes = NULL;
        type->returnTypes = NULL;
        type->arrayLen = 0;
        base = type;
    } else if (match(parser, TOKEN_STRING)) {
        Type* type = malloc(sizeof(Type));
        type->kind = TYPE_STRING;
        type->name = (Token){0};
        type->inner = NULL;
        type->typeArgs = NULL;
        type->paramTypes = NULL;
        type->returnTypes = NULL;
        type->arrayLen = 0;
        base = type;
    } else if (match(parser, TOKEN_BOOL)) {
        Type* type = malloc(sizeof(Type));
        type->kind = TYPE_BOOL;
        type->name = (Token){0};
        type->inner = NULL;
        type->typeArgs = NULL;
        type->paramTypes = NULL;
        type->returnTypes = NULL;
        type->arrayLen = 0;
        base = type;
    } else if (match(parser, TOKEN_IDENTIFIER)) {
        // Builtin raw pointer type: `ptr`
        if (parser->previous.length == 3 && memcmp(parser->previous.start, "ptr", 3) == 0) {
            Type* type = malloc(sizeof(Type));
            type->kind = TYPE_PTR;
            type->name = (Token){0};
            type->inner = NULL;
            type->typeArgs = NULL;
            type->paramTypes = NULL;
            type->returnTypes = NULL;
            type->arrayLen = 0;
            if (check(parser, TOKEN_LT)) {
                errorAtCurrent(parser, "Type 'ptr' does not accept generic arguments");
            }
            base = type;
        } else {
        Type* type = malloc(sizeof(Type));
        type->kind = TYPE_NAMED;
        type->name = parser->previous;
        type->inner = NULL;
        type->typeArgs = NULL;
        type->paramTypes = NULL;
        type->returnTypes = NULL;
        type->arrayLen = 0;

        // Generic type args: Name<Arg1, Arg2, ...>
        if (match(parser, TOKEN_LT)) {
            List* args = listNew();
            if (!check(parser, TOKEN_GT)) {
                do {
                    Type* a = parseType(parser);
                    listAppend(args, a);
                } while (match(parser, TOKEN_COMMA));
            }
            consume(parser, TOKEN_GT, "Expect '>' after generic type arguments");
            type->typeArgs = args;
        }
        base = type;
        }
    } else {
        printError(parser, "Expect type name");
        return NULL;
    }

    // Array suffix types: `T[]` (dynamic) and `T[N]` (fixed N)
    while (match(parser, TOKEN_LBRACKET)) {
        if (match(parser, TOKEN_RBRACKET)) {
            Type* arr = malloc(sizeof(Type));
            arr->kind = TYPE_ARRAY;
            arr->name = (Token){0};
            arr->inner = base;
            arr->typeArgs = NULL;
            arr->paramTypes = NULL;
            arr->returnTypes = NULL;
            arr->arrayLen = -1;
            base = arr;
            continue;
        }

        if (match(parser, TOKEN_INT) || match(parser, TOKEN_LONG)) {
            Token nTok = parser->previous;
            char* s = malloc((size_t)nTok.length + 1);
            memcpy(s, nTok.start, (size_t)nTok.length);
            s[nTok.length] = '\0';
            int64_t n = (int64_t)strtoll(s, NULL, 10);
            free(s);
            consume(parser, TOKEN_RBRACKET, "Expect ']' after array length");

            Type* arr = malloc(sizeof(Type));
            arr->kind = TYPE_ARRAY;
            arr->name = (Token){0};
            arr->inner = base;
            arr->typeArgs = NULL;
            arr->paramTypes = NULL;
            arr->returnTypes = NULL;
            arr->arrayLen = n;
            base = arr;
            continue;
        }

        printError(parser, "Expect ']' or integer length in array type");
        return NULL;
    }

    return base;
}

static Stmt* parseObjectDeclaration(Parser* parser) {
    Token name = consume(parser, TOKEN_IDENTIFIER, "Expect object name");
    while (match(parser, TOKEN_SEMICOLON)) {}
    consume(parser, TOKEN_LBRACE, "Expect '{' before object body");

    List* methods = listNew();
    while (!check(parser, TOKEN_RBRACE) && !check(parser, TOKEN_EOF)) {
        while (match(parser, TOKEN_SEMICOLON)) {}
        if (check(parser, TOKEN_RBRACE) || check(parser, TOKEN_EOF)) break;

        if (match(parser, TOKEN_PRIVATE)) {
            // ignore visibility for now
        }
        consume(parser, TOKEN_FUNC, "Expect 'fn' in object body");
        FuncStmt* method = (FuncStmt*)parseFunctionDeclaration(parser);
        listAppend(methods, method);
    }

    consume(parser, TOKEN_RBRACE, "Expect '}' after object body");

    ObjectStmt* stmt = malloc(sizeof(ObjectStmt));
    stmt->base.type = STMT_OBJECT;
    stmt->name = name;
    stmt->methods = methods;
    return (Stmt*)stmt;
}

static Stmt* parseEnumDeclaration(Parser* parser) {
    Token name = consume(parser, TOKEN_IDENTIFIER, "Expect enum name");
    while (match(parser, TOKEN_SEMICOLON)) {}
    consume(parser, TOKEN_LBRACE, "Expect '{' before enum body");

    List* variants = listNew();
    while (!check(parser, TOKEN_RBRACE) && !check(parser, TOKEN_EOF)) {
        while (match(parser, TOKEN_SEMICOLON) || match(parser, TOKEN_COMMA)) {}
        if (check(parser, TOKEN_RBRACE) || check(parser, TOKEN_EOF)) break;

        Token v = consume(parser, TOKEN_IDENTIFIER, "Expect enum variant");
        EnumVariantDecl* decl = malloc(sizeof(EnumVariantDecl));
        decl->name = v;
        decl->valueKind = ENUM_VALUE_NONE;
        decl->value = (Token){0};

        if (match(parser, TOKEN_ASSIGN)) {
            if (match(parser, TOKEN_INT) || match(parser, TOKEN_LONG)) {
                decl->valueKind = ENUM_VALUE_INT;
                decl->value = parser->previous;
            } else if (match(parser, TOKEN_STRING_LITERAL)) {
                decl->valueKind = ENUM_VALUE_STRING;
                decl->value = parser->previous;
            } else {
                errorAtCurrent(parser, "Expect integer or string literal after '=' in enum variant");
            }
        }

        listAppend(variants, decl);

        if (check(parser, TOKEN_COMMA)) {
            consume(parser, TOKEN_COMMA, "Expect ',' after variant");
        }
    }

    consume(parser, TOKEN_RBRACE, "Expect '}' after enum body");

    EnumStmt* stmt = malloc(sizeof(EnumStmt));
    stmt->base.type = STMT_ENUM;
    stmt->name = name;
    stmt->variants = variants;
    return (Stmt*)stmt;
}

static Stmt* declaration(Parser* parser) {
    while (match(parser, TOKEN_SEMICOLON)) {
        // skip statement separators (explicit ';' or newline)
    }
    if (check(parser, TOKEN_EOF) || check(parser, TOKEN_RBRACE)) return NULL;

    if (match(parser, TOKEN_IMPORT)) {
        Token kwImport = parser->previous;

        // Two supported forms:
        // - Import-all: `import "path.tua"`
        // - Namespace import: `import "path.tua" as ns`
        // - Named import: `import A,B,C from "path.tua"`
        if (check(parser, TOKEN_STRING_LITERAL)) {
            ImportStmt* stmt = malloc(sizeof(ImportStmt));
            stmt->base.type = STMT_IMPORT;
            stmt->keyword = kwImport;
            stmt->path = consume(parser, TOKEN_STRING_LITERAL, "Expect module path string after 'import'");
            stmt->hasAlias = false;
            stmt->alias = (Token){0};
            if (match(parser, TOKEN_AS)) {
                stmt->alias = consume(parser, TOKEN_IDENTIFIER, "Expect namespace identifier after 'as'");
                stmt->hasAlias = true;
            }
            if (check(parser, TOKEN_SEMICOLON)) consume(parser, TOKEN_SEMICOLON, "Expect statement separator after import");
            return (Stmt*)stmt;
        }

        // Named import form.
        FromImportStmt* stmt = malloc(sizeof(FromImportStmt));
        stmt->base.type = STMT_FROM_IMPORT;
        stmt->keywordImport = kwImport;

        List* names = listNew();
        do {
            Token name = consume(parser, TOKEN_IDENTIFIER, "Expect imported name after 'import'");
            ImportName* in = malloc(sizeof(ImportName));
            in->name = name;
            in->hasAlias = false;
            in->alias = (Token){0};
            if (match(parser, TOKEN_AS)) {
                in->alias = consume(parser, TOKEN_IDENTIFIER, "Expect alias identifier after 'as'");
                in->hasAlias = true;
            }
            listAppend(names, in);
        } while (match(parser, TOKEN_COMMA));
        stmt->names = names;

        stmt->keywordFrom = consume(parser, TOKEN_FROM, "Expect 'from' after import names");
        stmt->path = consume(parser, TOKEN_STRING_LITERAL, "Expect module path string after 'from'");

        if (check(parser, TOKEN_SEMICOLON)) consume(parser, TOKEN_SEMICOLON, "Expect statement separator after import-from");
        return (Stmt*)stmt;
    }

    if (match(parser, TOKEN_FROM)) {
        FromImportStmt* stmt = malloc(sizeof(FromImportStmt));
        stmt->base.type = STMT_FROM_IMPORT;
        stmt->keywordFrom = parser->previous;
        stmt->path = consume(parser, TOKEN_STRING_LITERAL, "Expect module path string after 'from'");
        stmt->keywordImport = consume(parser, TOKEN_IMPORT, "Expect 'import' after module path");

        List* names = listNew();
        do {
            Token name = consume(parser, TOKEN_IDENTIFIER, "Expect imported name");
            ImportName* in = malloc(sizeof(ImportName));
            in->name = name;
            in->hasAlias = false;
            in->alias = (Token){0};
            if (match(parser, TOKEN_AS)) {
                in->alias = consume(parser, TOKEN_IDENTIFIER, "Expect alias identifier after 'as'");
                in->hasAlias = true;
            }
            listAppend(names, in);
        } while (match(parser, TOKEN_COMMA));
        stmt->names = names;

        if (check(parser, TOKEN_SEMICOLON)) consume(parser, TOKEN_SEMICOLON, "Expect statement separator after from-import");
        return (Stmt*)stmt;
    }

    if (match(parser, TOKEN_PRIVATE)) {
        Token kw = parser->previous;
        Stmt* inner = declaration(parser);
        if (!inner) {
            errorAtCurrent(parser, "Expect declaration after 'private'");
            return NULL;
        }
        PrivateStmt* stmt = malloc(sizeof(PrivateStmt));
        stmt->base.type = STMT_PRIVATE;
        stmt->keyword = kw;
        stmt->inner = inner;
        return (Stmt*)stmt;
    }

    if (match(parser, TOKEN_FUNC)) {
        return parseFunctionDeclaration(parser);
    }

    if (match(parser, TOKEN_EXTERN)) {
        consume(parser, TOKEN_FUNC, "Expect 'fn' after 'extern'");
        return parseExternFunctionDeclaration(parser);
    }
    
    if (match(parser, TOKEN_VAR) || match(parser, TOKEN_CONST)) {
        return parseVarDeclaration(parser, false);
    }

    if (match(parser, TOKEN_STRUCT))
    {
        /* code */
        return parseStructDeclaration(parser);
    }

    if (match(parser, TOKEN_IMPL)) {
        return parseImplDeclaration(parser);
    }

    if (match(parser, TOKEN_OBJECT)) {
        return parseObjectDeclaration(parser);
    }

    if (match(parser, TOKEN_ENUM)) {
        return parseEnumDeclaration(parser);
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
