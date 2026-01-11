# 类型系统总览

Status: Frozen

本文件定义 CAI 的类型系统：类型分类、类型写法、可转换性与默认规则。

---

## 1. 类型分类

### 1.1 基础类型（primitive）
由语法关键字直接表示（见 `cai/parser.g4`）：
- 数值类型（见 `cai/docs/spec/numeric.md`）
- `bool`
- `string`（见 `cai/docs/spec/string.md`）
- `any`（见 `cai/docs/spec/any.md`）
- `void`

### 1.2 复合类型（composite）
- 数组：`T[]`（动态）与 `T[N]`（定长）（见 `cai/docs/spec/array.md`）
- map：`map<K,V>` 与 `map`（见 `cai/docs/spec/map.md`）
- 引用：`Ref<T>` 与 `&T`（见 `cai/docs/spec/ref.md`）
- 受限指针：`Ptr<T>`（见 `cai/docs/spec/ptr.md`）
- 用户类型：`struct`、`enum`、`trait`（见对应 spec）
- 函数类型：`(T1, T2) -> (R1, R2)`（见 `cai/docs/spec/fn.md`）
- 泛型命名类型：`Name<T1, T2, ...>`（如 `Option<T>`，见 `cai/docs/spec/option.md`）

---

## 2. `void`
- `void` 仅用于表示“无返回值”。
- `void` 不可作为变量类型（编译错误），避免“可有可无的值”引入歧义。

---

## 3. 可空性（nullable）
CAI 不提供“隐式可空的引用类类型”。

- `null` 只属于：`Ptr<T>`、`Ref<T>`、`any`（见 `cai/docs/spec/null.md`）。
- `string/bytes/array/map` 等拥有型值不可为 `null`。
- 表达“可能缺失”统一使用 `Option<T>`（见 `cai/docs/spec/option.md`）。

---

## 4. 类型相等与兼容

### 4.1 类型相等
两类型相等当且仅当：
- primitive 类型关键字相同；
- 复合类型的构造器与类型参数递归相等；
- 命名类型的全限定名相同且类型参数递归相等。

### 4.2 隐式转换（最小集合）
CAI 尽量少做隐式转换：
- 数值字面量在上下文中可被定型到目标数值类型（见 `cai/docs/spec/numeric.md`）。
- 其余转换一律要求显式 cast（见 `cai/docs/spec/expressions.md`）。

---

## 5. 类型推断（边界）
- `let x = expr`：从 `expr` 推断 `x` 类型。
- `let x: T = expr`：要求 `expr` 可转换到 `T`。
- `{}`/`[]` 等空字面量必须有可推断上下文（否则编译错误；见 `cai/docs/spec/map.md` 与 `cai/docs/spec/array.md`）。
