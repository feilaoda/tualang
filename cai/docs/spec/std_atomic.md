# `std.atomic`（原子）规范

Status: Draft

本文件定义 `std.atomic.Atomic<T>`：映射到 CPU 级原子指令的并发原语。

---

## 1. `Atomic<T>` 允许的 `T`
`Atomic<T>` 仅允许以下 `T`（否则编译错误）：
- 整数类型（`i8/i16/int/long/isize/u8/u16/u32/u64/usize/byte`）
- `bool`
- `Ptr<U>`

---

## 2. 内存序（最小冻结）
为保持实现简单且可预测，`std.atomic` 的默认内存序冻结为：
- 所有原子操作为 **顺序一致（seq_cst）**。

不提供更弱的内存序 API（例如 relaxed/acquire/release）；如将来需要扩展必须走版本升级。

---

## 3. 最低要求 API

`Atomic<T>` 是一个 `struct`（实现细节不冻结），但其行为必须满足以下 API 与语义：

- `std.atomic.Atomic.new(v: T) -> std.atomic.Atomic<T>`
- `Atomic.load(this) -> T`
- `Atomic.store(this, v: T)`
- `Atomic.swap(this, v: T) -> T`
- `Atomic.compareExchange(this, expected: T, desired: T) -> T, bool`
  - 语义：若当前值等于 `expected`，则写入 `desired` 并返回 `expected, true`；
  - 否则不写入，返回当前值与 `false`。

---

## 4. 与底层指令的关系（可观察语义）
实现应当映射到目标平台可用的原子指令（例如 x86 的 `LOCK CMPXCHG` 等），但这是性能要求，不改变上述可观察语义。
