# `string` 规范

Status: Frozen

本文件定义 CAI 的 `string` 类型语义。目标是：性能可预测、Unicode 语义清晰、无需暴露底层指针。

---

## 1. 语义模型
- `string` 表示一段 Unicode 文本。
- `string` 为**不可变**值：任何“修改”语义都必须生成新 `string`。
- `string` 无“身份相等”概念：
  - `==/!=` 按文本内容比较；
  - 不提供/不定义“底层地址是否相同”的比较。

---

## 2. 编码与规范化
- 运行时的规范存储编码为 UTF-8。
- 字符串字面量与运行时构造出来的 `string` 必须满足“合法 UTF-8”。
  - 若从字节序列构造 `string`，遇到非法序列的处理策略必须由 API 明确（见 7）。
- `string` 的相等性不做 Unicode 规范化（NFC/NFKC 等）：
  - `==` 比较的是 codepoint 序列（以 UTF-8 解码后的序列等价判断）。
  - 若需要规范化比较，由 `std.unicode`（或等价模块）提供显式函数。

---

## 3. 空值与字面量
- `""` 是空字符串。
- `string` **不可** 为 `null`（`null` 只用于 `Ptr<T>`/`Ref<T>`/`any`，见 `cai/docs/spec/null.md`）。

---

## 4. 运算符

### 4.1 相等比较
- 允许：`a == b`、`a != b`（其中 `a/b` 均为 `string`）。
- 结果为 `bool`。

### 4.2 拼接
- `a + b`：当 `a/b` 均为 `string` 时，返回新 `string`。
- `a += b`：语法糖，等价于 `a = a + b`。

### 4.3 有序比较
不定义 `string` 的 `< <= > >=`（编译错误），避免引入隐式 locale/规范化/比较规则。
若需要排序，使用 `std.string.compare(a,b) -> int` 等显式 API。

---

## 5. 长度与迭代

### 5.1 长度
`string` 同时存在两种常用长度：
- `byteLen(s)`：UTF-8 字节长度（`usize`）。
- `len(s)`：Unicode codepoint 个数（`usize`）。

约束：
- `byteLen(s)` 必须为 O(1)。
- `len(s)` 允许为 O(n)（按需扫描），实现可缓存以优化。

### 5.2 迭代
不提供 `s[i]` 形式的索引（索引对 Unicode 容易误导）。
遍历必须通过 `std.utf8`（或等价模块）提供的 iterator：
- `for cp in utf8.codepoints(s) { ... }`

---

## 6. 子串与切片
不提供语言级别的 `s[a:b]` 语法。
若需要子串，使用显式 API，并明确切片单位：
- 按字节范围：`std.string.sliceBytes(s, startByte, endByte) -> string, int`
- 按 codepoint 范围：`std.string.slice(s, start, end) -> string, int`

约束：
- 若切片边界落在 UTF-8 多字节中间，必须返回错误码（或以替换字符策略；需在 API 冻结）。

---

## 7. 与 `bytes` 的互转
`string` 与原始字节的互转必须显式指定编码策略：
- `std.string.toBytes(s: string, encoding: string) -> bytes, int`

必须至少支持：
- `"UTF-8"`（以及 `""` 等价于 `"UTF-8"` 的约定）

对非法序列：
- `fromBytes(...,"UTF-8")` 遇到非法 UTF-8 必须返回非 0 错误码，且返回值必须为 `""`。

---

## 8. 哈希
若 `string` 可作为 `map` key，则必须有稳定的哈希语义：
- `hash(s)` 返回 `u64`。
- 哈希算法允许由实现选择，但必须满足：
  - 同一进程内稳定；
  - 对抗 hash-flood（建议带随机 seed 的 SipHash 或等价方案）。

为支持可复现测试，编译器必须提供显式开关启用“确定性哈希模式”（固定 seed）；默认模式使用随机 seed。
