LLVM_CONFIG ?= /usr/local/opt/llvm/bin/llvm-config

LLVM_CFLAGS = $(shell $(LLVM_CONFIG) --cflags)
LLVM_LDFLAGS = $(shell $(LLVM_CONFIG) --ldflags)
LLVM_LIBS = $(shell $(LLVM_CONFIG) --libs --system-libs)

CFLAGS = $(LLVM_CFLAGS) -I./src -Wall
LDFLAGS = $(LLVM_LDFLAGS)
LIBS = $(LLVM_LIBS)
CC = clang

.PHONY: tuac tuac-debug tuaparse

tuac:
	$(CC) $(CFLAGS) -O2 \
		src/main.c src/llvm/llvm.c src/llvm/llvm_var.c src/llvm/llvm_for.c src/llvm/llvm_if.c src/llvm/llvm_expr.c src/llvm/llvm_call.c \
		src/debug.c src/lexer.c src/parser.c src/compiler.c src/list.c \
		$(LDFLAGS) $(LIBS) -o bin/tuac

tuac-debug:
	$(CC) $(CFLAGS) -O0 -g -DDEBUG \
		src/main.c src/llvm/llvm.c src/llvm/llvm_var.c src/llvm/llvm_for.c src/llvm/llvm_if.c src/llvm/llvm_expr.c src/llvm/llvm_call.c \
		src/debug.c src/lexer.c src/parser.c src/compiler.c src/list.c \
		$(LDFLAGS) $(LIBS) -o bin/tuac

tuac-asan:
	$(CC) $(CFLAGS) -O0 -g -DDEBUG -fsanitize=address -fno-omit-frame-pointer \
		src/main.c src/llvm/llvm.c src/llvm/llvm_var.c src/llvm/llvm_for.c src/llvm/llvm_if.c src/llvm/llvm_expr.c src/llvm/llvm_call.c \
		src/debug.c src/lexer.c src/parser.c src/compiler.c src/list.c \
		$(LDFLAGS) $(LIBS) -o bin/tuac

tuaparse:
	$(CC) -I./src -Wall -O0 -g \
		src/tuaparse.c src/lexer.c src/parser.c src/list.c src/debug.c \
		-o bin/tuaparse
