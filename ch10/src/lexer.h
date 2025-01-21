#ifndef LEXER_H
#define LEXER_H

#define bool int
#define true 1
#define false 0

typedef enum {
    // Keywords
    TOKEN_LET=0,
    TOKEN_CONST,
    TOKEN_IF,
    TOKEN_ELSE,
    TOKEN_FOR,
    TOKEN_FUNC,
    TOKEN_RETURN,
    TOKEN_IN,
    // Types
    TOKEN_INT,
    TOKEN_DOUBLE,
    TOKEN_LONG,
    TOKEN_STRING,
    TOKEN_TRUE,
    TOKEN_FALSE,
    // Operators
    TOKEN_ASSIGN,    // =
    TOKEN_PLUS,      // +
    TOKEN_MINUS,     // -
    TOKEN_STAR,      // *
    TOKEN_SLASH,     // /
    TOKEN_GT,        // >
    TOKEN_LT,        // <
    TOKEN_LE,        // <=
    TOKEN_GE,        // >=
    TOKEN_OR,        // ||
    TOKEN_AND,       // &&
    TOKEN_ARROW,     // ->
    TOKEN_COLON,     // :
    TOKEN_EQ,         //==
    TOKEN_NEQ,        //!=
    TOKEN_INC, //++
    TOKEN_DEC, //--
    // Delimiters
    TOKEN_LPAREN,    // (
    TOKEN_RPAREN,    // )
    TOKEN_LBRACE,    // {
    TOKEN_RBRACE,    // }
    TOKEN_SEMICOLON, // ;
    TOKEN_COMMA,     // ,
    // Literals
    TOKEN_IDENTIFIER,
    TOKEN_NUMBER,
    TOKEN_STRING_LITERAL,
    TOKEN_NOT,
    TOKEN_PRINTLN,
    TOKEN_PRINT,
    TOKEN_EOF,
    TOKEN_ERROR
} TokenType;

typedef struct {
    TokenType type;
    const char* start;
    int length;
    int line;
} Token;


typedef struct {
    const char* start;
    const char* current;
    int line;
} Lexer;

void initLexer(Lexer* lexer, const char* source);
Token nextToken(Lexer* lexer);
Token scanToken(Lexer* lexer);
const char* tokenToString(TokenType type);
#endif