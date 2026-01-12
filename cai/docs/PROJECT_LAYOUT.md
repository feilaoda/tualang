# CAI 项目目录结构（冻结）

Status: Frozen

本文件定义 CAI 项目的目录结构与职责边界，目标是：代码清晰、可维护、便于并行开发与长期演进。

> 注意：语言语义规范在 `cai/docs/spec/`；本文件只定义“工程布局”。

---

## 1. 顶层目录（`cai/`）

- `cai/docs/`：所有文档（语法、规范、编译器管线、白皮书、工程布局等）。
  - `cai/docs/spec/`：语言语义规范（按主题拆分，每个文件自洽）。
  - `cai/docs/SYNTAX.md`：语法外观参考（引用 `cai/parser.g4`）。
- `cai/parser.g4`：ANTLR4 grammar（lexer+parser），供 tooling/IDE/测试使用；`caic` 可复用或实现独立解析器。
- `cai/compiler/`：`caic` 编译器（C 实现），目标管线见 `cai/docs/COMPILER.md`。
- `cai/tests/`：测试集合（语法、编译器、文档索引等），以 `python -m unittest` 驱动。

> 预留（未来加入时必须遵守本布局）：
> - `cai/runtime/`：运行时（协程调度、IO 抽象、GC(若有)、平台适配等），C 实现。
> - `cai/std/`：标准库源码（CAI 语言实现为主，必要时带少量 C shim）。
> - `cai/tools/`：开发工具（formatter、lsp、包管理、代码生成器等）。

---

## 2. 编译器目录（`cai/compiler/`）

`cai/compiler/` 只放 `caic` 源码与构建产物，不混入语言规范。

- `cai/compiler/include/cai/`：对外/对内公共头文件（driver、diag、lexer、parser、module、backend API 等）。
  - 头文件按功能分目录：`cai/compiler/include/cai/<area>/...`（例如 `cai/compiler/include/cai/diag/diag.h`）。
  - 同时保留少量“兼容 umbrella header”（例如 `cai/compiler/include/cai/diag.h`）以减少 include 路径迁移成本。
- `cai/compiler/src/`：实现代码（按功能拆分为多个小文件，避免超大单文件）。
  - `driver/`：CLI/driver（`check`、`emit-llvm` 等子命令）。
  - `diag/`：诊断系统（error/warn/span）。
  - `lexer/`：词法（tokenize）。
  - `parser/`：语法解析实现（按 common/type/expr/stmt/decl 拆分）。
  - `ast/`：AST 数据结构与构建（parser 输出；语义层输入）。
  - `sema/`：语义分析（name/type/borrow/error-model 校验等）。
  - `mir/`：MIR（中间表示）与相关 pass（实现阶段逐步填充）。
  - `llvm/`：LLVM 后端（将来会继续拆分多个 `llvm/*.c`）。
  - `module/`：模块级收集（目前用于 `emit-llvm` 采集函数签名）。
  - `util/`：文件读取、字符串、平台小工具。
- `cai/compiler/bin/`：可执行文件输出（`caic`）。
- `cai/compiler/build/`：中间产物（`.o`）。
- `cai/compiler/tests/`：C 白盒单元测试（`make -C cai/compiler test`）。

冻结约束：
- 单个 `.c` 文件应保持可读性；出现“巨大文件”时必须拆分。
- 新增功能必须配套单元测试（优先在 `cai/tests/compiler/`）。
