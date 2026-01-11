# `std.tensor`（稳定 ABI 张量）规范

Status: Frozen

本文件定义标准库张量视图 `std.tensor.Tensor<T>` 的 ABI 与最小语义，用于跨模块/跨库零拷贝传输。

目标：
- 描述符布局固定（stable ABI），不随编译器实现细节变化；
- 以“视图（view）”为核心，不隐式复制数据；
- 与 FFI/底层推理库互操作时，能够以“指针 + 形状/步长”表达。

---

## 1. 类型定义
`Tensor<T>` 是命名泛型类型（通常以全名 `std.tensor.Tensor<T>` 引用）。

`Tensor<T>` 是**视图类型**（不拥有数据），默认 copy type。

---

## 2. 固定 ABI 描述符（布局冻结）
`Tensor<T>` 的运行时布局冻结为一个 `repr(C)` 描述符，字段顺序与类型如下（对任意 `T` 一致）：
- `data: Ptr<T>`：指向第一个元素
- `ndim: u8`：维度数量
- `flags: u8`：位标志（见 2.2）
- `reserved: u16`：保留（必须为 0）
- `shape: Ptr<usize>`：长度为 `ndim` 的维度数组
- `strides: Ptr<isize>`：长度为 `ndim` 的步长数组（单位：元素）

布局规则：
- `Tensor<T>` 默认 `repr(C)`（见 `cai/docs/spec/abi.md`）。
- 对任意 `T1/T2`：`sizeof(Tensor<T1>) == sizeof(Tensor<T2>)` 且字段偏移一致（类型参数仅用于静态类型检查）。

### 2.1 有效性约束（运行时前置条件）
当 `ndim == 0`：
- `shape` 与 `strides` 允许为 `null`；
- 视为标量张量（0 维）。

当 `ndim > 0`：
- `shape != null` 且 `strides != null`；
- `shape[i]` 与 `strides[i]` 的读取必须在边界内（长度为 `ndim`）。

`data` 可为 `null` 仅当该张量的总元素数为 0（任一 `shape[i] == 0` 时总元素数为 0）。

### 2.2 `flags`
`flags` 的位语义冻结为：
- bit0：`READONLY`（1 表示只读视图；0 表示可写视图）
- bit1：`CONTIGUOUS`（1 表示连续存储；0 表示可能非连续）

其余 bit 保留，必须为 0。

---

## 3. 跨库零拷贝语义
跨模块/跨库传递 `Tensor<T>`：
- 仅传递描述符值本身（固定大小）；
- 不复制底层数据/shape/strides；
- 调用方与被调用方必须在契约层约定底层数据与元数据的所有权与生命周期（CAI 默认不接管释放）。

---

## 4. 与 FFI 的关系
`Tensor<T>` 是否允许出现在 `extern fn` 的参数/返回类型中，由 `cai/docs/spec/ffi.md` 冻结。

最低要求：
- 工具链必须能够把 `Tensor<T>` 的 C 头文件表示为一个等价的 `struct` 描述符（指针字段可映射为 `T*`/`const T*` 等）。

