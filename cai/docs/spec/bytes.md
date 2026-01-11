# `bytes`（拥有型字节序列）规范

Status: Frozen

本文件定义 `bytes`：用于二进制数据的拥有型容器，适配 I/O、序列化、模型权重/张量等场景。

---

## 1. 类型与所有权
- `bytes` 是拥有型容器，默认 move-only（见 `cai/docs/spec/ownership.md`）。
- `bytes` 内容是连续的 `byte(u8)` 序列。

---

## 2. 基本操作（标准库约定）
最低要求 API（签名与错误行为冻结）：
- `bytes.len() -> usize`
- `bytes.capacity() -> usize`（若实现为可增长 buffer）
- `bytes.push(b: byte)`
- `bytes.extend(src: Slice<byte>)`
- `bytes.slice() -> Slice<byte>`（只读视图）
- `bytes.mutSlice() -> Slice<byte>`（可写视图）
- `bytes.clear()`
- `bytes.clone() -> bytes`
- `bytes.toString() -> string`
- `bytes.toString(encoding: string) -> string`
  
---

## 3. 与 `string` 的互转
见 `cai/docs/spec/string.md` 的编码章节：
- `string.toBytes(s: string, encoding: string) -> bytes, int`

---

## 4. FFI
`bytes` 不直接映射为裸指针：
- 对 extern/FFI 建议以 `Slice<byte>` 传递视图（只读/可写由 `slice()`/`mutSlice()` 以及形参 `const/let` 约束表达）；
- 若需要把 bytes 的所有权交给 C，必须定义明确的释放函数与所有权转移 API（避免双重释放）。
