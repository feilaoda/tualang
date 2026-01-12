# FFI（`extern fn`）规范

Status: Frozen

本文件定义 CAI 的外部函数声明、ABI 约束与类型映射规则。

---

## 1. `extern fn` 语义
- `extern fn` 声明一个由外部链接提供实现的函数。
- `extern fn` 不含函数体。

批量声明语法：
- `extern { fn ...; fn ...; }` 是语法糖，等价于多条顶层 `extern fn` 声明（见 `cai/parser.g4` 与 `cai/docs/SYNTAX.md`）。

---

## 2. ABI
- 默认使用目标平台的 C ABI。
- 不同平台分别编译与链接（不承诺一次编译多次运行）。

---

## 3. 类型映射（最小集合）

本节中“FFI-safe”的定义见 `cai/docs/spec/abi.md` 的 1.1。

### 3.1 数值与 bool
- 按位宽映射到对应 C 整型/浮点类型（需要在实现中固定具体 C 类型，如 `int32_t/int64_t` 等）。

### 3.2 `string`
`string` 不允许出现在 `extern fn` 的参数/返回类型中（编译错误）。

FFI 传递 UTF-8 文本必须使用：
- `Slice<byte>`（只读 UTF-8 字节序列）

FFI 返回文本建议使用：
- `Ptr<byte>, usize, int`（返回 UTF-8 字节指针 + 长度 + 错误码，并在文档中声明其所有权与释放方式），或
- 由调用方提供输出缓冲（`Slice<byte>` 或 `Ptr<byte> + usize`），并返回写入长度与错误码。

### 3.3 指针
- FFI 指针参数/返回值统一使用 `Ptr<T>`（见 `cai/docs/spec/ptr.md`）。
- 禁止裸地址类型。

### 3.4 引用
- `Ref<T>` 不允许出现在 extern 签名中（编译错误）。需要可写输出时，使用 `Ptr<T>`（并在文档中声明写入规则与所有权）。

### 3.5 `Slice<T>`
`Slice<T>` 允许出现在 `extern fn` 的**参数类型**中，用于表达“指针 + 长度”的连续内存视图。

约束：
- `Slice<T>` 不允许出现在 `extern fn` 的返回类型中（编译错误）。
- `T` 必须是 FFI-safe 且“按位可搬运”的元素类型：
  - `T` 属于 `cai/docs/spec/abi.md` 的 FFI-safe 集合；
  - `T` 必须是 copy type；
  - `T` 不得包含 `deinit()` 语义或拥有型资源字段（避免把析构/所有权通过一段原始内存视图传播）。

只读/可写约束：
- `extern fn f(const s: Slice<T>)`：外部函数不得写入 `s` 指向的内存（语义约束；工具链可在生成头文件时映射为 `const T*`）。
- `extern fn f(mut s: Slice<T>)`：外部函数允许写入 `s` 指向的内存（语义约束；头文件可映射为 `T*`）。

### 3.6 `struct` / `enum`
- `struct`/`enum` 允许出现在 `extern fn` 的参数/返回类型中，当且仅当：
  - 其布局为 `repr(C)`（`struct` 默认成立；`enum` 为 tagged union，见 `cai/docs/spec/abi.md`）；
  - 其所有字段/payload 类型均为 FFI-safe；
  - 其语义不依赖 CAI 运行时资源管理（例如包含 `bytes/map/string/Box` 等拥有型资源）；
  - 不包含 `deinit()`（避免把析构责任跨语言边界隐式传播）。

### 3.7 `std.tensor.Tensor<T>`
`std.tensor.Tensor<T>` 允许出现在 `extern fn` 的参数/返回类型中，当且仅当：
- `T` 为数值类型或 `bool`；
- `Tensor<T>` 按 `cai/docs/spec/tensor.md` 的固定 ABI 描述符映射为等价的 `repr(C)` struct。

---

## 3.8 多返回映射
`extern fn` 的多返回按 `cai/docs/spec/abi.md` 的规则映射为“返回一个 `repr(C)` struct”。

---

## 4. 错误约定
沿用“多返回 + 错误码”：
- `extern fn f(...) -> T, int`
- `0` 表示成功，非 0 表示错误。

与 guard `? {}` 配合见 `cai/docs/spec/error.md`。
