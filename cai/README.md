# CAI（规划中）

CAI 的目标是：**接近 C 的性能与可预测性**（AOT、无 GC、小运行时），同时把“调用/部署/约束 AI 能力”做成一等公民（LLM/Embedding/向量检索/工具调用/流式输出/可复现）。

本仓库当前已有一个实验性原型语言 **Tua**（见 `tua/`），CAI 计划在它验证过的分层与实现经验之上推进（LLVM 后端、无 GC、FFI、`tua_rt` 事件循环/IO、packages 体系等）。

- CAI 设计文档：`cai/docs/DESIGN.md`
- CAI 语法定义：`cai/docs/SYNTAX.md`、`cai/parser.g4`
- CAI 编译器管线：`cai/docs/COMPILER.md`
- “获得感”白皮书：`cai/docs/WHITEPAPER.md`
- 实现路线图：`cai/docs/ROADMAP.md`
- CAI 语言规范（按主题拆分）：`cai/docs/spec/README.md`
- Tua 语言与实现：`tua/README.md`、`tua/docs/SPEC.md`、`tua/docs/ROADMAP.md`
