LLVM_CONFIG ?= /usr/local/opt/llvm/bin/llvm-config

LLVM_CFLAGS = $(shell $(LLVM_CONFIG) --cflags)
LLVM_LDFLAGS = $(shell $(LLVM_CONFIG) --ldflags)
LLVM_LIBS = $(shell $(LLVM_CONFIG) --libs --system-libs)

CFLAGS = $(LLVM_CFLAGS) -I./src -Wall
LDFLAGS = $(LLVM_LDFLAGS)
LIBS = $(LLVM_LIBS)
CC = clang
AR ?= ar

UNAME_S := $(shell uname -s)

# Optional ggml (llama.cpp) acceleration for GGUF quant kernels.
# Enable automatically when llama.cpp is installed via Homebrew (or when GGML_PREFIX is set).
GGML_PREFIX ?= /usr/local/opt/llama.cpp
ifneq ("$(wildcard $(GGML_PREFIX)/include/ggml.h)","")
	CFLAGS += -DTUA_LLM_USE_GGML=1 -I$(GGML_PREFIX)/include
	LIBS += -L$(GGML_PREFIX)/lib -lggml-base -lggml-cpu
endif

ifeq ($(UNAME_S),Darwin)
	RT_LOOP_SRC = src/rt/platform/posix/rt_loop_kqueue.c
	RT_DEFS = -DTUA_OS_DARWIN=1 -DTUA_OS_POSIX=1 -DTUA_RT_LOOP_BACKEND_KQUEUE=1
	# JITed Tua code resolves runtime symbols from the host process; force-load the archive.
	RT_LINK = -Wl,-force_load,bin/libtuart.a
	RT_EXTRA_LIBS = -liconv
	TUAC_EXTRA_LIBS = -framework Accelerate $(RT_EXTRA_LIBS)
else ifeq ($(UNAME_S),Linux)
	RT_LOOP_SRC = src/rt/platform/posix/rt_loop_epoll.c
	RT_DEFS = -DTUA_OS_LINUX=1 -DTUA_OS_POSIX=1 -DTUA_RT_LOOP_BACKEND_EPOLL=1
	RT_LINK = -Wl,--whole-archive bin/libtuart.a -Wl,--no-whole-archive
	RT_EXTRA_LIBS =
	TUAC_EXTRA_LIBS = -ldl $(RT_EXTRA_LIBS)
else
	RT_LOOP_SRC = src/rt/platform/posix/rt_loop_poll.c
	RT_DEFS = -DTUA_OS_POSIX=1 -DTUA_RT_LOOP_BACKEND_POLL=1
	RT_LINK = bin/libtuart.a
	RT_EXTRA_LIBS =
	TUAC_EXTRA_LIBS = $(RT_EXTRA_LIBS)
endif

RT_CFLAGS = -I./src -Wall $(RT_DEFS)
RT_SRCS = \
	src/tua_map.c \
	src/tua_array.c \
	src/tua_bytes.c \
	src/tua_str.c \
	src/rt/rt_alloc.c \
	src/rt/rt_box.c \
	src/rt/rt_cancel.c \
	src/rt/rt_err.c \
	src/rt/rt_io.c \
	src/rt/rt_strconv.c \
	src/rt/platform/posix/rt_fs_posix.c \
	src/rt/rt_workqueue.c \
	src/rt/rt_time.c \
	src/rt/platform/posix/rt_thread_posix.c \
	src/rt/platform/posix/rt_fs_async_posix.c \
	src/rt/platform/posix/rt_net_posix.c \
	src/rt/platform/posix/rt_tua_bind_posix.c \
	$(RT_LOOP_SRC)
RT_OBJS = $(RT_SRCS:%.c=bin/obj/%.o)

.PHONY: tuac tuac-debug tuaparse librt rt-smoke rt-smoke-run rt-fs-smoke rt-fs-smoke-run rt-fs2-smoke rt-fs2-smoke-run rt-net-smoke rt-net-smoke-run
.PHONY: test

tuac: bin/libtuart.a
		$(CC) $(CFLAGS) -O2 \
			src/main.c src/llvm/llvm.c src/llvm/llvm_var.c src/llvm/llvm_for.c src/llvm/llvm_while.c src/llvm/llvm_loop.c src/llvm/llvm_goto.c src/llvm/llvm_if.c src/llvm/llvm_expr.c src/llvm/llvm_call.c \
			src/debug.c src/lexer.c src/parser.c src/compiler.c src/analyzer.c src/list.c \
			$(RT_LINK) -pthread $(LDFLAGS) $(LIBS) $(TUAC_EXTRA_LIBS) -o bin/tuac

tuac-debug: bin/libtuart.a
		$(CC) $(CFLAGS) -O0 -g -DDEBUG \
			src/main.c src/llvm/llvm.c src/llvm/llvm_var.c src/llvm/llvm_for.c src/llvm/llvm_while.c src/llvm/llvm_loop.c src/llvm/llvm_goto.c src/llvm/llvm_if.c src/llvm/llvm_expr.c src/llvm/llvm_call.c \
			src/debug.c src/lexer.c src/parser.c src/compiler.c src/analyzer.c src/list.c \
			$(RT_LINK) -pthread $(LDFLAGS) $(LIBS) $(TUAC_EXTRA_LIBS) -o bin/tuac

tuac-asan:
		$(CC) $(CFLAGS) -O0 -g -DDEBUG -fsanitize=address -fno-omit-frame-pointer \
			src/main.c src/llvm/llvm.c src/llvm/llvm_var.c src/llvm/llvm_for.c src/llvm/llvm_while.c src/llvm/llvm_loop.c src/llvm/llvm_goto.c src/llvm/llvm_if.c src/llvm/llvm_expr.c src/llvm/llvm_call.c \
			src/debug.c src/lexer.c src/parser.c src/compiler.c src/analyzer.c src/list.c \
			$(RT_LINK) -pthread $(LDFLAGS) $(LIBS) $(TUAC_EXTRA_LIBS) -o bin/tuac

tuaparse:
	$(CC) -I./src -Wall -O0 -g \
		src/tuaparse.c src/lexer.c src/parser.c src/list.c src/debug.c \
		-o bin/tuaparse

bin/obj/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(RT_CFLAGS) -c $< -o $@

bin/libtuart.a: $(RT_OBJS) Makefile
	@mkdir -p bin
	@rm -f $@
	$(AR) rcs $@ $(RT_OBJS)

librt: bin/libtuart.a

bin/rt-smoke: bin/libtuart.a tests/rt_smoke.c
	$(CC) $(RT_CFLAGS) tests/rt_smoke.c bin/libtuart.a -pthread $(RT_EXTRA_LIBS) -o $@

rt-smoke: bin/rt-smoke

rt-smoke-run: rt-smoke
	./bin/rt-smoke

bin/rt-fs-smoke: bin/libtuart.a tests/rt_fs_async_smoke.c
	$(CC) $(RT_CFLAGS) tests/rt_fs_async_smoke.c bin/libtuart.a -pthread $(RT_EXTRA_LIBS) -o $@

rt-fs-smoke: bin/rt-fs-smoke

rt-fs-smoke-run: rt-fs-smoke
	./bin/rt-fs-smoke

bin/rt-fs2-smoke: bin/libtuart.a tests/rt_fs_stat_readdir_smoke.c
	$(CC) $(RT_CFLAGS) tests/rt_fs_stat_readdir_smoke.c bin/libtuart.a -pthread $(RT_EXTRA_LIBS) -o $@

rt-fs2-smoke: bin/rt-fs2-smoke

rt-fs2-smoke-run: rt-fs2-smoke
	./bin/rt-fs2-smoke

bin/rt-net-smoke: bin/libtuart.a tests/rt_net_smoke.c
	$(CC) $(RT_CFLAGS) tests/rt_net_smoke.c bin/libtuart.a -pthread $(RT_EXTRA_LIBS) -o $@

rt-net-smoke: bin/rt-net-smoke

rt-net-smoke-run: rt-net-smoke
	./bin/rt-net-smoke

test:
	./tests/run.sh
	./packages/run_tests.sh
