%{
    #include <stdio.h>
    #include "y.tab.h"
    extern int word;
%}

WORD    [a-zA-Z_][a-zA-Z0-9_]*
NUMBER  [0-9]+
SPACE   [ \n\t]*
%%
{WORD} {
    word++;
    printf("WORD %s\n", yytext);
    return WORD;  
    }
{SPACE} {
    printf("SPACE %s\n", yytext);
    return SPACE;
}
{NUMBER}    {
    printf("NUMBER %s\n", yytext);
    return NUMBER;
}

%%

int yywrap(void) {
    return 1;
}

