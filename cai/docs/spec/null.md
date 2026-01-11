# `null` 与可空语义规范

Status: Frozen

本文件定义 `null` 字面量、与可空类型的最小语义。

---

## 1. `null` 字面量
- `null` 表示“空引用”。
- `null` 只能赋值给以下类型：
  - `Ptr<T>`
  - `Ref<T>`（或 `&T`）
  - `any`

---

## 2. 非可空类型
以下类型 **不可** 为 `null`：
- `string`
- `bytes`
- 数组 `T[]` / `T[N]`
- `map<K,V>`
- 数值类型与 `bool`

表达“可能缺失”请使用 `Option<T>`（见 `cai/docs/spec/option.md`），表达“可能失败”请使用错误码（见 `cai/docs/spec/error.md`）。

---

## 3. 比较
- 允许：`x == null`、`x != null`（当 `x` 为可空类型）。
- 禁止：`null < x` 等有序比较。

---

## 4. 使用限制
为避免隐式 truthiness：
- `if x {}` 为编译错误（除非 `x: bool`）。
- 必须显式写 `x != null` 或使用 `Option`/错误码分支。
