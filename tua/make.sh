#!/bin/bash

LLVM_CONFIG=${LLVM_CONFIG:-/usr/local/opt/llvm/bin/llvm-config}
if [[ ! -x "$LLVM_CONFIG" ]]; then
  echo "LLVM_CONFIG not found: $LLVM_CONFIG" >&2
  exit 1
fi

CFLAGS="$($LLVM_CONFIG --cflags) -I./src"
LDFLAGS="$($LLVM_CONFIG --ldflags)"
LIBS="$($LLVM_CONFIG --libs --system-libs)"

MACOSX_DEPLOYMENT_TARGET=14.0 clang $CFLAGS -DDEBUG \
  src/main.c src/llvm/llvm.c src/llvm/llvm_var.c src/llvm/llvm_for.c src/llvm/llvm_if.c src/llvm/llvm_expr.c src/llvm/llvm_call.c \
  src/debug.c src/lexer.c src/parser.c src/compiler.c src/list.c \
  $LDFLAGS $LIBS -o bin/tuac

# ./bin/tuac examples/test5.tua
# llvm-gcc  -o bin/test5.bin bin/output.ll
