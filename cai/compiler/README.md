# CAI 编译器（`caic`）— C 语言实现（WIP）

当前阶段：仅实现前端（读取源文件 → 词法 → 语法检查 → 诊断输出）。

## 构建
- `make -C cai/compiler`
- 产物：`cai/compiler/bin/caic`

## 使用
- `cai/compiler/bin/caic check path/to/file.ai`
- `cai/compiler/bin/caic emit-llvm path/to/file.ai [-o out.ll]`

## 说明
- 未来管线目标：C 前端 → MIR → LLVM IR → 链接/产物（见 `cai/docs/TODO.md`）。
- 本目录不依赖网络与第三方库；LLVM 集成需要本机安装 LLVM 开发环境（例如 `llvm-config`）。

## 代码结构（约定）
- `src/driver/`：CLI/driver
- `src/lexer/`：词法
- `src/parser/`：语法解析（common/type/expr/stmt/decl）
- `src/ast/`：AST（预留）
- `src/sema/`：语义分析（预留）
- `src/module/`：模块信息收集（原型阶段用于函数签名）
- `src/llvm/`：LLVM IR 生成（原型阶段；将来继续拆分）
- `src/mir/`：MIR（预留）

## 测试
- `make -C cai/compiler test`（C 白盒单测 runner）

工程层级的目录结构冻结见 `cai/docs/PROJECT_LAYOUT.md`。
