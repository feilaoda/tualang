# `enum` 规范

Status: Frozen

本文件定义 CAI 的 `enum`：变体、显式值、运行时布局与比较规则。

---

## 1. 声明与变体
语法见 `cai/parser.g4`：

```cai
enum Role {
  Admin
  User
}

enum Code {
  Ok = 0
  NotFound = 404
}

enum OptionInt {
  Some(int)
  None
}
```

变体规则：
- 变体名在 enum 内唯一。
- 允许对变体指定显式判别值：`Variant = INT_LIT | LONG_LIT`（约束见 2.1）。
- 变体可选携带 payload：
  - `Variant(T1, T2, ...)`
  - `Variant()` 不允许（空 payload 直接写 `Variant`）。

---

## 2. 底层表示
CAI 的 enum 为 **标记联合体（tagged union）**：
- `tag: int` 判别值（见 2.1）
- `payload: union`（可选；当所有变体都无 payload 时，payload 区域可被省略为实现细节）

布局细则由 `cai/docs/spec/abi.md` 冻结。

### 2.1 判别值（tag）
- tag 类型冻结为 `int`（i32）。
- 显式判别值必须可表示为 `int`；否则编译错误。
- 未显式指定者按声明顺序从 `0` 递增（冲突为编译错误）。

### 2.2 payload 类型规则
- payload 中的每个 `Ti` 都必须是可实例化类型（`sizeof(Ti)` 有限）。
- 若 payload 造成布局递归/无穷大小，为编译错误；递归结构必须通过 `std.memory.Box<T>` 断开（见 `cai/docs/spec/abi.md` 与 `cai/docs/spec/box.md`）。

---

## 3. 比较与哈希
对 enum 的比较与哈希规则冻结为：
- 仅当该 enum 为“无 payload 的 enum”（所有变体均无 payload）时：
  - 允许 `==/!=`（同 enum 类型），按 tag 值比较（见 `cai/docs/spec/operators.md`）。
  - 允许作为 `map` key：哈希基于其 tag 整数值（见 `cai/docs/spec/map.md`）。
- 若该 enum 存在任意带 payload 的变体：
  - 禁止 `==/!=`（编译错误）；
  - 禁止作为 `map` key（编译错误）。

对所有 enum：
- 不定义 `< <= > >=`（编译错误）；如需排序，使用显式 compare API（例如 `std.cmp`）。

---

## 4. 构造与访问（标准库约定）
构造一个带 payload 的变体使用“变体构造表达式”（语法由 `cai/parser.g4` 定义）：
- `E.Variant(args...)`

构造一个无 payload 的变体使用：
- `E.Variant`

约束：
- `args` 的数量与类型必须匹配该变体 payload 声明。
- 允许在 `E` 可唯一推断时省略 `E.` 前缀，直接写 `Variant(...)`。

payload 访问必须通过 `match` 解构：
- 不提供 `e.payload` 这类非穷尽式访问 API（避免遗漏变体）。
