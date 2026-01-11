# `impl` 规范

Status: Frozen

本文件定义 CAI 的 `impl`：为类型添加方法、以及为类型实现 trait。

---

## 1. inherent impl
语法：
```cai
impl Point {
  fn move(dx: int, dy: int) { this.x = this.x + dx }
}
```

语义：
- `impl Type { ... }` 中声明的函数成为 `Type` 的方法集合的一部分。
- 方法体内可用 `this`（语义同 struct 内的方法；见 `cai/docs/spec/struct.md`）。

冲突规则：
- 同一类型的同名方法不得重复定义（编译错误）。

---

## 2. trait impl
语法：
```cai
impl HasId for Point {
  fn id() -> int { return this.x }
}
```

语义：
- 为 `Type` 提供 `Trait` 的方法实现，使其满足 `T: Trait`。

一致性（coherence）规则：
- 只有当 `Trait` 或 `Type` 至少一个在当前编译单元/包内定义时，才允许写该 `impl`（避免全局冲突；类似 orphan rule）。
- 若存在多个满足同一 `T: Trait` 的实现，编译错误。

---

## 3. 方法解析顺序
当解析 `x.method(...)`：
1) `Type(x)` 的 inherent 方法
2) `Type(x)` 满足的 trait 方法（若存在多个候选，要求显式限定或编译错误）
3) 不存在动态 trait object；trait 仅支持静态分发（见 `cai/docs/spec/trait.md`）
