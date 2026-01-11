# `std.fs`（Windows 路径映射）规范

Status: Draft

本文件定义 `std.fs` 在 Windows 平台上的路径映射规则。CAI 用户侧路径统一使用 `/` 分隔符；本文件描述如何映射到 Windows API 所需形式。

本文件为 Draft：允许在实现期根据兼容性与安全性调整，但任何变更都必须保持“同一输入路径在同一版本上确定性映射”。

---

## 1. 接受的路径形式（用户侧）

Windows 上 `std.fs` 接受以下用户侧路径前缀：

### 1.1 盘符路径
- `X:/path/to/file`
  - `X` 为 ASCII 字母（不区分大小写）。
  - `:` 后必须紧跟 `/`，不允许 `X:relative` 省略分隔符。

### 1.2 UNC 路径
- `//server/share/path/to/file`

### 1.3 “当前盘符根”绝对路径
- `/path/to/file`
  - 表示当前进程的“当前盘符”的根目录下路径（与 Windows `\path\to\file` 的语义一致）。

相对路径：
- `foo/bar/baz`
  - 相对当前工作目录（由运行时决定）。

---

## 2. 规范化（normalize）与禁止项

`std.fs.normalize(path)` 在 Windows 上必须做纯字符串级规范化：
- 折叠重复 `/`；
- 消除 `.`；
- 处理 `..`（不得越过盘符根或 UNC share 根；越界为错误码 `std.error.Code.IO_ERROR`）。

禁止项（编译或运行时错误）：
- 路径中包含 `\\` 作为分隔符：标准库必须把 `\\` 当作普通字符并返回错误码 `IO_ERROR`（避免把 Windows 分隔符渗透到 CAI 语义中）。

---

## 3. 映射到 Windows API

映射规则（规范性）：
- 将用户侧 `/` 分隔符映射为 Windows 的 `\` 分隔符；
- `X:/a/b` 映射为 `X:\a\b`
- `//server/share/a/b` 映射为 `\\server\share\a\b`
- `/a/b` 映射为 `\a\b`（由 Windows 以“当前盘符根”解释）
- 相对路径 `a/b` 映射为 `a\b`

本规范不引入 `\\?\` 长路径前缀；是否启用属于实现细节（Draft）。

