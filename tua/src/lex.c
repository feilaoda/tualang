#include <stdio.h>
#include <stdlib.h>
#include "y.tab.h"

extern int yyparse();
 int yylineno;
 int columnNo;
     int word = 0;
int main(int argc, char *argv[]) {
    FILE *f = fopen(argv[1], "r");
    if (f == NULL) {
        perror("fopen");
        return 1;
    }
    FILE* yyin = f;

   
    yyparse();
    printf("word count %d\n", word);
    fclose(f);
    return 0;
}