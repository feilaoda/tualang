# `std.process`（进程）规范

Status: Frozen

本文件定义跨平台一致的进程创建与管理 API：`Command` 建造者、I/O 重定向与退出状态。

错误码：所有可能失败的操作以 `..., int` 返回，错误码语义见 `cai/docs/spec/std_error.md` 与 `cai/docs/spec/error.md`。

---

## 1. `Command`

`std.process.Command` 是 move-only 类型，用于构造一个将要执行的子进程。

最低要求 API（建造者模式；链式调用可由语言方法调用语法表达）：
- `std.process.Command.new(program: string) -> std.process.Command`
- `Command.arg(this, a: string) -> std.process.Command`
- `Command.args(this, a: Slice<string>) -> std.process.Command`
- `Command.env(this, key: string, value: string) -> std.process.Command`
- `Command.cwd(this, dir: string) -> std.process.Command`

I/O 重定向：
- `Command.stdinNull(this) -> std.process.Command`
- `Command.stdoutNull(this) -> std.process.Command`
- `Command.stderrNull(this) -> std.process.Command`
- `Command.stdoutPipe(this) -> std.process.Command`
- `Command.stderrPipe(this) -> std.process.Command`

执行：
- `Command.spawn(this) -> std.process.Child, int`
- `Command.status(this) -> std.process.ExitStatus, int`
- `Command.output(this) -> std.process.Output, int`

约束：
- `program/args/env` 的编码为 UTF-8 `string`；平台转换由标准库处理。

---

## 2. `Child`（子进程句柄）

`std.process.Child` 是拥有型句柄，默认 move-only。

最低要求 API：
- `Child.wait(this) -> std.process.ExitStatus, int`
- `async fn Child.waitAsync(this) -> std.process.ExitStatus, int`
- `Child.kill(this) -> int`

若启用了 `stdoutPipe/stderrPipe`：
- `Child.stdout(this) -> std.fs.File`（读取端）
- `Child.stderr(this) -> std.fs.File`

约束：
- `waitAsync` 必须挂起当前任务而不阻塞调度线程（见 `cai/docs/spec/runtime.md`）。实现可使用 I/O 事件机制或阻塞线程池，但对用户语义等价。

---

## 3. `ExitStatus` / `Output`

`ExitStatus` 必须是 FFI-safe 的 `struct`，字段冻结为：
- `code: int`（进程退出码；若被信号终止，`code` 为平台映射值）
- `signaled: bool`（是否由信号/等价机制终止）

`Output` 是 move-only 类型，字段冻结为：
- `status: std.process.ExitStatus`
- `stdout: bytes`
- `stderr: bytes`

---

## 4. 信号（跨平台统一）

标准库必须提供一个跨平台“统一信号枚举”，用于描述进程终止/中断等事件。

最低要求：
- `std.process.Signal`：无 payload enum，至少包含 `Term`、`Kill`、`Int`
- `Child.send(this, sig: std.process.Signal) -> int`

平台差异的映射由标准库实现，但必须保证：
- `Term/Int` 在支持的系统上尽量映射为温和终止；
- `Kill` 为强制终止。
