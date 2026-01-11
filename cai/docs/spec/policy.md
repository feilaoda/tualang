# 能力策略与审计（`cai.toml` / runtime policy）规范

Status: Frozen

本文件定义 CAI 的“能力（capability）”策略与审计机制，用于在不引入语言语法 `requires` 的前提下：
- 让外部效应（网络/文件写/进程等）可控、可审计；
- 让同一源码在不同部署环境下通过配置获得不同能力集合；
- 让标准库在能力被禁用时给出一致、可程序化处理的错误码与审计事件。

本文件冻结的是：能力集合、策略表达、强制点与审计事件的最低字段；不冻结具体配置文件命名之外的 CLI 形式。

---

## 1. 能力模型

### 1.1 能力枚举（冻结）
能力以字符串 ID 表示，集合冻结为：
- `net`：网络访问（含 TCP/UDP/DNS 等）
- `fs.read`：文件读取
- `fs.write`：文件写入（含创建/删除/修改）
- `process`：创建与控制子进程
- `ffi`：调用 `extern fn`

> `ffi` 被单独列出是为了允许“纯安全/纯沙箱”构建完全禁用外部 ABI 调用。

### 1.2 默认策略（冻结）
默认策略冻结为：
- `net = deny`
- `fs.read = allow`
- `fs.write = deny`
- `process = deny`
- `ffi = allow`

---

## 2. 策略表达（`cai.toml`）

策略通过 `cai.toml` 中的 `[policy]` 表达：

```toml
[policy]
net = "deny"
fs.read = "allow"
fs.write = "deny"
process = "deny"
ffi = "allow"
audit = "on"   # on|off
```

冻结约束：
- 未声明的能力使用默认策略（见 1.2）。
- 值域仅允许 `"allow"` / `"deny"`。
- `audit` 仅允许 `"on"` / `"off"`，默认 `"on"`。

---

## 3. 强制点（Enforcement Points）

### 3.1 标准库强制点（冻结）
当策略为 `deny` 时，以下操作必须失败并返回一致错误码：

- `net = deny`：
  - 任意 `std.net.*` 的网络 I/O（连接、accept、send/recv 等）必须失败。

- `fs.read = deny`：
  - `std.fs.open` 与任何读取相关操作必须失败。

- `fs.write = deny`：
  - `std.fs.create` 与任何写入/同步相关操作必须失败。

- `process = deny`：
  - `std.process.Command.spawn/status/output` 必须失败；
  - `Child.kill/send/wait/waitAsync` 必须失败（若 Child 句柄来自允许策略下创建则仍需按当前策略判断）。

- `ffi = deny`：
  - 任何 `extern fn` 调用必须失败或在编译期被禁止（两者任选其一，但必须全局一致）：
    - 若选择运行时强制：调用返回错误码；
    - 若选择编译期强制：对包含 `extern fn` 调用的程序报错。

统一错误码（冻结）：
- 所有因策略拒绝导致的失败，必须返回 `std.error.Code.PERMISSION_DENIED`（见 `cai/docs/spec/std_error.md`）。

### 3.2 模块可用性（可选但允许的行为）
工具链允许在 `net = deny` 时对 `import "std/net"` 直接报编译错误，以便更早发现；但不得替代 3.1 的运行时强制（即：即使允许编译通过，运行时也必须拒绝）。

---

## 4. 审计（Audit）

### 4.1 审计开关
当 `[policy].audit = "on"` 时，运行时必须对以下外部效应产生审计事件：
- 文件打开/创建
- 网络连接/监听/收发（若实现 `std.net`）
- 子进程创建与信号/终止
- `extern fn` 调用

当为 `"off"` 时，运行时可不产生审计事件。

### 4.2 审计事件格式（最低字段冻结）
审计事件以一行 JSON（JSONL）输出到 stderr 或一个由运行时配置指定的输出（输出介质不冻结，但默认 stderr）。

每条事件必须包含以下字段（字段名与类型冻结）：
- `tsNs: u64`：时间戳（纳秒；来源可为单调或系统时钟，不冻结）
- `event: string`：事件类型（见 4.3）
- `allowed: bool`：是否允许执行（策略判定结果）
- `code: int`：结果错误码（成功为 0；失败为非 0）
- `detail: map<string, any>`：附加信息（键集合不冻结，但必须为 JSON 可序列化的值）

### 4.3 事件类型（最低集合冻结）
`event` 的取值最低集合冻结为：
- `fs.open`
- `fs.create`
- `net.connect`
- `net.listen`
- `process.spawn`
- `process.signal`
- `ffi.call`

---

## 5. 与 `@attribute` 的关系（元数据，不影响强制）
CAI 允许使用 `@attribute` 记录可审计元数据（例如工具函数需要的能力），但策略强制不依赖语法：
- `@caps("net", "fs.read")`：声明该声明体的“预期能力集合”（仅用于工具链/审计/文档生成）。

约束：
- `@caps(...)` 不授予能力；运行时仍以 `[policy]` 为准。

