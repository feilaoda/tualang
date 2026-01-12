# `std.runtime`（运行时接口）规范

Status: Frozen

本文件定义标准库对运行时的最小公开接口。

---

## 1. `yield`

`std.runtime.Runtime.yield` 用于协作式调度：显式让出一次执行权。

导出约定：
- `std.runtime.Runtime` 是一个 `object`，用于承载 `std.runtime` 的“模块级 API”（避免导出顶层 `fn`）。

签名冻结为：
- `async fn std.runtime.Runtime.yield() -> int`

语义：
- 挂起当前任务一次，允许调度器运行其他可运行任务；
- 随后当前任务被重新调度继续执行；
- 成功返回 `0`；
- 若运行时未初始化或任务被取消，返回非 0 错误码：
  - 取消必须返回 `std.error.Code.CANCELLED`（见 `cai/docs/spec/std_error.md`）。

设计说明（冻结）：
- `await` 只在显式等待点让出执行权；对于长时间 CPU 循环，`yield` 是不依赖 I/O 的、可预测的让出点。
- CAI 提供 `yield` 关键字作为“精确调度指示符”（语义见 `cai/docs/spec/async.md`）。
- `std.runtime.Runtime.yield()` 是底层运行时接口；关键字 `yield` 在语义上等价于一次让出（编译器可直接 lowering 为运行时调用）。
