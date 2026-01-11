# `struct` 规范

Status: Frozen

本文件定义 CAI 的 `struct`：字段、方法、构造、`this`、可见性、嵌入字段（`...`）等。

---

## 1. 声明
语法见 `cai/parser.g4`：

```cai
struct Point {
  x: int = 0
  y: int = 0

  fn move(dx: int, dy: int) { this.x = this.x + dx }
}
```

---

## 2. 字段

### 2.1 字段声明
- `name: Type`
- 可选默认值：`name: Type = expr`
- `const name: Type = expr`：只读字段（写入为编译错误）。

### 2.2 初始化规则
struct 字段初始化规则冻结为：
- **不引入**“构造参数名自动映射字段名”的隐式规则；字段写入必须来自显式的 `S{ field: ... }` 或 `init(...)` 里的赋值。

当通过结构字面量 `S{ ... }` 创建 struct 时，对每个字段：
1) 若字面量显式提供该字段的值，则使用该值；
2) 否则若字段有默认值，则使用默认值；
3) 否则为编译错误（不得依赖“默认零值”隐式初始化）。

---

## 3. 方法与 `this`

### 3.1 方法声明
在 `struct` 体内声明的 `fn name(...) { ... }` 为实例方法。

### 3.2 `this`
- 在实例方法体内可用 `this`，其类型为当前 struct。
- `this.field` 访问字段；`this.method(...)` 调用方法。

### 3.3 方法接收者借用（冻结）
对调用 `x.method(args...)`：
- `x` 不会被 move；编译器必须把它视为一次“对 `x` 的借用”并检查借用冲突（见 `cai/docs/spec/ownership.md`）。
- 若方法体中对 `this`（或其字段/元素）存在写入，则该方法在语义上要求“独占可写借用”：
  - 调用点必须能为 `x` 提供独占借用（例如 `x` 为可写 lvalue 且不存在其他共享借用）。
- 若方法体中不对 `this` 发生写入，则该方法只要求“共享只读借用”。

对标准库/外部声明（无可分析方法体）的“读/写要求”，由对应 spec 冻结（例如 `bytes.mutSlice()`、`Box.get()` 等）。

### 3.3 可见性
- `private fn ...`：仅模块内可见。

---

## 4. 构造与析构

### 4.1 `init`
语法：
- `init() { ... }`

语义：
- `init` 是构造函数，负责把字段初始化到有效状态。
- `S(...)` 是构造调用：解析为调用 `S.init(...)` 并返回一个已初始化的 `S`。
- 构造调用与结构字面量的关系：
  - `S{ ... }` 不会隐式调用 `init`；
  - 需要运行构造逻辑时必须显式使用 `S(...)`。

### 4.2 `deinit`
语法：
- `deinit() { ... }`

语义：
- 在值生命周期结束时调用（作用域结束、覆盖赋值、return 等路径）。
- `deinit` 不得失败：不返回错误码；运行时错误会终止程序。

> 析构/所有权细则建议在 `cai/docs/spec/ownership.md` 冻结；本文件冻结的是 `deinit` 的存在与调用时机概念。

---

## 5. 结构字面量（named init suffix）
语法（解析规则见 `cai/parser.g4`）：
- `S{ a: expr, b: expr }`

约束：
- 不允许出现未知字段名（编译错误）。
- 字面量中指定的字段会按值/按 move 语义写入（由所有权规则决定）。

---

## 6. 嵌入字段（embedded field）
语法：
- `... OtherType`
- `... OtherType as Alias`（可选）

语义：
- 嵌入字段等价于一个匿名字段（或具名字段 Alias），并将其方法/字段提升到外层的成员查找路径（需要定义冲突规则）。
- 若产生同名冲突，必须编译错误或要求显式限定（`this.Alias.field`）。

---

## 7. 数据布局与递归约束
- `struct` 默认 `repr(C)`（见 `cai/docs/spec/abi.md`）。
- 若字段类型导致布局递归/无穷大小，为编译错误；递归结构必须通过 `std.memory.Box<T>` 断开（见 `cai/docs/spec/abi.md` 与 `cai/docs/spec/box.md`）。
