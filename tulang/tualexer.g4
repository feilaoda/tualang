lexer grammar tualexer;

options {
}



BREAK    : 'break';
GOTO     : 'goto';
DO       : 'do';
WHILE    : 'while';
MATCH    : 'match';
IF       : 'if';
ELSE     : 'else';
FOR      : 'for';
COMMA    : ',';
IN       : 'in';
FUNCTION : 'fn';
LET      : 'let';
CONST    : 'const';
SWITCH   : 'switch';
CASE     : 'case';
RETURN   : 'return';
CONTINUE : 'continue';
NULL     : 'null';
FALSE    : 'false';
TRUE     : 'true';

LPAREN : '(';
RPAREN : ')';
LBRACE : '{';
RBRACE : '}';
LBRACK : '[';
RBRACK : ']';
SEMI   : ';';
DOT    : '.';


ASSIGN     : '=';
GT         : '>';
LT         : '<';
BANG       : '!';
TILDE      : '~';
QUESTION   : '?';
COLON      : ':';
EQUAL      : '==';
LE         : '<=';
GE         : '>=';
NOTEQUAL   : '!=';
AND        : '&&';
OR         : '||';
INC        : '++';
DEC        : '--';
ADD        : '+';
SUB        : '-';
MUL        : '*';
DIV        : '/';
BITAND     : '&';
BITOR      : '|';
CARET      : '^';
MOD        : '%';
ARROW      : '->';
COLONCOLON : '::';

ADD_ASSIGN     : '+=';
SUB_ASSIGN     : '-=';
MUL_ASSIGN     : '*=';
DIV_ASSIGN     : '/=';
AND_ASSIGN     : '&=';
OR_ASSIGN      : '|=';
XOR_ASSIGN     : '^=';
MOD_ASSIGN     : '%=';
LSHIFT_ASSIGN  : '<<=';
RSHIFT_ASSIGN  : '>>=';
URSHIFT_ASSIGN : '>>>=';


CHAR: '\'' SingleCharacter '\'' | '\'' EscapeSequence '\'';

fragment SingleCharacter: ~['\\\r\n];

fragment EscapeSequence:
    '\\' [abfnrtvz"'|$#\\] // World of Warcraft Lua additionally escapes |$# 
    | '\\' '\r'? '\n'
    | DecimalEscape
    | HexEscape
    | UtfEscape
;

fragment DecimalEscape: '\\' Digit | '\\' Digit Digit | '\\' [0-2] Digit Digit;

fragment HexEscape: '\\' 'x' HexDigit HexDigit;

fragment UtfEscape: '\\' 'u{' HexDigit+ '}';

fragment Digit: [0-9];

fragment HexDigit: [0-9a-fA-F];

NORMALSTRING: '"' ( EscapeSequence | ~('\\' | '"'))* '"';

CHARSTRING: '\'' ( EscapeSequence | ~('\'' | '\\'))* '\'';


WS: [ \t\u000C\r]+ -> channel(HIDDEN);

NL: [\n] -> channel(2);

COMMENT: '//' { this.HandleComment(); } -> channel(HIDDEN);
