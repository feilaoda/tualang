# `std.runtime`（运行时接口）规范

Status: Frozen

本文件定义标准库对运行时的最小公开接口。

---

## 1. `yield`

`std.runtime.yield` 用于协作式调度：显式让出一次执行权。

签名冻结为：
- `async fn std.runtime.yield() -> int`

语义：
- 挂起当前任务一次，允许调度器运行其他可运行任务；
- 随后当前任务被重新调度继续执行；
- 成功返回 `0`；
- 若运行时未初始化或任务被取消，返回非 0 错误码：
  - 取消必须返回 `std.error.Code.CANCELLED`（见 `cai/docs/spec/std_error.md`）。

