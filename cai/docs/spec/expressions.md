# 表达式语义规范

Status: Frozen

本文件定义 CAI 表达式的求值顺序、短路、成员访问、调用、索引、字面量、cast、`??` 与 guard `? {}` 等。

---

## 1. 求值顺序
为可预测性，求值顺序冻结为：
- 函数调用参数从左到右求值。
- 二元运算符左右操作数从左到右求值（短路运算除外）。
- struct/map/array 字面量的字段/项从左到右求值。

---

## 2. 字面量
- 数值/字符串/布尔/`null`：见 `cai/docs/spec/lexical.md` 与各类型规范。
- map 字面量：见 `cai/docs/spec/map.md`
- array 字面量：见 `cai/docs/spec/array.md`
- struct 字面量：`TypeName{ field: expr }`：见 `cai/docs/spec/struct.md`

---

## 3. 成员访问与调用

### 3.1 成员访问
- `obj.field`：读取字段（或属性）。
- `obj.method(args...)`：方法调用（语义等价于将 `obj` 作为隐式 `this` 传入；具体规则见 `cai/docs/spec/struct.md` 与 `cai/docs/spec/impl.md`）。

### 3.2 调用
- `callee(args...)`：调用函数值或可调用对象。
- 泛型显式调用：`f<T1,T2>(args...)`（单态化规则见 `cai/docs/spec/fn.md`）。

---

## 4. 索引
- `x[i]`：
  - 若 `x` 为数组，表示元素访问（边界规则见 `cai/docs/spec/array.md`）。
  - 若 `x` 为 map，`x[i]` 只允许出现在赋值左侧：`x[i] = v`；在表达式位置使用为编译错误（见 `cai/docs/spec/map.md`）。

---

## 5. 逻辑与短路

### 5.1 `&&` / `||`
- 操作数必须为 `bool`（不做 truthiness）。
- `&&`：左为 `false` 时右侧不求值。
- `||`：左为 `true` 时右侧不求值。

### 5.2 `!`
- 操作数必须为 `bool`。

---

## 6. 空值合并 `??`
`a ?? b` 仅定义在 `Option<T>` 上：
- 当 `a` 为 `None` 时，结果为 `b`；
- 当 `a` 为 `Some(v)` 时，结果为 `v`；
- `b` 仅在需要时求值（短路）。

---

## 7. `&`（借用）

### 7.1 `&expr`
- `&x` 产生 `Ref<T>` 借用（类型规则见 `cai/docs/spec/ref.md`）。
- `&` 仅允许对可取址的 lvalue 使用（变量、字段、索引位置）；对 rvalue 为编译错误。

---

## 8. cast
语法：
- `(T)expr`（Java 风格）
- `expr as T`（后缀 cast）

约束：
- 仅支持数值类型之间的转换（见 `cai/docs/spec/numeric.md`）。
- 禁止对 `Ptr/Ref/struct/map/array` 等做 cast（编译错误）。

---

## 9. Guard `call ? { ... }`
语法见 `cai/docs/SYNTAX.md`。

语义：
- `call` 必须是调用表达式，并返回至少 2 个值；
- 最后一个返回值类型为 `int` 错误码，`0` 表示成功；
- 若错误码非 0，执行 guard block（必须中断控制流）；
- 若成功，表达式结果为“去掉末尾 err 的剩余返回值”：
  - 若剩余仅 1 个值，则表达式类型为该值类型；
  - 若剩余为多个值，则表达式产生多返回（可被解构）。

此机制属于错误模型的一部分（见 `cai/docs/spec/error.md`）。
