// ANTLR4 reference grammar for the current Tua surface syntax.
// Source-of-truth: src/lexer.c + src/parser.c + docs/SPEC.md
//
// Notes:
// - Newlines act as statement separators (tokenized as SEMI).
// - This file is intended as a documentation / tooling grammar; it does not encode
//   semantic constraints (borrow checking, map key const-ness checks, etc.).
grammar tuaparser;

// -------------------------
// Parser rules
// -------------------------

compilationUnit
  : sep* topLevelItem* EOF
  ;

topLevelItem
  : declaration
  | statement
  ;

declaration
  : importDecl sep?
  | fromImportDecl sep?
  | privateDecl
  | functionDecl
  | externFunctionDecl
  | varDecl
  | typedVarDeclSugar
  | structDecl
  | traitDecl
  | implDecl
  | objectDecl
  | enumDecl
  ;

privateDecl
  : KW_PRIVATE (declaration | statement) // matches current hand-written parser behavior
  ;

importDecl
  : KW_IMPORT STRING_LIT (KW_AS Identifier)?
  | KW_IMPORT importName (COMMA importName)* KW_FROM STRING_LIT
  ;

fromImportDecl
  : KW_FROM STRING_LIT KW_IMPORT importName (COMMA importName)*
  ;

importName
  : Identifier (KW_AS Identifier)?
  ;

functionDecl
  : KW_FN Identifier typeParamList? LPAREN paramList? RPAREN returnSig? block
  ;

externFunctionDecl
  : KW_EXTERN KW_FN Identifier LPAREN paramList? RPAREN externReturnSig? (KW_AS Identifier)? sep?
  ;

typeParamList
  : LT sep* typeParam (sep* COMMA sep* typeParam)* sep* GT
  ;

typeParam
  : Identifier (COLON Identifier)?
  ;

paramList
  : param (COMMA param)*
  ;

param
  : paramMode? Identifier (COLON type)?
  ;

paramMode
  : KW_CONST
  | KW_LET
  | KW_MOVE
  ;

// Normal functions:
// - Preferred: `-> T[,U...]`
// - Sugar (current impl): only for primitive return types, without `->`
returnSig
  : ARROW typeList
  | primitiveTypeList
  ;

primitiveTypeList
  : primitiveType (COMMA type)*
  ;

// extern functions accept broader sugar: `extern fn f(...) T` (without `->`)
externReturnSig
  : ARROW typeList
  | typeList
  ;

typeList
  : type (COMMA type)*
  ;

// `let`/`const` declarations.
// - `let a[:T] = expr, b[:U] = expr`
// - `let a, b = expr` (destructure)
// - `let a, b` (multiple decls without init)
varDecl
  : (KW_LET | KW_CONST) varDeclBody sep?
  ;

varDeclBody
  : destructureDecl
  | multiDeclNoInit
  | binding (COMMA binding)*
  ;

binding
  : Identifier (COLON type)? (ASSIGN expression)?
  ;

destructureDecl
  : destructureTargets ASSIGN expression
  ;

multiDeclNoInit
  : destructureTargets
  ;

// Type-annotated declaration sugar (no `let/const` keyword):
// `name: Type = expr`
typedVarDeclSugar
  : Identifier COLON type (ASSIGN expression)? sep?
  ;

destructureTargets
  : Identifier (COLON type)? (COMMA Identifier (COLON type)?)+
  ;

structDecl
  : KW_STRUCT Identifier sep* LBRACE sep* structItem* RBRACE
  ;

structItem
  : sep+
  | KW_PRIVATE KW_FN functionDeclLike
  | KW_FN functionDeclLike
  | (KW_INIT | KW_DEINIT) LPAREN RPAREN returnSig? block
  | embeddedField
  | fieldDecl
  ;

functionDeclLike
  : Identifier typeParamList? LPAREN paramList? RPAREN returnSig? block
  ;

embeddedField
  : ELLIPSIS type (KW_AS Identifier)? (COMMA)? sep*
  ;

fieldDecl
  : KW_CONST? Identifier COLON type (ASSIGN expression)? (COMMA)? sep*
  ;

traitDecl
  : KW_TRAIT Identifier sep* LBRACE sep* traitItem* RBRACE
  ;

traitItem
  : sep+
  | KW_PRIVATE? KW_FN traitMethodDecl
  ;

traitMethodDecl
  : Identifier LPAREN traitParamList? RPAREN (returnSig | externReturnSig)? sep?
  ;

traitParamList
  : traitParam (COMMA traitParam)*
  ;

traitParam
  : paramMode? Identifier COLON type
  ;

implDecl
  : KW_IMPL Identifier (KW_FOR Identifier)? sep* LBRACE sep* implItem* RBRACE
  ;

implItem
  : sep+
  | KW_PRIVATE KW_FN functionDeclLike
  | KW_FN functionDeclLike
  | (KW_INIT | KW_DEINIT) LPAREN RPAREN returnSig? block
  ;

objectDecl
  : KW_OBJECT Identifier sep* LBRACE sep* objectItem* RBRACE
  ;

objectItem
  : sep+
  | KW_PRIVATE? KW_FN functionDeclLike
  ;

enumDecl
  : KW_ENUM Identifier sep* LBRACE sep* enumVariantList? sep* RBRACE
  ;

enumVariantList
  : enumVariant (sepOrComma+ enumVariant)* sepOrComma*
  ;

enumVariant
  : Identifier (ASSIGN (INT_LIT | LONG_LIT | STRING_LIT))?
  ;

statement
  : sep+ // allow stray separators
  | labelStmt sep?
  | ifStmt
  | forStmt
  | whileStmt
  | doWhileStmt
  | breakStmt
  | continueStmt
  | gotoStmt
  | returnStmt
  | block
  | destructureAssignStmt
  | exprStmt
  ;

block
  : LBRACE sep* (topLevelItem sep*)* RBRACE
  ;

ifStmt
  : KW_IF (LPAREN expression RPAREN | expression) statement (sep* KW_ELSE (ifStmt | statement))?
  ;

forStmt
  : KW_FOR forClause sep* block
  ;

forClause
  : LPAREN? (forInClause | forCClause) RPAREN?
  ;

forInClause
  : Identifier (COMMA Identifier)? KW_IN expression
  ;

forCClause
  : forInit? SEMI expression? SEMI expression?
  ;

forInit
  : (KW_LET)? Identifier (COLON type)? ASSIGN expression
  | expression
  ;

whileStmt
  : KW_WHILE (LPAREN expression RPAREN | expression) statement
  ;

doWhileStmt
  : KW_DO statement sep* KW_WHILE (LPAREN expression RPAREN | expression) sep?
  ;

breakStmt
  : KW_BREAK sep?
  ;

continueStmt
  : KW_CONTINUE sep?
  ;

gotoStmt
  : KW_GOTO Identifier sep?
  ;

returnStmt
  : KW_RETURN (expression (COMMA expression)*)? sep?
  ;

labelStmt
  : DBLCOLON Identifier DBLCOLON
  ;

destructureAssignStmt
  : Identifier (COMMA Identifier)+ ASSIGN expression sep?
  ;

exprStmt
  : expression sep?
  ;

sepOrComma
  : sep
  | COMMA
  ;

sep
  : SEMI
  ;

// -------------------------
// Expressions (precedence)
// -------------------------

expression
  : assignmentExpr
  ;

assignmentExpr
  : coalesceExpr (ASSIGN assignmentExpr)?
  ;

coalesceExpr
  : logicalOrExpr (COALESCE coalesceExpr)?
  ;

logicalOrExpr
  : logicalAndExpr (OR logicalAndExpr)*
  ;

logicalAndExpr
  : bitOrExpr (AND bitOrExpr)*
  ;

bitOrExpr
  : bitXorExpr (BOR bitXorExpr)*
  ;

bitXorExpr
  : bitAndExpr (BXOR bitAndExpr)*
  ;

bitAndExpr
  : equalityExpr (AMP equalityExpr)*
  ;

equalityExpr
  : relationalExpr ((EQ | NEQ) relationalExpr)*
  ;

relationalExpr
  : shiftExpr (relOp shiftExpr)*
  ;

shiftExpr
  : additiveExpr (shiftOp additiveExpr)*
  ;

relOp
  : LE
  | GE
  | LT { _input.LA(1) != LT }?
  | GT { _input.LA(1) != GT }?
  ;

shiftOp
  : LT LT
  | GT GT
  ;

additiveExpr
  : multiplicativeExpr ((PLUS | MINUS) multiplicativeExpr)*
  ;

multiplicativeExpr
  : unaryExpr ((STAR | SLASH) unaryExpr)*
  ;

unaryExpr
  : castExpr
  | (INC | DEC | MINUS | NOT | BNOT | AMP | KW_MOVE) unaryExpr
  | postfixExpr
  ;

// Java-style cast: `(T)expr`, restricted to builtin cast types to avoid ambiguity.
castExpr
  : LPAREN castType RPAREN unaryExpr
  ;

castType
  : KW_INT
  | KW_LONG
  | KW_I8
  | KW_I16
  | KW_ISIZE
  | KW_U8
  | KW_U16
  | KW_U32
  | KW_U64
  | KW_USIZE
  | KW_BYTE
  | KW_FLOAT
  | KW_DOUBLE
  | KW_F8
  | KW_F16
  | KW_F32
  | KW_F64
  | KW_BF8
  | KW_BF16
  | KW_BOOL
  | KW_STRING
  ;

postfixExpr
  : primaryExpr postfixSuffix*
  ;

postfixSuffix
  : memberSuffix
  | genericCallSuffix
  | callSuffix
  | indexSuffix
  | structInitSuffix
  | (INC | DEC)
  | (KW_AS type)
  ;

memberSuffix
  : DOT Identifier
  ;

callSuffix
  : LPAREN argumentList? RPAREN
  ;

genericCallSuffix
  : LT typeList GT LPAREN argumentList? RPAREN
  ;

argumentList
  : expression (COMMA expression)*
  ;

indexSuffix
  : LBRACK expression RBRACK
  ;

structInitSuffix
  : LBRACE sep* structInitFieldList? sep* RBRACE
  ;

structInitFieldList
  : structInitField (COMMA sep* structInitField)* (COMMA)?
  ;

structInitField
  : Identifier COLON expression
  ;

primaryExpr
  : literal
  | arrayLiteral
  | braceLiteral
  | KW_THIS
  | builtinIdent
  | qualifiedName
  | LPAREN expression RPAREN
  | lambdaExpr
  ;

builtinIdent
  : KW_PRINT
  | KW_PRINTLN
  ;

qualifiedName
  : Identifier (DOT Identifier)*
  ;

lambdaExpr
  : KW_FN LPAREN paramList? RPAREN lambdaReturnSig? block
  ;

lambdaReturnSig
  : ARROW typeList
  | typeList // sugar: `fn(...) int {}` (parser accepts; may error for function-type return without `->`)
  ;

literal
  : INT_LIT
  | LONG_LIT
  | FLOAT_LIT
  | STRING_LIT
  | KW_NULL
  | KW_TRUE
  | KW_FALSE
  ;

arrayLiteral
  : LBRACK sep* (expression (COMMA sep* expression)*)? (COMMA)? sep* RBRACK
  ;

// `{ ... }` is either map-literal or array-literal; empty `{}` is ambiguous.
braceLiteral
  : LBRACE sep* (mapLiteralBody | arrayLiteralBody)? sep* RBRACE
  ;

mapLiteralBody
  : mapEntry (COMMA sep* mapEntry)* (COMMA)?
  ;

mapEntry
  : mapKey COLON expression
  ;

mapKey
  : INT_LIT
  | LONG_LIT
  | STRING_LIT
  ;

arrayLiteralBody
  : expression (COMMA sep* expression)* (COMMA)?
  ;

// -------------------------
// Types
// -------------------------

type
  : typePrimary typeSuffix*
  ;

typeSuffix
  : LBRACK RBRACK
  | LBRACK INT_LIT RBRACK
  ;

typePrimary
  : functionType
  | refType
  | primitiveType
  | ptrType
  | namedType
  ;

functionType
  : LPAREN typeList? RPAREN (ARROW functionTypeReturn | functionTypeReturn)
  ;

functionTypeReturn
  : LPAREN typeList? RPAREN
  | type
  ;

refType
  : AMP type
  | KW_REF LT type GT
  ;

ptrType
  : KW_PTR
  ;

primitiveType
  : KW_INT
  | KW_LONG
  | KW_I8
  | KW_I16
  | KW_ISIZE
  | KW_U8
  | KW_U16
  | KW_U32
  | KW_U64
  | KW_USIZE
  | KW_BYTE
  | KW_FLOAT
  | KW_DOUBLE
  | KW_F8
  | KW_F16
  | KW_F32
  | KW_F64
  | KW_BF8
  | KW_BF16
  | KW_BOOL
  | KW_STRING
  | KW_VOID
  | KW_ANY
  ;

namedType
  : qualifiedTypeName typeArgList?
  ;

qualifiedTypeName
  : Identifier (DOT Identifier)*
  ;

typeArgList
  : LT type (COMMA type)* GT
  ;

// -------------------------
// Lexer rules
// -------------------------

// Keywords
KW_LET     : 'let';
KW_CONST   : 'const';
KW_IF      : 'if';
KW_ELSE    : 'else';
KW_FOR     : 'for';
KW_WHILE   : 'while';
KW_DO      : 'do';
KW_BREAK   : 'break';
KW_CONTINUE: 'continue';
KW_GOTO    : 'goto';
KW_IMPORT  : 'import';
KW_FROM    : 'from';
KW_AS      : 'as';
KW_FN      : 'fn';
KW_EXTERN  : 'extern';
KW_PRIVATE : 'private';
KW_RETURN  : 'return';
KW_STRUCT  : 'struct';
KW_OBJECT  : 'object';
KW_ENUM    : 'enum';
KW_IMPL    : 'impl';
KW_TRAIT   : 'trait';
KW_INIT    : 'init';
KW_DEINIT  : 'deinit';
KW_THIS    : 'this';
KW_MOVE    : 'move';
KW_IN      : 'in';
KW_PRINTLN : 'println';
KW_PRINT   : 'print';
KW_NULL    : 'null';
KW_TRUE    : 'true';
KW_FALSE   : 'false';

// Type keywords / builtins (some are currently parsed as identifiers in the C parser,
// but are included here for clarity in tooling).
KW_PTR     : 'ptr';
KW_REF     : 'Ref';
KW_ANY     : 'any';
KW_VOID    : 'void';

KW_INT     : 'int';
KW_LONG    : 'long';
KW_DOUBLE  : 'double';
KW_FLOAT   : 'float';
KW_STRING  : 'string';
KW_BOOL    : 'bool';
KW_BYTE    : 'byte';
KW_U8      : 'u8';
KW_I8      : 'i8';
KW_U16     : 'u16';
KW_I16     : 'i16';
KW_U32     : 'u32';
KW_U64     : 'u64';
KW_USIZE   : 'usize';
KW_ISIZE   : 'isize';
KW_F8      : 'f8';
KW_F16     : 'f16';
KW_F32     : 'f32';
KW_F64     : 'f64';
KW_BF8     : 'bf8';
KW_BF16    : 'bf16';

// Operators / punctuation (longer first)
ELLIPSIS : '...';
DBLCOLON : '::';
COALESCE : '??';
ARROW    : '->';
LE       : '<=';
GE       : '>=';
EQ       : '==';
NEQ      : '!=';
AND      : '&&';
OR       : '||';
INC      : '++';
DEC      : '--';

ASSIGN   : '=';
PLUS     : '+';
MINUS    : '-';
STAR     : '*';
SLASH    : '/';
LT       : '<';
GT       : '>';
NOT      : '!';
AMP      : '&';
BOR      : '|';
BXOR     : '^';
BNOT     : '~';
QUESTION : '?';
DOT      : '.';
COLON    : ':';
COMMA    : ',';

LPAREN   : '(';
RPAREN   : ')';
LBRACE   : '{';
RBRACE   : '}';
LBRACK   : '[';
RBRACK   : ']';

// Statement separator: semicolon OR newline(s)
SEMI
  : ';'
  | [\r\n]+
  ;

LONG_LIT  : Digit+ [lL];
INT_LIT   : Digit+;
FLOAT_LIT : Digit+ '.' Digit+ ExponentPart?
          | Digit+ ExponentPart
          ;

STRING_LIT
  : '"' ( '\\' . | ~["\\] )* '"'
  ;

Identifier
  : [A-Za-z_][A-Za-z0-9_]*
  ;

WS
  : [ \t\f]+ -> skip
  ;

LINE_COMMENT
  : '//' ~[\r\n]* -> skip
  ;

BLOCK_COMMENT
  : '/*' .*? '*/' -> skip
  ;

fragment ExponentPart
  : [eE] [+-]? Digit+
  ;

fragment Digit
  : [0-9]
  ;
