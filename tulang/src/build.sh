lex tua.l && yacc -d tua.y && gcc lex.c lex.yy.c y.tab.c -o tua 
