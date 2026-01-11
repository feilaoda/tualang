# 模式匹配（`match`）规范

Status: Frozen

本文件定义 CAI 的 `match` 语句：语法形态、pattern 规则、绑定规则与穷尽性检查。

---

## 1. 语法
语法由 `cai/parser.g4` 定义，形态如下：

```cai
match expr {
  Pattern1 => { ... }
  Pattern2 => { ... }
}
```

约束：
- 每个 arm 的 body 必须是 block：`{ ... }`。

---

## 2. 执行语义
- `expr` 先求值一次。
- arm 按出现顺序从上到下匹配；第一个匹配成功的 arm 执行其 block，并结束整个 `match`。
- `match` 是语句（statement），不产生表达式值。

---

## 3. 支持的 pattern

### 3.1 通配
- `_`：匹配任意值（wildcard）。

### 3.2 字面量 pattern
允许使用字面量作为 pattern（按 `==` 语义匹配）：
- `null`（仅当被匹配值类型为 `Ptr<T>`/`Ref<T>`/`any`）
- `true` / `false`
- 整数/浮点/字符串字面量

### 3.3 `enum` pattern
当被匹配表达式类型为某个 `enum E` 时：
- `E.Variant` 匹配该变体；
- `Variant` 也允许作为简写（在可唯一解析为该 enum 变体时）。

若该变体带 payload，则允许解构绑定：
- `E.Variant(x)` / `E.Variant(x, y, ...)`
- `Variant(x)` / `Variant(x, y, ...)`（同样要求可唯一解析）

绑定规则：
- `x/y/...` 为该 arm block 内的 `const` 绑定；
- 其类型为对应 payload 的 `Ti`。

约束：
- 对 `enum` 的变体，`Variant()`（空括号）为编译错误：
  - 无 payload 变体使用 `Variant`；
  - 有 payload 变体必须提供绑定名数量匹配其 payload arity。

### 3.4 `Option<T>` pattern
当被匹配表达式类型为 `Option<T>` 时：
- `Some(x)`：匹配 `Some(v)`，并在该 arm 的 block 内绑定 `x: T`；
- `None()`：匹配 `None`。

### 3.5 绑定 pattern
`name`（标识符）在以下情况下表示绑定 pattern：
- 被匹配类型不是 `enum`，或该名字无法解析为当前 enum 的变体名；
- 绑定 pattern 总是匹配成功，并在该 arm 的 block 内绑定 `name` 为被匹配值（类型为 `Type(expr)`）。

约束：
- 绑定变量为 `const`（只读绑定）。
- 若绑定 pattern 出现在非最后一个 arm，后续 arm 永远不可达，编译器必须报错或至少发出诊断。

---

## 4. 穷尽性（exhaustiveness）

CAI 对以下类型的 `match` 强制穷尽：
- `bool`
- `enum`
- `Option<T>`

规则：
- 必须覆盖所有可能值，否则编译错误。
- `_` 视为覆盖剩余所有情况。

对“无限域”的类型（例如整数、字符串、`any`）：
- 必须包含 `_` 或绑定 pattern 作为兜底 arm，否则编译错误。

---

## 5. 与控制流的交互
- arm 的 block 内可使用 `return/break/continue/goto/defer` 等语句，规则与普通 block 一致（见 `cai/docs/spec/controlflow.md`）。
