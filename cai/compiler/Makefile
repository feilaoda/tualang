.PHONY: all clean
.PHONY: test

CC ?= clang
AR ?= ar
CFLAGS ?= -std=c17 -O0 -g -Wall -Wextra -Wpedantic -Werror

LLVM_CONFIG ?= /usr/local/Cellar/llvm/21.1.0/bin/llvm-config
LLVM_CFLAGS := $(shell $(LLVM_CONFIG) --cflags 2>/dev/null)
LLVM_LDFLAGS := $(shell $(LLVM_CONFIG) --ldflags --libs core --system-libs 2>/dev/null)

ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
INCLUDE := $(ROOT)/include
SRC := $(ROOT)/src
BUILD := $(ROOT)/build
BIN := $(ROOT)/bin

DRIVER_SOURCES := \
  $(SRC)/driver/main.c

CORE_SOURCES := \
  $(SRC)/diag/diag.c \
  $(SRC)/lexer/lexer.c \
  $(SRC)/llvm/llvm_backend.c \
  $(SRC)/module/module.c \
  $(SRC)/parser/parser.c \
  $(SRC)/parser/parser_common.c \
  $(SRC)/parser/parser_type.c \
  $(SRC)/parser/parser_expr.c \
  $(SRC)/parser/parser_stmt.c \
  $(SRC)/parser/parser_decl.c \
  $(SRC)/util/util.c

TEST_SOURCES := \
  $(ROOT)/tests/test_main.c \
  $(ROOT)/tests/test_lexer.c \
  $(ROOT)/tests/test_parser.c

CORE_OBJECTS := $(patsubst $(SRC)/%.c,$(BUILD)/%.o,$(CORE_SOURCES))
DRIVER_OBJECTS := $(patsubst $(SRC)/%.c,$(BUILD)/%.o,$(DRIVER_SOURCES))
TEST_OBJECTS := $(patsubst $(ROOT)/%.c,$(BUILD)/%.o,$(TEST_SOURCES))

all: $(BIN)/caic

$(BUILD)/libcaic.a: $(CORE_OBJECTS) | $(BUILD)
	$(AR) rcs $@ $(CORE_OBJECTS)

$(BIN)/caic: $(BUILD)/libcaic.a $(DRIVER_OBJECTS) | $(BIN)
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) -I$(INCLUDE) -I$(SRC) -o $@ $(DRIVER_OBJECTS) $(BUILD)/libcaic.a $(LLVM_LDFLAGS)

$(BUILD)/%.o: $(SRC)/%.c | $(BUILD)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) -I$(INCLUDE) -I$(SRC) -c -o $@ $<

$(BUILD)/%.o: $(ROOT)/%.c | $(BUILD)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) -I$(INCLUDE) -I$(SRC) -c -o $@ $<

$(BUILD):
	mkdir -p $(BUILD)

$(BIN):
	mkdir -p $(BIN)

$(BIN)/caic_tests: $(BUILD)/libcaic.a $(TEST_OBJECTS) | $(BIN)
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) -I$(INCLUDE) -I$(SRC) -o $@ $(TEST_OBJECTS) $(BUILD)/libcaic.a $(LLVM_LDFLAGS)

test: $(BIN)/caic_tests
	$(BIN)/caic_tests

clean:
	rm -rf $(BUILD) $(BIN)
