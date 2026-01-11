# `unsafe` 规范

Status: Frozen

本文件定义 `unsafe { ... }`：语法形态，以及哪些操作必须处于 unsafe 上下文才能使用。

---

## 1. 语法
语法由 `cai/parser.g4` 定义：

```cai
unsafe {
  // statements...
}
```

`unsafe` 只能作用于一个 block。

---

## 2. 语义
- `unsafe` 不改变运行时语义；它只影响编译期允许/禁止的操作集合。
- `unsafe` 是“显式承诺”：表示该 block 内的某些操作无法由编译器证明安全性，调用者承担满足前置条件的责任。

---

## 3. 必须在 `unsafe` 中的操作
以下操作在 `unsafe` block 之外为编译错误：

### 3.1 FFI 调用
- 调用任意 `extern fn`。

### 3.2 指针内存访问
通过 `Ptr<T>` 进行的任何内存读写（由 `std.ptr` 提供），例如：
- `std.ptr.read<T>(p: Ptr<T>) -> T`
- `std.ptr.write<T>(p: Ptr<T>, v: T)`
- `std.ptr.copy<T>(dst: Ptr<T>, src: Ptr<T>, count: usize)`

---

## 4. unsafe 允许但不要求的操作
- 创建/传递/比较 `Ptr<T>`、`Ref<T>` 本身不要求 unsafe；
- 只要不发生内存访问、FFI 调用，就不需要 unsafe。

