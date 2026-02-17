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

## 2) 当前未被 `Makefile` 目标引用（可视为“待清理候选”）

说明：这里的“无用”是指 **没有被 Makefile 目标或仓库脚本引用**；如果你本地有额外脚本/流程用到它们，需要再确认。

当前仓库里仍存在，但 `make tuac` / `make tuaparse` 都不会编译/链接它们：
- `src/opcode.c`
- `src/opcode.h`
