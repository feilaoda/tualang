# 控制流规范

Status: Frozen

本文件定义 CAI 的语句与控制流：block、if、match、for、while、do-while、unsafe、break/continue/return、label/goto、defer。

---

## 1. block
- `{ ... }` 是一个 block 语句，并引入新作用域（见 `cai/docs/spec/bindings.md`）。
- block 内允许声明与语句混排。

---

## 2. `if` / `else`
语法见 `cai/parser.g4`。

约束：
- 条件表达式类型必须为 `bool`（不做 truthiness）。

---

## 3. `for`

### 3.1 C 风格 for
- `for init; cond; inc { ... }`

约束：
- `cond` 若省略，视为 `true`。
- `break`/`continue` 语义与 C 一致（见 6）。

### 3.2 for-in
- `for v in expr { ... }`
- `for k, v in expr { ... }`

约束：
- `expr` 必须是可迭代类型（map/array）；具体迭代语义见各类型规范。

---

## 4. `while` / `do-while`
- `while cond { ... }`
- `do { ... } while cond`

约束：
- `cond` 类型必须为 `bool`。

---

## 5. `return`
- `return` 可返回 0..n 个表达式（多返回规则见 `cai/docs/spec/fn.md`）。
- 若函数声明了返回类型，则所有控制路径必须返回匹配数量与类型的值（否则编译错误）。

---

## 6. `break` / `continue`
- `break`：跳出最近一层循环。
- `continue`：继续下一次迭代。

---

## 7. label 与 `goto`
语法：
- label：`::Name::`
- `goto Name`

约束：
- `goto` 只能跳转到同一函数内的 label。
- 禁止跳入一个尚未初始化的变量作用域（需要编译器做控制流验证）。

---

## 8. `defer`
语法：`defer statement`

语义：
- 在当前作用域退出时执行；
- 多个 defer 以 LIFO 顺序执行；
- 在 `return/break/continue/goto` 导致离开作用域时同样执行。

与 async 的交互见 `cai/docs/spec/async.md`。

---

## 9. `match`
语法与 pattern 规则见 `cai/docs/spec/match.md`。

---

## 10. `unsafe`
语法与限制规则见 `cai/docs/spec/unsafe.md`。
