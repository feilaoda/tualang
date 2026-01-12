# `std.time`（时间）规范

Status: Frozen

本文件定义时间相关的跨平台一致 API：`Duration`、`Instant` 与 `sleep`。

导出约定：
- `std.time.Time` 是一个 `object`，用于承载 `std.time` 的“模块级 API”（避免导出顶层 `fn`）。

---

## 1. `Duration`

`std.time.Duration` 必须是 FFI-safe 的 `struct`（见 `cai/docs/spec/abi.md`），字段冻结为：
- `nanos: long`（i64，单位：纳秒）

约束：
- `nanos` 允许为负值，用于表达差值。

---

## 2. `Instant`

`std.time.Instant` 表示单调时钟时间点（不受系统时间回拨影响）。

`Instant` 必须是 FFI-safe 的 `struct`，字段冻结为：
- `ticks: u64`

`ticks` 的绝对含义不对用户公开；仅允许通过 API 做差与比较。

最低要求 API：
- `std.time.Time.now() -> std.time.Instant`
- `std.time.Time.since(a: std.time.Instant, b: std.time.Instant) -> std.time.Duration`
  - 语义：返回 `a - b` 的差值（纳秒级）。

---

## 3. `sleep`

`std.time.sleep` 必须以异步形式提供，挂起当前任务而不阻塞调度线程（见 `cai/docs/spec/runtime.md`）：
- `async fn std.time.Time.sleep(d: std.time.Duration) -> int`

语义：
- 当 `d.nanos <= 0`：返回 `0`（不挂起）。
- 正常情况下：挂起当前任务，至少等待 `d` 时长后唤醒，并返回 `0`。
- 失败情况（例如运行时未初始化或被取消）返回非 0 错误码：
  - 取消必须返回 `std.error.Code.CANCELLED`（见 `cai/docs/spec/std_error.md` 与 `cai/docs/spec/async.md`）。
