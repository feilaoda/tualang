# 可见性与 `private` 规范

Status: Frozen

本文件定义 CAI 的可见性规则。

---

## 1. 默认可见性
- 默认 public（可被其他模块导入）。
- `private` 显式标注为私有。

---

## 2. `private` 的作用域
`private` 仅允许修饰 declaration（见 `cai/parser.g4`）。

可见性边界：
- `private` 符号仅在同一模块内可见。
