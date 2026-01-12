# ABI 与数据布局规范

Status: Frozen

本文件冻结 CAI 的 ABI（调用约定层面的可观察行为）与数据布局（`sizeof/alignof/字段偏移`）规则，用于：
- 跨模块/跨库二进制交互的稳定性；
- FFI 的可定义映射（见 `cai/docs/spec/ffi.md`）；
- 递归/循环嵌套类型的可判定布局（见 4）。

CAI 不承诺“一次编译多次运行”；ABI/布局以目标平台的 C ABI 为基准（见 `cai/docs/spec/ffi.md`）。

---

## 1. 基本原则
- **确定性**：同一目标平台/同一编译选项下，布局必须确定。
- **C 对齐**：聚合类型默认与 C 兼容（`repr(C)`）。
- **禁止无穷大小**：任何可实例化类型的 `sizeof(T)` 必须在编译期可计算且有限。

---

## 1.1 FFI-safe（可用 C ABI 表达的类型）
本规范中“FFI-safe”用于限定哪些类型允许出现在：
- `extern fn` 签名（见 `cai/docs/spec/ffi.md`）；
- `@export` 导出函数签名（见 `cai/docs/spec/aot.md`）。

冻结为以下集合（最小但可实现）：
- 标量：
  - 所有数值类型
  - `bool`
  - `Ptr<T>`（指针本身；`T` 仅用于静态约束）
- 聚合：
  - `struct`：默认 `repr(C)`，且其字段类型均为 FFI-safe，并且不包含 `deinit()`，也不包含拥有型资源字段（如 `bytes/map/string/Box` 等）。
  - `enum`：tagged union（见 3），且其 payload 类型均为 FFI-safe，并且不包含拥有型资源语义（同上）。
  - `std.tensor.Tensor<T>`：按固定 ABI 描述符（见 `cai/docs/spec/tensor.md`）。

特殊规则：
- `Slice<T>` 仅允许作为 `extern fn` 的**参数类型**（见 `cai/docs/spec/ffi.md`）。
- 多返回会被 ABI 映射为返回一个 `repr(C)` struct（见 5），因此“多返回本身”在 ABI 上仍属于 FFI-safe 的聚合返回。

以下类型一律不是 FFI-safe（编译错误）：
- `Ref<T>`、`any`、`string`、`bytes`、`map`、`T[]`、`Box<T>`。

---

## 1.2 语言内部调用约定（非 `extern fn`）
本节冻结 CAI 语言内部调用（同一程序/同一工具链生成的代码之间）的最低 ABI 规则，用于避免“按值传参导致大量拷贝”，并与 `const/mut/move` 参数模式一致。

适用范围：
- 仅适用于 CAI 内部函数调用；
- 不适用于 `extern fn`（其 ABI 必须使用目标平台默认 C ABI；见 `cai/docs/spec/ffi.md`）。

### 1.2.1 标量参数
下列类型作为参数时按值传递（目标平台寄存器/栈细节由平台 ABI 决定）：
- 所有数值类型、`bool`
- `Ptr<T>`
- `Ref<T>`、`Slice<T>`（它们自身是小型描述符值；借用冲突仍由语义阶段检查）

### 1.2.2 聚合/拥有型参数
对下列类型 `T`，当形参写作 `p: T`（或 `mut p: T` / `move p: T`）时，语言内部调用必须按“指针传参”实现：
- 用户定义 `struct` / `enum`
- 拥有型标准库类型（例如 `string/bytes/map/Box` 等）

指针传参规则（规范性要求）：
- `fn f(p: T)`：传入 `const T*`（只读借用）
- `fn f(mut p: T)`：传入 `T*`（独占可写借用）
- `fn f(move p: T)`：传入 `T*`（所有权转移；callee 接管 drop/释放责任，caller 在调用后视为 moved-out）

实现说明（非规范性）：
- 编译器可选择在 callee 入口把 `T*` 指向的数据 move/copy 到 callee 的局部存储，以便优化；但不得改变上述可观察语义（借用/独占/所有权转移与 drop 责任）。

---

## 2. `struct` 布局（默认 `repr(C)`）

CAI 的 `struct` 默认 `repr(C)`，具体规则冻结为（等价于目标平台 C 编译器的 struct 布局）：
- 字段按**声明顺序**从前到后排列；
- 每个字段的起始偏移满足该字段的对齐要求；
- 必要时插入 padding；
- struct 的对齐为其字段对齐的最大值；
- struct 的大小为其最后一个字段末尾向上取整到 struct 对齐后的结果。

可见性（`private`）、`const` 字段、默认值不影响布局。

嵌入字段（`... OtherType`）在布局层等价于一个普通字段（其提升成员查找不影响布局）。

---

## 3. `enum` 布局（标记联合体 / tagged union）

CAI 的 `enum` 是标记联合体：一个 tag（判别值）+ 一个 payload 联合体。

### 3.1 判别值（tag）
- tag 类型冻结为 `int`（i32）。
- 若 enum 变体声明包含显式判别值 `= <int字面量>`，该值必须可表示为 `int`，否则编译错误。
- 未显式指定判别值的变体，其判别值按声明顺序从 `0` 递增（跳过已显式占用的值；若产生冲突为编译错误）。

### 3.2 payload（联合体）
每个变体可携带 payload（见 `cai/docs/spec/enum.md` 的语法与类型规则）。payload 的布局按如下冻结：
- 把每个变体的 payload 视为一个独立的 `repr(C)` struct（字段按 payload 顺序声明为 `_0/_1/...`）。
- enum 的 payload 区域是一个 C union，其大小为所有变体 payload struct 大小的最大值，对齐为其对齐的最大值。

### 3.3 enum 总体布局
enum 的整体布局冻结为：
1) `tag: int`
2) padding（使 payload 起始满足 payload 对齐）
3) `payload: union { ... }`

enum 的对齐为 `max(alignof(int), alignof(payload))`；
enum 的大小为末尾向上取整到该对齐后的结果。

---

## 4. 布局断开（Layout Breaking）与递归/循环嵌套

### 4.1 无穷布局禁止
若一个 `struct` 或带 payload 的 `enum` 在其“按值内联展开”的字段/成员中出现递归或循环嵌套，导致 `sizeof(T)` 无法终止计算，则为编译错误。

典型非法例子（无穷大小）：
- `struct Node { next: Node }`
- `enum List { Cons(int, List)\nNil }`

### 4.2 强制使用 `std.memory.Box<T>`
对递归/循环嵌套的数据结构，必须通过 `std.memory.Box<T>` 断开布局递归：
- `Box<T>` 的运行时表示仅包含一个堆内存指针（固定大小），因此能终止布局递归；
- 编译器在计算布局时，**不得**内联展开 `Box<T>` 的 `T`。

冻结约束：
- 若某个类型的递归仅通过 `Ptr<T>` 等“裸地址形态”断开，仍视为**不合法**；递归必须至少经过一次 `Box<...>`（否则编译错误）。

---

## 5. 多返回值 ABI（对标底层调用约定）

CAI 的“多返回”是语言层语义（见 `cai/docs/spec/fn.md`），其 ABI 降级规则冻结为：
- 对返回 `R1, R2, ... , Rn`（`n >= 2`）的函数，ABI 等价于返回一个 `repr(C)` struct：
  - 字段为 `_0: R1, _1: R2, ... _{n-1}: Rn`（按声明顺序）
  - 该 struct 的返回遵循目标平台 C ABI 的聚合返回规则（寄存器/隐式 sret 等由平台决定）。

该规则同时适用于：
- CAI 内部函数调用；
- `extern fn` 的类型映射（见 `cai/docs/spec/ffi.md`）；
- `@export` 导出（见 `cai/docs/spec/aot.md`）。

---

## 6. 稳定 ABI：`std.tensor` 描述符
标准库张量 `std.tensor` 必须提供固定 ABI 的描述符，用于跨库零拷贝传输：
- 描述符的字段集合、顺序、位宽与对齐必须冻结；
- 其定义与约束见 `cai/docs/spec/tensor.md`。
