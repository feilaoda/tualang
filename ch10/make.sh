#!/bin/bash
# gcc -DDEBUG -o bin/tua src/main.c src/lexer.c src/parser.c src/compiler.c src/list.c

CFLAGS="-I/usr/local/Cellar/llvm/19.1.6/include  -D__STDC_CONSTANT_MACROS -D__STDC_FORMAT_MACROS -D__STDC_LIMIT_MACROS"
LDFLAGS="-L/usr/local/Cellar/llvm/19.1.6/lib -Wl,-search_paths_first -Wl,-headerpad_max_install_names"
LIBS="-lLLVM-19"
# echo 'clang $(CFLAGS) src/llvm.c $(LDFLAGS) $(LIBS) -o bin/tuac'
#MACOSX_DEPLOYMENT_TARGET=14.0 clang $(CFLAGS) -DDEBUG src/main.c src/llvm.c src/debug.c  src/lexer.c src/parser.c src/compiler.c src/list.c $(LDFLAGS) $(LIBS) -o bin/tuac -lLLVM-C -L/usr/local/Cellar/llvm/19.1.6/lib -Wl,-search_paths_first -Wl,-headerpad_max_install_names
MACOSX_DEPLOYMENT_TARGET=14.0 clang $CFLAGS -DDEBUG \
    src/main.c src/llvm.c src/debug.c src/lexer.c src/parser.c src/compiler.c src/list.c \
    $LDFLAGS $LIBS -o bin/tuac

# ./bin/tuac examples/test5.tua
# llvm-gcc  -o bin/test5.bin bin/output.ll
