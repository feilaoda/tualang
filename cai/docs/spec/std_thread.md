# `std.thread`（线程）规范

Status: Draft

本文件定义原生 OS 线程 API：创建、join、线程句柄语义。

错误码：所有可能失败的操作以 `..., int` 返回，错误码语义见 `cai/docs/spec/std_error.md` 与 `cai/docs/spec/error.md`。

---

## 1. `Thread`
`std.thread.Thread` 是拥有型句柄，默认 move-only。

导出约定：
- `std.thread.Thread` 的静态方法用于承载 `std.thread` 的“模块级 API”（避免导出顶层 `fn`）。

最低要求 API：
- `std.thread.Thread.spawn(f: fn() -> void) -> std.thread.Thread, int`
  - 语义：创建一个新的 OS 线程并在其中执行 `f`。
  - 失败返回非 0 错误码（例如资源不足）。

- `Thread.join(this) -> int`
  - 语义：阻塞等待线程结束；成功返回 0。

- `Thread.detach(this) -> int`
  - 语义：放弃 join 责任，让线程自行结束并由 OS 回收；成功返回 0。

约束：
- `Thread` 在 drop 时不得隐式 join（避免不可控阻塞）。
- 若 `Thread` 在 drop 时既未 `join` 也未 `detach`：为运行时错误。

---

## 2. 与运行时（M:N 调度）的关系
- `std.thread.Thread.spawn` 创建的是 OS 线程，不等价于 `async` 任务。
- `async` 任务由运行时调度（见 `cai/docs/spec/runtime.md`）；OS 线程可作为运行时 worker 或用户自管线程。
