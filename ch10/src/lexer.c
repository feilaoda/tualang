#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "lexer.h"
static const char* TokenNames[] = {
    "LET",
    "CONST",
    "IF",
    "ELSE",
    "FOR", 
    "FUNC",
    "RETURN",
    "IN",
    "INT",
    "STRING",
    "ASSIGN",
    "PLUS",
    "MINUS",
    "STAR",
    "SLASH",
    "GT",
    "LT",
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
    "SEMICOLON",
    "COMMA",
    "IDENTIFIER",
    "NUMBER",
    "STRING_LITERAL",
    "NOT",
    "EOF",
    "ERROR"
};

const char* tokenToString(TokenType type) {
    if (type < 0 || type >= sizeof(TokenNames) / sizeof(TokenNames[0])) {
        return "UNKNOWN";
    }
    return TokenNames[type];
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
            case '\n':
                lexer->line++;
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
    return token;
}


static Token errorToken(Lexer* lexer, const char* message) {
    Token token;
    token.type = TOKEN_ERROR;
    token.start = message;
    token.length = (int)strlen(message);
    token.line = lexer->line;
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
        case '&': return checkKeyword(lexer, 1, 1, "&", TOKEN_AND);
        case 'e': return checkKeyword(lexer, 1, 3, "lse", TOKEN_ELSE);
        case 'f':
            if (lexer->current - lexer->start > 1) {
                switch (lexer->start[1]) {
                    case 'o': return checkKeyword(lexer, 2, 1, "r", TOKEN_FOR);
                    case 'u': return checkKeyword(lexer, 2, 2, "nc", TOKEN_FUNC);
                }
            }
            break;
        case 'i': ;
          if (lexer->current - lexer->start > 1) {
                switch (lexer->start[1]) {
                    case 'n': {
                        if (lexer->current - lexer->start > 2) {
                            if (memcmp(lexer->start, "int", 3) == 0) {
                                return TOKEN_INT;
                            }
                            
                        }
                        if (checkEqWord(lexer, 1, 1, "n")) {
                            return TOKEN_IN;
                        }
                    }
                    case 'f': return checkKeyword(lexer, 1, 1, "f", TOKEN_IF);
                }
          }
        case 'l': return checkKeyword(lexer, 1, 2, "et", TOKEN_LET);
        case 'c': return checkKeyword(lexer, 1, 4, "onst", TOKEN_CONST);
        case 'n': return checkKeyword(lexer, 1, 2, "ot", TOKEN_NOT);
        case '|': return checkKeyword(lexer, 1, 1, "|", TOKEN_OR);
        case 'r': return checkKeyword(lexer, 1, 5, "eturn", TOKEN_RETURN);
        case 's':
            if (lexer->current - lexer->start > 5) {
                if (memcmp(lexer->start, "string", 6) == 0) {
                    return TOKEN_STRING;
                }
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

static Token number(Lexer* lexer) {
    while (isDigit(peek(lexer))) advance(lexer);
    if (peek(lexer) == '.' && isDigit(peekNext(lexer))) {
        advance(lexer);
        while (isDigit(peek(lexer))) advance(lexer);
    }
    return makeToken(lexer, TOKEN_NUMBER);
}

static Token string(Lexer* lexer) {
    while (peek(lexer) != '"' && !isAtEnd(lexer)) {
        if (peek(lexer) == '\n') lexer->line++;
        advance(lexer);
    }
    if (isAtEnd(lexer)) return errorToken(lexer, "Unterminated string.");
    advance(lexer);
    return makeToken(lexer, TOKEN_STRING);
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
}


Token scanToken(Lexer* lexer) {
    skipWhitespace(lexer);
    
    lexer->start = lexer->current;
    
    if (isAtEnd(lexer)) {
     return makeToken(lexer, TOKEN_EOF);

    }

    
    char c = advance(lexer);
    // printf("scantoken c, %c %d\n", c, c);
    // Handle identifiers
    if (isAlpha(c)) return identifier(lexer);
    
    // Handle numbers
    if (isDigit(c)) return number(lexer);
    
    
    switch (c) {
        // Single character tokens
        case '(': return makeToken(lexer, TOKEN_LPAREN);
        case ')': return makeToken(lexer, TOKEN_RPAREN);
        case '{': return makeToken(lexer, TOKEN_LBRACE);
        case '}': return makeToken(lexer, TOKEN_RBRACE);
        case ';': return makeToken(lexer, TOKEN_SEMICOLON);
        case ',': return makeToken(lexer, TOKEN_COMMA);
        case ':': return makeToken(lexer, TOKEN_COLON);
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
        case '=': return makeToken(lexer, TOKEN_ASSIGN);
        case '<': return makeToken(lexer, TOKEN_LT);
        case '>': return makeToken(lexer, TOKEN_GT);
        case '&':
            if (match(lexer, '&')) return makeToken(lexer, TOKEN_AND);
            break;
        case '|':
            if (match(lexer, '|')) return makeToken(lexer, TOKEN_OR);
            break;
        
        // String literals
        case '"': return string(lexer);
        
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