# `std.error`（错误码与错误分类）规范

Status: Frozen

本文件定义 CAI 标准库的错误码与跨平台错误分类机制。CAI 语言层的错误模型使用 `int` 错误码（见 `cai/docs/spec/error.md`），本文件冻结该 `int` 的“可移植解释”。

导出约定：
- `std.error.Error` 是一个 `object`，用于承载 `std.error` 的“模块级 API”（避免导出顶层 `fn`）。

---

## 1. 错误码值域
- 错误码类型为 `int`。
- `0` 表示成功。
- `>= 10000` 的错误码为**标准库冻结错误码**（见 2），在所有平台上语义一致。
- `1..9999` 为**实现/平台保留区**：
  - 允许运行时/系统调用把 OS 错误映射到该区间；
  - 但工具链必须保证：这些码可通过 `std.error.Error.kind(code)` 归类为稳定的 `Kind`（见 3）。

---

## 2. `std.error.Code`（冻结错误码）

`std.error.Code` 是一个 `enum`（无 payload），其判别值从 `10000` 起冻结：
- `IO_ERROR = 10000`
- `NET_TIMEOUT = 10001`
- `PERMISSION_DENIED = 10002`
- `PROCESS_ABORTED = 10003`
- `CANCELLED = 10004`

约束：
- 标准库/API 返回上述语义时，必须返回对应的固定值。
- 新增错误码只能追加在末尾（不得重排/复用数值）。

---

## 3. `std.error.Kind`（跨平台统合分类）

`Kind` 用于把不同平台/不同实现的错误码归一化到一个稳定分类，便于上层业务逻辑处理。

`std.error.Kind` 是一个 `enum`（无 payload），其成员集合冻结为：
- `Ok`
- `Io`
- `NetTimeout`
- `Permission`
- `ProcessAborted`
- `Cancelled`
- `Unknown`

---

## 4. 分类 API（最低要求）

标准库必须提供：
- `std.error.Error.kind(code: int) -> std.error.Kind`
- `int.kind() -> std.error.Kind`

语义冻结：
- `kind(0) == Ok`
- `kind(IO_ERROR) == Io`
- `kind(NET_TIMEOUT) == NetTimeout`
- `kind(PERMISSION_DENIED) == Permission`
- `kind(PROCESS_ABORTED) == ProcessAborted`
- `kind(CANCELLED) == Cancelled`
- 对 `1..9999` 的平台保留区错误码：
  - 必须映射到 `Io/NetTimeout/Permission/ProcessAborted/Unknown` 之一；
  - 映射规则由运行时实现，但对同一平台应保持稳定。
- 对其他值（负值/未定义值）：
  - 返回 `Unknown`。
