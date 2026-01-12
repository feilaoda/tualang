# `std.net`（网络）规范

Status: Draft

本文件定义跨平台一致的网络 API：`TcpStream`、`TcpListener`、`UdpSocket` 与 `IpAddr`。

说明：`std.net` 可能受能力策略禁用（见 `cai/docs/spec/policy.md`）。语言不提供 `requires`；能力控制走 `cai.toml` 的 `[policy]` 与运行时强制。

错误码：所有可能失败的操作以 `..., int` 返回，错误码语义见 `cai/docs/spec/std_error.md` 与 `cai/docs/spec/error.md`。

导出约定：
- `std.net.Net` 是一个 `object`，用于承载 `std.net` 的“模块级 API”（避免导出顶层 `fn`）。

---

## 1. `IpAddr`

`std.net.IpAddr` 是带 payload 的 enum：
- `V4(u8, u8, u8, u8)`
- `V6(u16, u16, u16, u16, u16, u16, u16, u16)`

约束（冻结）：
- `IpAddr` 不支持 `==/!=`，也不可作为 `map` key（因为它是带 payload 的 enum；见 `cai/docs/spec/enum.md` 与 `cai/docs/spec/operators.md`）。
- 如需比较，使用标准库 API：
  - `std.net.Net.ipEqual(a: std.net.IpAddr, b: std.net.IpAddr) -> bool`

---

## 2. TCP

### 2.1 `TcpStream`
`std.net.TcpStream` 是拥有型句柄，默认 move-only。

最低要求 API：
- `std.net.Net.connect(addr: std.net.IpAddr, port: u16) -> std.net.TcpStream, int`
- `TcpStream.read(this, dst: Slice<byte>) -> usize, int`
- `TcpStream.write(this, src: Slice<byte>) -> usize, int`
- `TcpStream.shutdown(this) -> int`

异步要求：
- 必须提供异步版本（或同名 `async` 版本）：
  - `async fn TcpStream.readAsync(this, dst: Slice<byte>) -> usize, int`
  - `async fn TcpStream.writeAsync(this, src: Slice<byte>) -> usize, int`

### 2.2 `TcpListener`
`std.net.TcpListener` 是拥有型句柄，默认 move-only。

最低要求 API：
- `std.net.Net.listen(addr: std.net.IpAddr, port: u16) -> std.net.TcpListener, int`
- `TcpListener.accept(this) -> std.net.TcpStream, std.net.IpAddr, int`
- `async fn TcpListener.acceptAsync(this) -> std.net.TcpStream, std.net.IpAddr, int`

---

## 3. UDP

`std.net.UdpSocket` 是拥有型句柄，默认 move-only。

最低要求 API：
- `std.net.Net.bindUdp(addr: std.net.IpAddr, port: u16) -> std.net.UdpSocket, int`
- `UdpSocket.sendTo(this, dstAddr: std.net.IpAddr, dstPort: u16, src: Slice<byte>) -> usize, int`
- `UdpSocket.recvFrom(this, dst: Slice<byte>) -> usize, std.net.IpAddr, u16, int`

异步要求：
- `async fn UdpSocket.sendToAsync(...) -> usize, int`
- `async fn UdpSocket.recvFromAsync(...) -> usize, std.net.IpAddr, u16, int`

---

## 4. 超时与错误分类
当网络操作因超时失败时，必须返回：
- `std.error.Code.NET_TIMEOUT`（见 `cai/docs/spec/std_error.md`）

并保证：
- `std.error.Error.kind(err) == NetTimeout`。
