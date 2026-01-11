# `std.memory.Box<T>` 规范

Status: Frozen

本文件定义 `std.memory.Box<T>`：用于在堆上分配 `T` 的拥有型容器，也是 CAI 用于“断开布局递归”的唯一允许方式（见 `cai/docs/spec/abi.md`）。

---

## 1. 类型与布局
`Box<T>` 是命名泛型类型（通常以全名 `std.memory.Box<T>` 引用）。

运行时表示冻结为：
- `ptr: Ptr<T>`（指向堆上分配的 `T`）

并且：
- `sizeof(Box<T>) == sizeof(Ptr<T>)`
- `alignof(Box<T>) == alignof(Ptr<T>)`

`Box<T>` 本身不可为 `null`；表达“可能没有 box”必须使用 `Option<Box<T>>`。

---

## 2. 所有权与 drop
- `Box<T>` 是 move-only（见 `cai/docs/spec/ownership.md`）。
- 当 `Box<T>` 被 drop 时：
  1) 先 drop 其堆上 `T`（若 `T` 定义了 `deinit()`，则调用之）；
  2) 再释放该堆内存。

内存分配失败（OOM）为运行时错误（见 `cai/docs/spec/error.md`）。

---

## 3. 构造与访问（标准库最低 API）

### 3.1 构造
- `std.memory.Box.new<T>(v: T) -> Box<T>`

语义：
- 在堆上分配并 move `v` 到该内存；
- 返回拥有该内存的 `Box<T>`。

### 3.2 访问
`Box<T>` 提供对内部 `T` 的借用访问（不暴露裸地址）：
- `Box.get(this) -> Ref<T>`

权限规则冻结为：
- `get()` 返回的 `Ref<T>` 权限与 `this` 的借用权限一致：
  - 若 `this` 在当前上下文为共享只读借用，则返回共享只读 `Ref<T>`；
  - 若 `this` 在当前上下文为独占可写借用，则返回独占可写 `Ref<T>`。

> “借用权限”规则由 `cai/docs/spec/ref.md` 与 `cai/docs/spec/ownership.md` 冻结；本文件冻结 Box 访问不会泄露裸指针，且权限不允许升级。

---

## 4. FFI
`Box<T>` 不允许出现在 `extern fn` 的参数/返回类型中（编译错误）。

