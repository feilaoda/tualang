# AST 目录

本目录用于存放 CAI 编译器的 AST（抽象语法树）数据结构与构建逻辑。

Status: Draft

约定：
- parser 输出 AST；
- sema 以 AST 为输入，生成类型信息/符号表并进入 MIR 构建。

