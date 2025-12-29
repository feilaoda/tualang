# src/ 文件状态标注（tuac/tuaparse/legacy）

这个目录里同时存在三类代码：当前 LLVM 编译器（`tuac`）、调试用的解析器工具（`tuaparse`）、以及早期/实验性质的代码与生成物。下面按“仓库内现有构建入口是否引用”来标注，方便后续清理。

## 1) 当前正在使用（Makefile）

**`make tuac`（默认编译器/JIT）**
- `src/main.c`
- `src/compiler.c` `src/compiler.h`
- `src/lexer.c` `src/lexer.h`
- `src/parser.c` `src/parser.h`
- `src/list.c` `src/list.h`
- `src/debug.c` `src/debug.h`
- `src/error.h`
- `src/tua_map.c` `src/tua_map.h`
- `src/llvm/*`（`llvm.c/llvm_expr.c/llvm_call.c/...` + `llvm.h`）

**`make tuaparse`（仅解析/调试 AST）**
- `src/tuaparse.c`
- 以及上述 `lexer/parser/list/debug` 等公共文件

## 2) 仍可能使用：legacy VM（仓库脚本引用）

这些文件不参与 `make tuac`，但会被仓库根目录脚本 `debug.sh` / `release.sh` 直接用 `gcc ...` 编译成旧的 `tua` 可执行：
- `src/tua.c` `src/tua.h`
- `src/tvm.c` `src/tvm.h`
- `src/opcode.c` `src/opcode.h`
- `src/tvm_debug.c` `src/tvm_debug.h`
- `src/pool.c` `src/pool.h`

## 3) 当前未被任何构建入口引用（可视为“无用/待清理候选”）

说明：这里的“无用”是指 **没有被 Makefile 目标或仓库脚本引用**；如果你本地有额外脚本/流程用到它们，需要再确认。

**3.1 实验/遗留 parser/lexer（未引用）**
- `src/parser2.c`
- `src/build.sh`（lex/yacc + gcc 的 demo 构建脚本）
- `src/tua.l` `src/tua.y`（配套 lex/yacc 语法文件；当前仓库内无其它入口调用）

**3.2 另一套（c.*）语法与其生成物（未引用）**
- `src/c.l` `src/c.y` `src/c.h`
- `src/c.yy.c` `src/c.tab.c` `src/c.tab.h`
- `src/lex.yy.c` `src/y.tab.c` `src/y.tab.h` `src/y.output` `src/y.vcg`

**3.3 实验/产物（未引用）**
- `src/testllvm.c`
- `src/testc`（可执行文件产物）

**3.4 目前全仓库 grep 不到引用（未引用）**
- `src/object.h`（仅定义了 `gc_object/gc_string`，当前无任何 `.c` include）

