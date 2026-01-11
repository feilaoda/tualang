# 所有权、move、drop 与借用生命周期

Status: Frozen

本文件定义 CAI 在“无 GC”前提下的内存与资源管理语义：哪些类型是 copy/move-only、drop 时机、借用生命周期与冲突规则。

---

## 1. Copy vs Move-only

### 1.1 Copy types
默认可复制（赋值/传参不转移所有权）：
- 所有数值类型
- `bool`
- `Ptr<T>`（按地址值复制）
- `Ref<T>`（按引用值复制；但借用冲突由借用检查约束）
- `Slice<T>`（按视图值复制；借用冲突由借用检查约束，见 `cai/docs/spec/slice.md`）
- `std.tensor.Tensor<T>`（按描述符值复制；布局见 `cai/docs/spec/tensor.md`）
- `string`（共享/引用计数或等价实现；语义上按值复制）
- `enum`：当且仅当其所有变体均无 payload，或 payload 中的所有元素类型均为 copy type 时，该 enum 为 copy type；否则为 move-only

### 1.2 Move-only types
默认 move-only（赋值/传参会转移所有权，源变量变为“不可用”）：
- `struct`（除非显式声明为可复制类型；CAI 不提供隐式 copyable 推断）
- `map<K,V>`
- 动态数组 `T[]`
- `bytes`
- 任务/异步类型（例如 `Task<T>`，若标准库提供）

---

## 2. move 语义
- `let b = a`：
  - 若 `a` 是 copy type：复制
  - 若 `a` 是 move-only：转移所有权，`a` 之后不可再使用（编译错误）
- `f(a)` 传参遵循同样规则（除非参数是借用/引用类型或显式 `move` 参数；见 `cai/docs/spec/fn.md`）。

显式 `move a` 语义见 `cai/docs/spec/expressions.md`。

---

## 3. drop/析构
规则：
- 对 move-only/拥有型值，在以下时机自动 drop：
  - 作用域结束
  - 被新值覆盖赋值
  - `return` 路径离开作用域
- 若 `struct` 定义了 `deinit()`，则 drop 时调用（见 `cai/docs/spec/struct.md`）。

---

## 4. 借用生命周期
规则：
- 借用至少存活到“最后一次使用所在语句结束”。
- 编译器可做 NLL（非词法生命周期）缩短，但必须保持语义等价。

借用冲突规则见 `cai/docs/spec/ref.md`；容器与借用的交互规则见 `cai/docs/spec/array.md` 与 `cai/docs/spec/map.md`。

---

## 5. 防止悬垂引用
必须保证：
- 不允许返回指向局部变量的 `Ref<T>`（编译错误）。
- 不允许返回指向局部变量的 `Slice<T>`（编译错误）。
- `Ref<T>` 不可逃逸：禁止存入 struct 字段/全局变量/闭包环境、禁止跨 `await` 存活（见 `cai/docs/spec/async.md`）。
- “借用 slice”（例如由 `bytes.slice()`/`bytes.mutSlice()` 创建的 `Slice<T>`）不可逃逸：禁止存入 struct 字段/全局变量/闭包环境、禁止跨 `await` 存活。
- `embed` 创建的 `Slice<byte>` 具有 `static` 生命周期，允许逃逸（见 `cai/docs/spec/embed.md`）。

---

## 6. `null` 与 move-only slot
规则：
- move-only 变量在被 move 后，逻辑上变为“无值”；再次使用为编译错误。
- 运行时可将其底层存储置为 `null`/0 以降低 UAF 风险，但这是实现细节，不可被程序观察为一个“值”。
