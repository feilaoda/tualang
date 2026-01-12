# 函数（`fn`）规范

Status: Frozen

本文件定义 CAI 的函数、lambda、参数模式、返回、多返回、泛型、`extern fn` 与 `async fn` 的语义。

---

## 1. 声明与作用域

### 1.1 顶层函数
语法（见 `cai/parser.g4`）：
- `fn name(params...) -> returns { ... }`
- 返回类型糖：`fn name(params...) int { ... }`（仅允许 primitive return 的 sugar）

### 1.2 lambda（匿名函数）
语法：
- `fn (params...) -> returns { ... }`
- `async fn (...) { ... }`

lambda 是一等值，可赋给变量、作为参数传递、返回。

---

## 2. 参数模式（最关键的冻结点）

每个参数可选模式（语法支持）：`const | mut | move`。

### 2.1 默认模式
若省略参数模式：
- 默认等价于 `const`（只读借用语义）。
- 语义上参数默认**不发生所有权转移**；只有 `move` 参数才会转移所有权。

### 2.2 `const`
- 表示参数在函数体内只读：不可重新绑定、不可通过该参数写字段/元素（若其为引用/容器视图）。

### 2.3 `mut`
- 表示参数在函数体内可写：允许通过该参数写字段/元素（若该类型支持写入）。
- `mut` 参数要求调用点提供“独占可写借用”能力；借用冲突规则见 `cai/docs/spec/ownership.md` 与 `cai/docs/spec/ref.md`。

### 2.4 `move`
- 表示所有权转移：对该参数的传参会发生 move（所有权交给被调函数；调用后原绑定不可再用，见 `cai/docs/spec/ownership.md`）。
- `move` 参数在函数内成为 owner，可被返回或存入更长期结构。

---

## 3. 返回与多返回

### 3.1 单返回
- `return expr`
- 省略 `return` 的情况下，函数若声明了非 `void` 返回类型，则必须在所有路径返回（编译错误）。
- CAI 不支持“块末尾表达式作为返回值”的写法；必须显式 `return`。

### 3.2 多返回
语法：
- `fn f() -> T1, T2, ... { return v1, v2, ... }`
- 解构：`let a, b = f()`

约束：
- 返回值个数必须匹配声明。
- 多返回是显式语义，不做隐式 tuple。

---

## 4. 泛型

### 4.1 类型参数
语法：
- `fn id<T>(x: T) -> T { ... }`
- 调用：`id<int>(1)`

- 泛型采用单态化（monomorphization）。
- 泛型调用必须显式给出类型实参（例如 `id<int>(1)`），不提供自动类型实参推断。

### 4.2 约束（bounds）
语法：`fn f<T: Trait>(x: T) ...`
具体 trait 语义见 `cai/docs/spec/trait.md`。

---

## 5. `extern fn`（FFI）
语法：
- `extern fn name(params...) -> returns`（函数体缺省）

约束：
- 调用约定使用目标平台默认 C ABI。
- 指针互操作使用 `Ptr<T>`（见 `cai/docs/spec/ptr.md`），不允许裸地址类型。
- 所有权与释放责任必须在接口文档中明确；默认视为“借用，不接管释放”。

---

## 6. `async fn` 与 `await`
语法由 `cai/parser.g4` 定义。

语义：
- `async fn` 返回“可等待对象”（具体类型由标准库定义，例如 `Task<T>`）。
- `await expr` 只能出现在 `async` 上下文。
- `defer` 在 async 中的执行时机与普通作用域一致（离开当前作用域时执行）。
