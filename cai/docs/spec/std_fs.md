# `std.fs`（文件系统）规范

Status: Frozen

本文件定义标准库 `std.fs` 的跨平台一致 API：文件/目录句柄及其最小操作集合。

错误码：所有可能失败的操作以 `..., int` 返回，错误码语义见 `cai/docs/spec/std_error.md` 与 `cai/docs/spec/error.md`。

---

## 1. 路径规则（跨平台一致性）

### 1.1 路径分隔符
CAI 标准库的路径字符串统一使用 `/` 作为分隔符。

在 Windows 平台：
- 标准库必须在系统调用边界把 CAI 路径转换为 Windows 所需形式（对用户不可见）。
- Windows 路径映射与边界行为见 `cai/docs/spec/std_fs_windows.md`（Status: Draft）。

### 1.2 绝对/相对
路径语义由标准库实现，但最低要求：
- 支持相对路径与绝对路径；
- 提供规范化 API（见 4）。

---

## 2. 句柄类型

### 2.1 `File`
- `std.fs.File` 是一个拥有型句柄，默认 move-only。
- drop 时必须关闭底层 OS 句柄。

### 2.2 `Dir`
- `std.fs.Dir` 是一个拥有型句柄，默认 move-only。
- drop 时必须关闭底层 OS 句柄。

---

## 3. `File` API（最低要求）

所有方法若失败返回非 0 错误码：

- `std.fs.open(path: string) -> std.fs.File, int`
- `std.fs.create(path: string) -> std.fs.File, int`

- `File.read(this, dst: Slice<byte>) -> usize, int`
  - 读入至多 `dst.len` 字节，返回实际读取字节数；
  - 到达 EOF 返回 `0, 0`（不视为错误）。

- `File.write(this, src: Slice<byte>) -> usize, int`
  - 写入至多 `src.len` 字节，返回实际写入字节数；
  - 允许短写；上层可循环写满。

- `File.seek(this, offset: long, whence: int) -> long, int`
  - `whence` 语义冻结：
    - `0`：从文件头（SET）
    - `1`：从当前位置（CUR）
    - `2`：从文件尾（END）
  - 返回新的绝对位置（字节偏移）。

- `File.stat(this) -> std.fs.Stat, int`
- `File.sync(this) -> int`

---

## 4. `Dir` 与路径工具（最低要求）

- `std.fs.openDir(path: string) -> std.fs.Dir, int`

路径工具：
- `std.fs.normalize(path: string) -> string, int`
  - 语义：做纯字符串级规范化（消除 `.`、处理 `..`、折叠重复 `/`），不得访问文件系统。

---

## 5. `Stat`（文件元信息）

`std.fs.Stat` 必须是 FFI-safe 的 `struct`（见 `cai/docs/spec/abi.md`），字段集合冻结为：
- `size: u64`
- `mtimeNs: u64`（最后修改时间，纳秒）
- `isDir: bool`
- `isFile: bool`
