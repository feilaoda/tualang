#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "lexer.h"

static const char* TokenNames[] = {
    "VAR",
    "CONST",
    "IF", 
    "ELSE",
    "FOR",
    "FUNC",
    "RETURN",
    "IN",
    "INT",
    "LONG",
    "NULL",
    "TRUE",
    "FALSE",
    "DOUBLE",
    "STRING",
    "ASSIGN",
    "PLUS",
    "MINUS",
    "STAR",
    "SLASH",
    "GT",
    "LT",
    "LE",
    "GE",
    "OR",
    "AND",
    "ARROW",
    "COLON",
    "EQ",
    "NEQ",
    "INC",
    "DEC",
    "LPAREN",
    "RPAREN", 
    "LBRACE",
    "RBRACE",
    "LBRACKET",
    "RBRACKET",
    "SEMICOLON",
    "COMMA",
    "DOT",
    "IDENTIFIER",
    "STRING_LITERAL",
    "NOT",
    "PRINTLN",
    "PRINT",
    "INIT",
    "IMPL",
    "STRUCT",
    "EOF",
    "ERROR"
};



const char* tokenToString(TokenType type) {
    switch (type) {
        // 关键字
        case TOKEN_VAR: return "var/let";
        case TOKEN_CONST: return "const";
        case TOKEN_IF: return "if";
        case TOKEN_ELSE: return "else";
        case TOKEN_FOR: return "for";
        case TOKEN_WHILE: return "while";
        case TOKEN_DO: return "do";
        case TOKEN_BREAK: return "break";
        case TOKEN_CONTINUE: return "continue";
        case TOKEN_GOTO: return "goto";
        case TOKEN_IMPORT: return "import";
        case TOKEN_FROM: return "from";
        case TOKEN_AS: return "as";
        case TOKEN_FUNC: return "func/fn";
        case TOKEN_RETURN: return "return";
        case TOKEN_IN: return "in";
        case TOKEN_OBJECT: return "object";
        case TOKEN_ENUM: return "enum";
        case TOKEN_PRIVATE: return "private";
        case TOKEN_THIS: return "this";
        case TOKEN_DEINIT: return "deinit";
        
        // 类型
        case TOKEN_INT: return "int";
        case TOKEN_DOUBLE: return "double";
        case TOKEN_LONG: return "long";
        case TOKEN_STRING: return "string";
        case TOKEN_BOOL: return "bool";
        case TOKEN_NULL: return "null";
        case TOKEN_TRUE: return "true";
        case TOKEN_FALSE: return "false";
        
        // 运算符
        case TOKEN_ASSIGN: return "=";
        case TOKEN_PLUS: return "+";
        case TOKEN_MINUS: return "-";
        case TOKEN_STAR: return "*";
        case TOKEN_SLASH: return "/";
        case TOKEN_GT: return ">";
        case TOKEN_LT: return "<";
        case TOKEN_LE: return "<=";
        case TOKEN_GE: return ">=";
        case TOKEN_OR: return "||";
        case TOKEN_AND: return "&&";
        case TOKEN_COALESCE: return "??";
        case TOKEN_AMP: return "&";
        case TOKEN_ARROW: return "->";
        case TOKEN_COLON: return ":";
        case TOKEN_EQ: return "==";
        case TOKEN_NEQ: return "!=";
        case TOKEN_INC: return "++";
        case TOKEN_DEC: return "--";
        
        // 分隔符
        case TOKEN_LPAREN: return "(";
        case TOKEN_RPAREN: return ")";
        case TOKEN_LBRACE: return "{";
        case TOKEN_RBRACE: return "}";
        case TOKEN_LBRACKET: return "[";
        case TOKEN_RBRACKET: return "]";
        case TOKEN_SEMICOLON: return ";";
        case TOKEN_COMMA: return ",";
        case TOKEN_DOT: return ".";
        
        // 字面量和其他
        case TOKEN_IDENTIFIER: return "identifier";
        case TOKEN_NUMBER: return "number";
        case TOKEN_STRING_LITERAL: return "string_literal";
        case TOKEN_NOT: return "!";
        case TOKEN_PRINTLN: return "println";
        case TOKEN_PRINT: return "print";
        case TOKEN_INIT: return "init";
        case TOKEN_IMPL: return "impl";
        case TOKEN_STRUCT: return "struct";
        case TOKEN_EOF: return "EOF";
        case TOKEN_ERROR: return "ERROR";
        
        default: return "unknown";
    }
}
static bool isDigit(char c) {
    return c >= '0' && c <= '9';
}

static bool isAlpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool isAtEnd(Lexer* lexer) {
    return *lexer->current == '\0';
}

static char advance(Lexer* lexer) {
    lexer->current++;
    return lexer->current[-1];
}

static char peek(Lexer* lexer) {
    return *lexer->current;
}

static char peekNext(Lexer* lexer) {
    if (isAtEnd(lexer)) return '\0';
    return lexer->current[1];
}

static bool match(Lexer* lexer, char expected) {
    if (isAtEnd(lexer)) return false;
    if (*lexer->current != expected) return false;
    
    lexer->current++;
    return true;
}

static void skipWhitespace(Lexer* lexer) {
    for (;;) {
        char c = peek(lexer);
        switch (c) {
            case ' ':
            case '\r':
            case '\t':
            case '\v':
                advance(lexer);
                break;
            default:
                return;
        }
    }
}

static Token makeToken(Lexer* lexer, TokenType type) {
    Token token;
    token.type = type;
    token.start = lexer->start;
    token.length = (int)(lexer->current - lexer->start);
    token.line = lexer->line;
    token.col = lexer->lineStart ? (int)(lexer->start - lexer->lineStart) + 1 : 0;
    token.hasDot = 0;
    return token;
}


static Token errorToken(Lexer* lexer, const char* message) {
    Token token;
    token.type = TOKEN_ERROR;
    token.start = message;
    token.length = (int)strlen(message);
    token.line = lexer->line;
    token.col = lexer->lineStart ? (int)(lexer->current - lexer->lineStart) + 1 : 0;
    return token;
}

static bool checkEqWord(Lexer* lexer, int start, int length, const char* rest) {
    if (lexer->current - lexer->start == start + length &&
        memcmp(lexer->start + start, rest, length) == 0) {
        return true;
    }
    return false;
}

static TokenType checkKeyword(Lexer* lexer, int start, int length, const char* rest, TokenType type) {
    if (lexer->current - lexer->start == start + length &&
        memcmp(lexer->start + start, rest, length) == 0) {
        return type;
    }
    return TOKEN_IDENTIFIER;
}

static TokenType identifierType(Lexer* lexer) {
    switch (lexer->start[0]) {
        case 'a':
            if (lexer->current - lexer->start == 2 && memcmp(lexer->start, "as", 2) == 0) {
                return TOKEN_AS;
            }
            break;
        case '&': return checkKeyword(lexer, 1, 1, "&", TOKEN_AND);
        case 'e':
            if (lexer->current - lexer->start > 3 && memcmp(lexer->start, "enum", 4) == 0) {
                return TOKEN_ENUM;
            }
            return checkKeyword(lexer, 1, 3, "lse", TOKEN_ELSE);
        case 'w':
            if (lexer->current - lexer->start > 4 && memcmp(lexer->start, "while", 5) == 0) {
                return TOKEN_WHILE;
            }
            break;
        case 'f':
            if (lexer->current - lexer->start > 3 && memcmp(lexer->start, "from", 4) == 0) {
                return TOKEN_FROM;
            }
            if (lexer->current - lexer->start > 1) {
                
                switch (lexer->start[1]) {
                    case 'a': {
                        if (lexer->current - lexer->start > 4 && memcmp(lexer->start, "false", 5) == 0) {
                            return TOKEN_FALSE;
                        }
                        break;
                    }
                    case 'n': return checkKeyword(lexer, 1, 1, "n", TOKEN_FUNC);
                    case 'o': return checkKeyword(lexer, 2, 1, "r", TOKEN_FOR);
                    case 'u': return checkKeyword(lexer, 2, 2, "nc", TOKEN_FUNC);
                }
            }
            
            break;
        case 'd':
            if (lexer->current - lexer->start > 1 && memcmp(lexer->start, "do", 2) == 0) {
                return TOKEN_DO;
            }
            if (lexer->current - lexer->start > 5 && memcmp(lexer->start, "deinit", 6) == 0) {
                return TOKEN_DEINIT;
            }
            if (lexer->current - lexer->start > 5 && memcmp(lexer->start, "double", 6) == 0) {
                return TOKEN_DOUBLE;
            }
            break;
        case 'i': ;
          if (lexer->current - lexer->start > 5 && memcmp(lexer->start, "import", 6) == 0) {
              return TOKEN_IMPORT;
          }
          if (lexer->current - lexer->start > 1) {
                switch (lexer->start[1]) {
                    case 'n': {
                        if (lexer->current - lexer->start > 2) {
                            if (memcmp(lexer->start, "int", 3) == 0) {
                                return TOKEN_INT;
                            }
                            if(memcmp(lexer->start, "init", 4) == 0) {
                                return TOKEN_INIT;
                            }
                        }
                        if (checkEqWord(lexer, 1, 1, "n")) {
                            return TOKEN_IN;
                        }
                        break;
                    }
                    case 'f': return checkKeyword(lexer, 1, 1, "f", TOKEN_IF);
                    case 'm': return checkKeyword(lexer, 2, 2, "pl", TOKEN_IMPL);
                }
          }
          break;
        case 'v': return checkKeyword(lexer, 1, 2, "ar", TOKEN_VAR);
        case 'c':
            if (lexer->current - lexer->start > 7 && memcmp(lexer->start, "continue", 8) == 0) {
                return TOKEN_CONTINUE;
            }
            return checkKeyword(lexer, 1, 4, "onst", TOKEN_CONST);
        case 'l':
            if (lexer->current - lexer->start > 2 && memcmp(lexer->start, "let", 3) == 0) {
                return TOKEN_VAR;
            }
            if (lexer->current - lexer->start > 3 && memcmp(lexer->start, "long", 4) == 0) {
                return TOKEN_LONG;
            }
            break;
        case 'b':
            if (lexer->current - lexer->start > 4 && memcmp(lexer->start, "break", 5) == 0) {
                return TOKEN_BREAK;
            }
            if (lexer->current - lexer->start > 3 && memcmp(lexer->start, "bool", 4) == 0) {
                return TOKEN_BOOL;
            }
            if (lexer->current - lexer->start > 6 && memcmp(lexer->start, "boolean", 7) == 0) {
                return TOKEN_BOOL;
            }
            break;
        case 'n':
            if (lexer->current - lexer->start > 3 && memcmp(lexer->start, "null", 4) == 0) {
                return TOKEN_NULL;
            }
            return checkKeyword(lexer, 1, 2, "ot", TOKEN_NOT);
        case '|': return checkKeyword(lexer, 1, 1, "|", TOKEN_OR);
        case 'r': return checkKeyword(lexer, 1, 5, "eturn", TOKEN_RETURN);
        case 's':
            if (lexer->current - lexer->start > 5) {
                if (memcmp(lexer->start, "string", 6) == 0) {
                    return TOKEN_STRING;
                }
                if (memcmp(lexer->start, "struct", 6) == 0) {
                    return TOKEN_STRUCT;
                }
            }
            break;
        case 't':
            if (lexer->current - lexer->start > 3 && memcmp(lexer->start, "this", 4) == 0) {
                return TOKEN_THIS;
            }
            return checkKeyword(lexer, 1, 3, "rue", TOKEN_TRUE);
        case 'o':
            if (lexer->current - lexer->start > 5 && memcmp(lexer->start, "object", 6) == 0) {
                return TOKEN_OBJECT;
            }
            break;
        case 'p':
            if (lexer->current - lexer->start > 6 && memcmp(lexer->start, "private", 7) == 0) {
                return TOKEN_PRIVATE;
            }
            if (lexer->current - lexer->start > 5) {
                if (memcmp(lexer->start, "println", 7) == 0) {
                    return TOKEN_PRINTLN;
                }
            }
            if (lexer->current - lexer->start > 3) {
                if (memcmp(lexer->start, "print", 5) == 0) {
                    return TOKEN_PRINT;
                }
            }
            break;
        case 'g':
            if (lexer->current - lexer->start > 3 && memcmp(lexer->start, "goto", 4) == 0) {
                return TOKEN_GOTO;
            }
            break;
        default:
          return TOKEN_IDENTIFIER;
    }
    return TOKEN_IDENTIFIER;
}

static Token identifier(Lexer* lexer) {
    while (isAlpha(peek(lexer)) || isDigit(peek(lexer))) advance(lexer);
    return makeToken(lexer, identifierType(lexer));
}

// static Token number(Lexer* lexer) {
//     while (isDigit(peek(lexer))) advance(lexer);
//     if (peek(lexer) == '.' && isDigit(peekNext(lexer))) {
//         advance(lexer);
//         while (isDigit(peek(lexer))) advance(lexer);
//     }
//     return makeToken(lexer, TOKEN_NUMBER);
// }

static Token number(Lexer* lexer) {
    TokenType type = TOKEN_INT;  // Default to int
    
    // Parse integer part
    while (isDigit(peek(lexer))) advance(lexer);
    
    // Look for decimal point
    if (peek(lexer) == '.' && isDigit(peekNext(lexer))) {
        type = TOKEN_DOUBLE;
        advance(lexer); // consume '.'
        
        // Parse decimal part
        while (isDigit(peek(lexer))) advance(lexer);
        
        // Parse exponent if present
        if (peek(lexer) == 'e' || peek(lexer) == 'E') {
            advance(lexer);
            if (peek(lexer) == '+' || peek(lexer) == '-') advance(lexer);
            if (!isDigit(peek(lexer))) {
                return errorToken(lexer, "Invalid number format.");
            }
            while (isDigit(peek(lexer))) advance(lexer);
        }
    } else {
        // Check for long suffix
        if (peek(lexer) == 'l' || peek(lexer) == 'L') {
            type = TOKEN_LONG;
            advance(lexer);
        }
    }
    
    return makeToken(lexer, type);
}

static Token string(Lexer* lexer) {
    while (peek(lexer) != '"' && !isAtEnd(lexer)) {
        if (peek(lexer) == '\n') {
            advance(lexer);
            lexer->line++;
            lexer->lineStart = lexer->current;
        } else {
            advance(lexer);
        }
    }
    if (isAtEnd(lexer)) return errorToken(lexer, "Unterminated string.");
    advance(lexer);
    return makeToken(lexer, TOKEN_STRING_LITERAL);
}

// Token nextToken(Lexer* lexer) {
//     skipWhitespace(lexer);
//     lexer->start = lexer->current;
//     if (isAtEnd(lexer)) return makeToken(lexer, TOKEN_EOF);
//     char c = advance(lexer);
//     if (isDigit(c)) return number(lexer);
//     if (isAlpha(c)) return identifier(lexer);
//     switch (c) {
//         case '(': return makeToken(lexer, TOKEN_LPAREN);
//         case ')': return makeToken(lexer, TOKEN_RPAREN);
//         case '{': return makeToken(lexer, TOKEN_LBRACE);
//         case '}': return makeToken(lexer, TOKEN_RBRACE);
//         case ';': return makeToken(lexer, TOKEN_SEMICOLON);
//         case ',': return makeToken(lexer, TOKEN_COMMA);
//         case '+': return makeToken(lexer, TOKEN_PLUS);
//         case '-': return makeToken(lexer, TOKEN_MINUS);
//         case '*': return makeToken(lexer, TOKEN_STAR);
//         case '/': return makeToken(lexer, TOKEN_SLASH);
//         case '=': return makeToken(lexer, TOKEN_ASSIGN);
//         case '!': return makeToken(lexer, TOKEN_NOT);
//         case '<': return makeToken(lexer, TOKEN_LT);
//         case '>': return makeToken(lexer, TOKEN_GT);
//         case '"': return string(lexer);
//     }
//     return errorToken(lexer, "Unexpected character.");
// }

void initLexer(Lexer* lexer, const char* source) {
    lexer->start = source;
    lexer->current = source;
    lexer->line = 1;
    lexer->lineStart = source;
}


Token scanToken(Lexer* lexer) {
    skipWhitespace(lexer);
    
    lexer->start = lexer->current;
    
    if (isAtEnd(lexer)) {
     return makeToken(lexer, TOKEN_EOF);

    }

    // Treat newline as statement separator
    if (peek(lexer) == '\n') {
        advance(lexer);
        lexer->line++;
        lexer->lineStart = lexer->current;
        return makeToken(lexer, TOKEN_SEMICOLON);
    }
    
    char c = advance(lexer);
    // printf("scantoken c, %c %d\n", c, c);
    // Handle identifiers
    if (isAlpha(c)) return identifier(lexer);
    // if (isAlpha(c)) {
    //     Token identifier = makeToken(lexer, TOKEN_IDENTIFIER);
        
    //     // Look ahead for dot
    //     while (peek(lexer) == '.') {
    //         advance(lexer); // consume dot
            
    //         // Must be followed by identifier
    //         if (!isAlpha(peek(lexer))) {
    //             return errorToken(lexer, "Expect property name after '.'");
    //         }
            
    //         // Consume property/method name
    //         while (isAlpha(peek(lexer)) || isDigit(peek(lexer))) {
    //             advance(lexer);
    //         }
            
    //         // Update token length to include dot and property
    //         identifier.length = (int)(lexer->current - lexer->start);
    //         identifier.hasDot = 1;
    //     }
        
    //     return identifier;
    // }
    
    // Handle numbers
    if (isDigit(c)) return number(lexer);
    
    
    switch (c) {
        // Single character tokens
        case '(': return makeToken(lexer, TOKEN_LPAREN);
        case ')': return makeToken(lexer, TOKEN_RPAREN);
        case '{': return makeToken(lexer, TOKEN_LBRACE);
        case '}': return makeToken(lexer, TOKEN_RBRACE);
        case '[': return makeToken(lexer, TOKEN_LBRACKET);
        case ']': return makeToken(lexer, TOKEN_RBRACKET);
        case ';': return makeToken(lexer, TOKEN_SEMICOLON);
        case ',': return makeToken(lexer, TOKEN_COMMA);
        case '.': return makeToken(lexer, TOKEN_DOT);
        case ':': return makeToken(lexer, TOKEN_COLON);
        case '*': return makeToken(lexer, TOKEN_STAR); 
        case '+': {
          if(match(lexer, '+')) {
            return makeToken(lexer, TOKEN_INC);
          }
          return makeToken(lexer, TOKEN_PLUS);
        }

        case '-': 
            if (match(lexer, '>')) return makeToken(lexer, TOKEN_ARROW);
            else if (match(lexer, '-')) return makeToken(lexer, TOKEN_DEC);
            else
            return makeToken(lexer, TOKEN_MINUS);
        
        // Multi-character tokens
        case '=':
            if (match(lexer, '=')) return makeToken(lexer, TOKEN_EQ);
            return makeToken(lexer, TOKEN_ASSIGN);
        case '<': 
        {
            if (match(lexer, '=')) return makeToken(lexer, TOKEN_LE);
            return makeToken(lexer, TOKEN_LT);
            break;
        }
        case '>': {
            if (match(lexer, '=')) return makeToken(lexer, TOKEN_GE);
            return makeToken(lexer, TOKEN_GT);
            break;
        }
        case '&':
            if (match(lexer, '&')) return makeToken(lexer, TOKEN_AND);
            return makeToken(lexer, TOKEN_AMP);
        case '|':
            if (match(lexer, '|')) return makeToken(lexer, TOKEN_OR);
            //或
            break;
        case '?':
            if (match(lexer, '?')) return makeToken(lexer, TOKEN_COALESCE);
            return errorToken(lexer, "Unexpected '?' (did you mean '??'?)");
        
        // String literals
        case '"': return string(lexer);
        case '!': {
            if (match(lexer, '=')) return makeToken(lexer, TOKEN_NEQ);
            return makeToken(lexer, TOKEN_NOT);
        }
        // Comments
        case '/':
            if (match(lexer, '/')) {
                // Single line comment
                while (peek(lexer) != '\n' && !isAtEnd(lexer)) advance(lexer);
                return scanToken(lexer);
            } else if (match(lexer, '*')) {
                // Multi-line comment
                while (!isAtEnd(lexer)) {
                    if (peek(lexer) == '*' && peekNext(lexer) == '/') {
                        advance(lexer); // consume *
                        advance(lexer); // consume /
                        return scanToken(lexer);
                    }
                    advance(lexer);
                }
                return errorToken(lexer, "Unterminated multi-line comment");
            }
            return makeToken(lexer, TOKEN_SLASH);
    }
    
    printf("scantoken error,return -1");
    return errorToken(lexer, "Unexpected character");
}

// static bool match(Lexer* lexer, char expected) {
//     if (isAtEnd(lexer)) return false;
//     if (*lexer->current != expected) return false;
//     lexer->current++;
//     return true;
// }

// static void skipWhitespace(Lexer* lexer) {
//     for (;;) {
//         char c = peek(lexer);
//         switch (c) {
//             case ' ':
//             case '\r':
//             case '\t':
//                 advance(lexer);
//                 break;
//             case '\n':
//                 lexer->line++;
//                 advance(lexer);
//                 break;
//             default:
//                 return;
//         }
//     }
// }

// static Token string(Lexer* lexer) {
//     while (peek(lexer) != '"' && !isAtEnd(lexer)) {
//         if (peek(lexer) == '\n') lexer->line++;
//         advance(lexer);
//     }
    
//     if (isAtEnd(lexer)) return errorToken(lexer, "Unterminated string");
    
//     advance(lexer); // The closing "
//     return makeToken(lexer, TOKEN_STRING_LITERAL);
// }

// static Token number(Lexer* lexer) {
//     while (isDigit(peek(lexer))) advance(lexer);
    
//     // Look for decimal point
//     if (peek(lexer) == '.' && isDigit(peekNext(lexer))) {
//         advance(lexer); // Consume the "."
//         while (isDigit(peek(lexer))) advance(lexer);
//     }
    
//     return makeToken(lexer, TOKEN_NUMBER);
// }

// static Token identifier(Lexer* lexer) {
//     while (isAlpha(peek(lexer)) || isDigit(peek(lexer))) advance(lexer);
//     return makeToken(lexer, identifierType(lexer));
// }
