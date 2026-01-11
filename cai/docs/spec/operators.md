# 运算符（类型规则与语义）规范

Status: Frozen

本文件定义 CAI 的运算符集合、适用类型与核心语义。除非本文件明确允许，否则运算符不支持用户自定义重载。

---

## 1. 总则
- CAI 不支持运算符重载。
- 除少量语法糖（如 `+=` 等价于 `a = a + b`），运算符语义不依赖标准库。
- 需要隐式转换的地方必须冻结；CAI 的默认策略是“尽量不做隐式转换”（见 `cai/docs/spec/types.md` 与 `cai/docs/spec/numeric.md`）。

---

## 2. 赋值类
- `=`：左侧必须是可写 lvalue（见 `cai/docs/spec/bindings.md`）。
- `+=`：仅当 `a + b` 合法时允许，等价于 `a = a + b`。

---

## 3. 相等比较 `==` / `!=`

`==`/`!=` 仅对以下类型定义：

### 3.1 数值
- 两侧必须是**同一数值类型**；不同数值类型比较需要显式 cast。

### 3.2 `bool`
- 两侧必须为 `bool`。

### 3.3 `string`
- 两侧必须为 `string`，按内容比较（见 `cai/docs/spec/string.md`）。

### 3.4 `enum`
- 仅对“无 payload 的 enum”（fieldless enum）定义 `==/!=`：
  - 两侧必须为同一 `enum` 类型；
  - 按 tag 值比较（见 `cai/docs/spec/enum.md`）。

对带 payload 的 enum 使用 `==/!=` 为编译错误（必须通过 `match` 做分支处理）。

### 3.5 `Ptr<T>` 与 `Ref<T>`
- 允许与 `null` 比较：`p == null` / `p != null`
- 允许同类型指针/引用比较：`Ptr<T>` 与 `Ptr<T>`、`Ref<T>` 与 `Ref<T>`
- 比较语义为地址值相等（不解引用）。

### 3.6 `any`
- `any` 的 `==/!=` 语义见 `cai/docs/spec/any.md`。

对其他类型（例如 `struct`、`map`、数组、`bytes`）使用 `==/!=` 为编译错误。

---

## 4. 有序比较 `< <= > >=`
- 仅对数值类型定义（两侧必须同类型；不同数值类型需显式 cast）。
- 对 `string/enum/Ptr/Ref/any/struct/map/array/bytes` 使用有序比较为编译错误。

---

## 5. 逻辑运算 `&& || !`
- 仅对 `bool` 定义。
- 不存在 truthiness：`if`/`while`/`for` 的条件表达式必须为 `bool`（见 `cai/docs/spec/controlflow.md`）。

---

## 6. 算术与位运算
详见：
- `cai/docs/spec/numeric.md`（`+ - * /`, `~ & | ^ << >>`, `++/--`, cast）
- `cai/docs/spec/string.md`（`string + string`）

---

## 7. `??`（空值合并）
`??` 仅定义在 `Option<T>` 上，语义见 `cai/docs/spec/expressions.md` 与 `cai/docs/spec/option.md`。

---

## 8. `as` 与 `(T)expr`（cast）
cast 仅用于数值类型之间转换；规则见 `cai/docs/spec/numeric.md` 与 `cai/docs/spec/expressions.md`。
