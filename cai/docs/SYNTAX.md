# CAI 语法定义

本文件为 CAI 的语法参考与冻结依据（自包含的“语言外观”说明）。完整的可解析 grammar（ANTLR4，包含 lexer+parser）见 `cai/parser.g4`。

约定：
- 文件编码：UTF-8。
- 本文描述“语法与写法”；类型系统/所有权/借用/错误模型等语义规则应在单独的 SPEC 中冻结（后续补齐）。
- 语义规范按主题拆分在 `cai/docs/spec/README.md`。

---

## 1. 词法（Lexical）

> 以 `cai/parser.g4` 的 lexer 规则为准；本节给出人类可读的总览。

### 1.1 空白、换行、分隔符
- 空白字符：`' ' '\t' '\f' '\v' '\r'`（忽略）。
- 换行：`'\n'`（以及 `\r\n` 的 `\n`）会被词法层产生 **语句分隔符 token `SEMI`**。
- 显式分号 `;` 也会产生 `SEMI`。

因此：CAI 的“语句分隔”是 **“换行或分号”**（语法中常以 `sep` 表示）。

### 1.2 注释
- 单行注释：`// ...`（到行尾）。
- 多行注释：`/* ... */`（不嵌套）。

### 1.3 标识符
- `Identifier ::= [A-Za-z_][A-Za-z0-9_]*`

### 1.4 关键字（Keywords）

#### 1.4.1 声明与控制流
- `let`（别名：`var`）
- `const`
- `if` `else`
- `match`
- `for` `in`
- `while` `do`
- `unsafe`
- `break` `continue`
- `goto`
- `return`
- `import` `from` `as`
- `private`
- `embed`

#### 1.4.2 类型/声明体
- `fn`（别名：`func`）
- `extern`
- `struct` `enum` `trait` `impl` `object`
- `init` `deinit`
- `this`
- `move`
- `mut`

说明：
- `mut` 仅用于函数参数模式（例如 `fn f(mut x: T) {}`），不用于变量绑定声明。

#### 1.4.3 并发/异步（语义在 SPEC 冻结）
- `async`
- `await`
- `defer`
- `yield`

#### 1.4.4 字面量关键字
- `null`
- `true` `false`

#### 1.4.5 基础类型关键字
- `Ref` `any` `void`
- `bool`（别名：`boolean`）
- `string`
- 整数：`byte u8 u16 u32 u64 usize i8 i16 int long isize`
- 浮点/存储：`f8 f16 f32 f64 bf8 bf16 float double`

### 1.5 字面量（Literals）
- 整数：
  - `INT_LIT ::= Digit+`
  - `LONG_LIT ::= Digit+ ('l' | 'L')`
- 浮点：
  - `FLOAT_LIT ::= Digit+ '.' Digit+ ExponentPart? | Digit+ ExponentPart`
  - `ExponentPart ::= ('e'|'E') ('+'|'-')? Digit+`
- 字符串：
  - `STRING_LIT ::= '"' ( EscapeSeq | ~["\\] )* '"'`
  - `EscapeSeq ::= '\\' .`

### 1.6 运算符与分隔符（按“最长匹配”）
- `...` `::` `??` `->` `=>` `<=` `>=` `==` `!=` `&&` `||` `++` `--`
- `=` `+` `-` `*` `/` `<` `>` `!` `&` `|` `^` `~` `?`
- `.` `:` `,`
- `(` `)` `{` `}` `[` `]`

---

## 2. 运算符优先级与结合性

从高到低：
1. 后缀：成员访问 `.`、调用 `()`、泛型调用 `<T>(...)`、索引 `[]`、struct init `{...}`、guard `? { ... }`、后缀自增减 `x++/x--`、`as` cast
2. 一元：`++x --x -x !x ~x &x await x`
3. 乘除：`* /`
4. 加减：`+ -`
5. 位移：`<< >>`
6. 按位与：`&`
7. 按位异或：`^`
8. 按位或：`|`
9. 逻辑与：`&&`
10. 逻辑或：`||`
11. 空值合并：`??`（右结合、短路）
12. 赋值：`=`（右结合）

---

## 3. 语法（Grammar）

完整 grammar 见 `cai/parser.g4`（包含 lexer+parser、表达式优先级、以及“换行/分号作为 `SEMI`”的处理）。

---

## 4. 语法糖与语义约束（语法以外但与“写法”强相关）

### 4.1 类型标注声明糖
- `name: T = expr` 等价于 `let name: T = expr`

说明：
- `name` 是变量名（标识符），`T` 是类型名。

### 4.2 Guard block（错误守卫）
语法：`callExpr ? { ... }`

说明：
- `? { ... }` 仅用于错误守卫；CAI 不提供 `?.`（可选链）或 `?` 作为可选类型解包语法。

约束（应在语义阶段强制）：
- `callExpr` 必须是调用表达式；
- 该调用必须返回至少 2 个值，且最后一个返回值类型为 `int`（错误码，`0` 表示成功）；
- guard 块必须中断控制流（`return`/`break`/`continue`），不得“顺序落下”。

### 4.3 权限/特性控制（不作为语言语法）
CAI 不提供 `requires ...` 这类“能力声明语法”：
- 若需要权限/特性控制，走构建参数与运行时配置（例如生产构建禁用网络 provider，仅允许本地模型）。
- 若需要可审计元数据，使用 `@attribute`（例如 `@caps("net")`；见 `cai/docs/spec/policy.md`）或项目级清单文件（例如 `cai.toml`；见 `cai/docs/spec/package.md`）。

### 4.4 `@attribute`（注解）
语法：
- `@name`
- `@name(key = value, ...)`

约束：
- `@attribute` 只能出现在 declaration 前（见 `cai/parser.g4` 的 `declaration: attribute* (...)`）。
- `attributeArg` 建议限制为编译期常量表达式，便于工具链静态提取 schema/超时/策略等元数据。

### 4.5 `async/await/defer`（并发语法）
语法：
- `async fn f(...) { ... }`
- `await expr`（一元前缀运算符）
- `defer statement`
- `yield`（语句）

约定：
- `await` 只能用于 `async` 上下文；否则为编译错误。
- `defer` 执行顺序为 LIFO，并在 `return`/`break`/`continue` 等离开作用域路径上执行。
- `yield` 只能用于 `async` 上下文；否则为编译错误。

### 4.6 `Ptr<T>`（限制性指针）
CAI **不提供** 裸指针类型关键字（例如 `ptr`/`void*` 这类语法层面直接暴露地址的类型）。

指针只以受限形式出现：`Ptr<T>`（一个普通的泛型命名类型；由 `std.ptr` 或 prelude 提供）。

语法层面：
- `Ptr<T>` 按 `namedType` + `typeArgList` 解析，不需要新增语法。

语义约束（在 SPEC 中实现/强制）：
- `Ptr<T>` 可为 `null`；允许 `==`/`!=` 与同类型指针或 `null` 比较。
- 禁止指针算术（`p + 1`、`p - 1`、`p[i]` 等）与有序比较（`< <= > >=`）。
- 禁止把 `Ptr<T>` 与整数互转（除非通过受控的 `std.ptr` API，并且可选要求 `unsafe` 上下文）。
- 解引用/读写内存只能通过受控 API（例如 `Ptr<T>.read()`/`write()`/`copyFrom()`），以便将来引入 `unsafe { ... }` 或静态检查边界。

### 4.7 `embed`（资源嵌入）
语法：`embed "path/to/asset.bin"`

约束：
- 路径必须是字符串字面量（编译期常量）。
- 语义与产物要求见 `cai/docs/spec/embed.md` 与 `cai/docs/spec/aot.md`。

### 4.8 `enum`（带 payload 的变体）
写法：
- `enum E { A\nB }`（无 payload；变体使用换行分隔）
- `enum E { A(int)\nB(string, int) }`（带 payload；变体使用换行分隔）

约束：
- `Variant()` 不允许；空 payload 直接写 `Variant`。
- 布局语义见 `cai/docs/spec/abi.md` 与 `cai/docs/spec/enum.md`。

### 4.9 `extern`（批量声明）
除了单个声明形式 `extern fn ...`，还支持批量声明：

```cai
extern {
  fn f() -> int
  fn f2() -> void
}
```

语义：
- 等价于写多个顶层 `extern fn` 声明；
- 语义规则见 `cai/docs/spec/ffi.md`。

### 4.10 函数参数默认借用
写法（示意）：
- 默认只读借用：`fn f(x: T) { ... }`
- 可写借用：`fn f(mut x: T) { ... }`
- 所有权转移：`fn f(move x: T) { ... }`

说明：
- 参数默认是 `const`（只读借用）；`mut` 表示独占可写借用；`move` 表示接管所有权。
- 语义冻结见 `cai/docs/spec/fn.md` 与 `cai/docs/spec/ownership.md`。
