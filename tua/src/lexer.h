#ifndef LEXER_H
#define LEXER_H

#define bool int
#define true 1
#define false 0

typedef enum {
    // Keywords
    TOKEN_VAR=0,
    TOKEN_CONST,
    TOKEN_IF,
    TOKEN_ELSE,
    TOKEN_FOR,
    TOKEN_WHILE,
    TOKEN_DO,
    TOKEN_BREAK,
    TOKEN_CONTINUE,
    TOKEN_GOTO,
    TOKEN_IMPORT,
    TOKEN_FROM,
    TOKEN_AS,
    TOKEN_FUNC,
    TOKEN_RETURN,
    TOKEN_IN,
    TOKEN_OBJECT,
    TOKEN_ENUM,
    TOKEN_PRIVATE,
    TOKEN_THIS,
    TOKEN_DEINIT,
    // Types
    TOKEN_INT,
    TOKEN_DOUBLE,
    TOKEN_FLOAT,
    TOKEN_LONG,
    TOKEN_STRING,
    TOKEN_BOOL,
    TOKEN_NULL,
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
    TOKEN_COALESCE,  // ??
    TOKEN_AMP,       // &
    TOKEN_ARROW,     // ->
    TOKEN_COLON,     // :
    TOKEN_EQ,         //==
    TOKEN_NEQ,        //!=
    TOKEN_INC, //++
    TOKEN_DEC, //--
    TOKEN_DOT, // .
    // Delimiters
    TOKEN_LPAREN,    // (
    TOKEN_RPAREN,    // )
    TOKEN_LBRACE,    // {
    TOKEN_RBRACE,    // }
    TOKEN_LBRACKET,  // [
    TOKEN_RBRACKET,  // ]
    TOKEN_SEMICOLON, // ;
    TOKEN_COMMA,     // ,
    // Literals
    TOKEN_IDENTIFIER,
    TOKEN_NUMBER,
    TOKEN_STRING_LITERAL,
    TOKEN_NOT,
    TOKEN_PRINTLN,
    TOKEN_PRINT,
    TOKEN_INIT,
    TOKEN_IMPL,
    TOKEN_STRUCT,
    TOKEN_EOF,
    TOKEN_ERROR
} TokenType;

typedef struct {
    TokenType type;
    const char* start;
    int length;
    int line;
    int col;   // 1-based column within the line
    int hasDot;
} Token;


typedef struct {
    const char* start;
    const char* current;
    int line;
    const char* lineStart;
} Lexer;

void initLexer(Lexer* lexer, const char* source);
Token nextToken(Lexer* lexer);
Token scanToken(Lexer* lexer);
const char* tokenToString(TokenType type);
#endif
