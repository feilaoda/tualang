# `map` / `map<K,V>` 规范

Status: Frozen

本文件定义 CAI 的内建映射类型 `map` 与 `map<K,V>` 的语义与字面量规则。

---

## 1. 类型定义

### 1.1 类型形态
- `map<K,V>`：静态类型映射。
- `map`：语法糖，等价于 `map<any, any>`。

### 1.2 可空性
`map<K,V>` 不可为 `null`（见 `cai/docs/spec/null.md`）。

---

## 2. 字面量（Literal）

### 2.1 语法
使用大括号 `{ ... }`：
- map 字面量：`{ key: value, ... }`
- 空 map：`{}`

注：`{ ... }` 仅用于 map 字面量；数组字面量使用 `[...]`（见 `cai/docs/spec/array.md`）。

### 2.2 key 表达式
map 字面量允许任意表达式作为 key：
- `{ kExpr: vExpr, ... }`

求值顺序：
- key/value 按出现顺序从左到右求值（见 `cai/docs/spec/expressions.md`）。

### 2.3 字面量的类型推断
当上下文提供目标类型 `map<K,V>` 时：
- 每个 key/value 必须可转换到 `K/V`（必要时使用显式 cast 或显式构造）。

当上下文没有提供目标类型时：
- `{ "a": 1 }` 的默认类型为 `map<any, any>`。

### 2.4 空字面量 `{}` 的类型
- `{}` 表示空 map 字面量。
- `{}` 必须出现在“可推断上下文”中，否则为编译错误：
  - 允许：`let m: map<string,int> = {}`
  - 禁止：`let x = {}`

---

## 3. 基本操作与复杂度

### 3.1 插入/更新
语义：
- `m[k] = v`：插入或更新 key 对应 value（语言内建写入语法）。

约束：
- `m[k] = v` 要求 `m` 为 map 值；对非 map 为编译错误。

复杂度：
- 平均 O(1)，最坏 O(n)（hash 冲突）。

### 3.2 读取
`m[k]` **只允许出现在赋值左侧**（写入语法糖）。在表达式位置使用 `m[k]` 为编译错误。

读取必须使用以下 API 族（更明确，也更易实现安全默认）：
- `m.get(k) -> Option<V>`：仅当 `V` 是“可复制值类型”时允许（例如数值/bool/`string`）。
- `m.getRef(k) -> Option<Ref<V>>`：返回共享只读引用（适用于任意 `V`）。
- `m.getMut(k) -> Option<Ref<V>>`：返回独占可写引用（适用于任意 `V`）。

> `Ref<V>` 的读写规则应在 `cai/docs/spec/ref.md` 冻结；本文件只冻结 map 与引用交互的形态。

### 3.3 删除
- `m.remove(k) -> Option<V>`：移除并返回 value（当 `V` 为 move-only 时，此操作会发生所有权转移）。

### 3.4 包含判断
- `m.contains(k) -> bool`

### 3.5 长度
- `m.len() -> usize`

---

## 4. 迭代
迭代必须提供稳定的语义（但不要求稳定顺序）：
- `for v in m { ... }`：遍历 value
- `for k, v in m { ... }`：遍历 key/value

顺序：
- 不保证迭代顺序稳定；如需稳定顺序，使用 `std.map.sortedKeys()` 等显式 API。

借用约束：
- 在迭代期间，不允许对同一个 map 结构做“可能触发 rehash/扩容”的写操作（否则迭代器失效）。
- `getMut` 与迭代的交互必须定义并在语义阶段拒绝冲突借用。

---

## 5. Key 类型与哈希/相等

### 5.1 Key 约束
`map<K,V>` 的 `K` 类型限制为以下集合（否则为编译错误）：
- 整数类型与 `bool`
- `string`
- `enum`（仅限无 payload 的 enum；基于其 tag 值）
- `Ptr<T>`

相等性：
- 对 key 使用该类型的 `==` 语义比较相等（见 `cai/docs/spec/operators.md`）。

哈希：
- 对 key 使用如下哈希：
  - 整数/bool：按其数值映射为 `u64` 并混合
  - `string`：使用 `string.hash() -> u64`（见 `cai/docs/spec/string.md`）
  - `enum`：按其 tag 值映射为 `u64` 并混合
  - `Ptr<T>`：按其地址位模式映射为 `u64` 并混合

### 5.2 `null` key
不允许 `null` 作为 key（编译错误）。

---

## 6. 值类型与所有权（最容易出错的部分）

为保证“无 GC + 可预测释放”，本文件对 value 行为做出约束：
- 当 `V` 是“可复制值类型”（例如数值、bool、string）：
  - `get(k)` 可按值返回（复制/共享）
- 当 `V` 是“可能持有资源的类型”（例如 struct/容器/句柄等）：
  - 默认不允许 `m[k]` 产生按值读取；
  - 必须通过 `getRef/getMut/remove` 这类显式 API 与所有权/借用模型配合。

---

## 7. 错误模型
map 的失败模式应当“可预测且可组合”：
- `get*`：未命中用 `Option` 表达，不用错误码。
- 运行时错误（例如 `null` map 解引用、内存不足）应有统一的错误路径（在全局错误模型中冻结）。
