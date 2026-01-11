# `any`（动态值）规范

Status: Frozen

本文件定义 `any`：用于 map 等动态容器或与 AI/JSON 交互的动态值表示。

---

## 1. 值域
`any` 的最小值域冻结为：
- `null`
- `bool`
- 整数（统一为 `long`）
- 浮点（统一为 `double`）
- `string`
- `map<any, any>`
- `any[]`（动态数组）

---

## 2. 相等性
`any` 的 `==/!=` 行为冻结为：
- 若两侧类型不同，返回 `false`（不做数值提升/隐式转换）。
- 若两侧同类，按各自类型规则比较：
  - `string` 按内容
  - `map/array` 按引用身份比较（不做深比较）。

---

## 3. 类型判定与转换
标准库约定：
- `any.typeOf() -> string`（或 enum）
- `any.asInt()/asLong()/asString()` 等返回 `Option<T>`（失败为 `None`）

语言层面不提供 `any as T` 的强制 cast（编译错误），避免把动态错误变成隐式 UB。
