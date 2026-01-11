# `@attribute` 注解与元数据规范

Status: Frozen

本文件定义 CAI 的注解系统：语法位置、参数形式、以及工具链可依赖的最小语义。

---

## 1. 语法位置
- `@attribute` 只能出现在 declaration 前（见 `cai/parser.g4`）。

---

## 2. 参数形式
语法：
- `@name`
- `@name(key = value, ...)`

规则：
- attribute 参数必须是编译期常量表达式（字面量与字面量组合）。
- 工具链（LSP/docgen/ai tool schema 提取）可在不执行程序的情况下读取这些元数据。

---

## 3. 保留注解名
为生态一致性，标准库与工具链保留一批标准注解名（名称与参数形态冻结）：
- `@tool(...)`：声明该函数可被模型作为工具调用
- `@timeout(ms=...)`
- `@idempotent`
- `@pure`
- `@deprecated(msg=...)`
- `@export`：导出顶层函数为 C ABI 符号（见 `cai/docs/spec/aot.md`）
- `@export_name(name="...")`：指定导出符号名（见 `cai/docs/spec/aot.md`）
- `@weak`：将导出符号标记为 weak（见 `cai/docs/spec/linking.md`）
- `@caps(...)`：声明该声明体的“预期能力集合”（不授予能力；见 `cai/docs/spec/policy.md`）
