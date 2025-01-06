%token WORD NUMBER SPACE

%start commands

%{
    #include <stdio.h>
    extern int yylex();
     extern int yylineno;
     extern FILE* yyin;
     void yyerror(const char* s);
     int yyparse();
%}

%%
commands: /* empty */
        | commands command
        ;

command: word
        ;        

word:  WORD
        | NUMBER
        ;

%%
#include <stdio.h>

extern char yytext[];
extern int column;

void yyerror(const char *s) {
  extern int yylineno;
  extern int columnNo;
  //MyFile<<"[-] ERROR : LINE "<<yylineno<<" COLUMN "<<columnNo<<" : "<<s<<"\n";
  printf("[-] ERROR : LINE %d COLUMN %d : %s\n", yylineno, columnNo, s);
}
