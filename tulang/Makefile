LLVM_CONFIG = /usr/local/opt/llvm/bin/llvm-config

CFLAGS = $(shell $(LLVM_CONFIG) --cflags) -Wall
LDFLAGS = $(shell $(LLVM_CONFIG) --ldflags)
LIBS = $(shell $(LLVM_CONFIG) --libs core)
CC = clang

llvm: src/llvm.c
    $(CC) $(CFLAGS) $< $(LDFLAGS) $(LIBS) -o $@


