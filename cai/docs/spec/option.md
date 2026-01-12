# `Option<T>` 规范

Status: Frozen

本文件定义 `Option<T>`：用于表达“可能缺失的值”，并与 `??`、解包 API 等配合。

---

## 1. 类型与构造
- `Option<T>` 是一个泛型命名类型。
- 其值域包含两种情况：
  - `Some(value: T)`
  - `None`

构造与判定 API（标准库/预导入约定）：
- `Some(v)`
- `None`
- `opt.isSome() -> bool`
- `opt.isNone() -> bool`

---

## 2. 与 `??`
- `opt ?? defaultValue`：
  - 当 `opt` 为 `None` 时结果为 `defaultValue`
  - 当 `opt` 为 `Some(v)` 时结果为 `v`
  - `defaultValue` 短路求值

详见 `cai/docs/spec/expressions.md`。

---

## 3. 模式匹配
CAI 不提供 `if let` 语法；模式匹配通过 `match` 完成（见 `cai/docs/spec/match.md`）。

对 `Option<T>`，`match` 支持以下 pattern：
- `Some(x)`：匹配 `Some(v)` 并绑定 `x: T`
- `None`：匹配 `None`

---

## 4. 解包
标准库约定：
- `opt.unwrap() -> T`：`None` 时运行时错误
- `opt.unwrapOr(v: T) -> T`
