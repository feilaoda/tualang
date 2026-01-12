# 绑定与赋值（`let/const`、解构）规范

Status: Frozen

本文件定义变量绑定、作用域、`const` 语义、赋值与解构规则。

---

## 1. 作用域
- CAI 采用块级作用域：`{ ... }` 创建新作用域。
- 内层作用域允许 shadow 外层同名绑定（可选给出 warning）。

---

## 2. 声明

### 2.1 `let`
- `let x: T = expr`
- `let x = expr`（类型从 `expr` 推断）

`let` 绑定可重新赋值：`x = expr`。

约束（冻结）：
- `let` 仅允许出现在 block 内（例如函数体、`if/for/while/match` 的 block）。
- 模块顶层不允许出现 `let` 声明（编译错误）。

### 2.2 `const`
- `const x: T = expr`
- `const x = expr`

`const` 绑定不可重新赋值。

对“通过该绑定写入底层对象”的限制：
- 若 `x` 是引用/借用（`Ref<T>`）或可变视图，`const x` 表示该引用本身不可被重新绑定，但是否允许通过它写入由 `Ref` 权限决定（见 `cai/docs/spec/ref.md`）。
- 对容器/struct 的“字段写/元素写”规则冻结为：通过 `const` 绑定不可对其底层值进行写入（字段赋值、数组元素赋值、map 写入均为编译错误），除非该写入发生在独占可写借用 `Ref<T>` 上。

约束（冻结）：
- `const` 仅允许出现在 block 内。
- 模块顶层不允许出现 `const` 声明（编译错误）。

---

## 3. 类型标注糖
- `name: T = expr` 等价于 `let name: T = expr`（语法糖；见 `cai/docs/SYNTAX.md`）。

约束（冻结）：
- 该语法糖仅允许出现在 block 内；模块顶层禁止。

---

## 4. 赋值
- `lhs = rhs`：
  - `lhs` 必须是可写 lvalue（变量、可写字段、可写索引位置）。
  - `rhs` 必须可转换到 `lhs` 类型（隐式规则见 `cai/docs/spec/types.md`）。

所有权规则（move/copy）由 `cai/docs/spec/ownership.md` 冻结。

---

## 5. 解构

### 5.1 解构声明
语法：
- `let a, b = expr`
- `let a: T1, b: T2 = expr`

语义：
- `expr` 必须产生多返回值（见 `cai/docs/spec/fn.md`），且数量匹配。
- 不允许“仅声明不初始化”的解构（例如 `let a, b` 为编译错误）。

### 5.2 解构赋值
语法：
- `a, b = expr`

语义：
- `a/b` 必须已声明且为可写绑定（非 `const`）。
- `expr` 必须产生多返回值，且数量匹配。
