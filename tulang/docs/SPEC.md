## Tua 语言规格（工作草案）

本文件用于把 `README.md` 的“描述性语法”落成“可实现的语义约束”，并记录尚未决策的点。

约定：本文用 `Status: Implemented | Partial | Planned` 标记当前实现进度。

### 1. 词法与分隔（Status: Implemented）
- 语句以换行分隔；不要求每句以 `;` 结尾（实现层面：换行会被当作 separator token）。
- 注释：
  - `//` 单行注释
  - `/* ... */` 多行注释

### 2. 类型系统（Status: Implemented）
- 基础类型：`int/long/double/bool/string/void`
  - 当前运行时表示：`string` 仍等价于 C 字符串指针（`i8*`）；“真正的 string 类型”见后续规划
- 命名类型：`T`（用于 `struct T`；值语义）
- 引用类型：`&T`（LLVM 后端中表现为 `T*` 指针）

### 3. 变量与赋值（Status: Implemented）
- 变量声明：
  - `let name[:Type] = expr`
  - `const name[:Type] = expr`
  - `let name = expr` 会进行类型推断（当前为“编译期 LLVM 类型推断”，尚未做完整语义层）
  - 若无法推断，当前默认按 `int` 处理（后续语义层会补齐错误提示）
- 赋值：
  - `name = expr`
  - `obj.field = expr`
- `private` 修饰符：
  - `struct`/`object` 中的 `private fn` 已支持解析
  - 模块导出：默认导出全部顶层符号；`private fn/struct/object/enum` 不导出（Status: Implemented）

### 4. 表达式（Status: Implemented）
- 字面量：整数/长整数/浮点/字符串/`true`/`false`
- 一元运算：`-x`、`!x`、`&x`（取址目前仅支持变量）
- 二元运算：`+ - * /`、`== != < <= > >=`、`&& ||`（`&&/||` 为短路求值）
- 自增自减：`i++`、`i--`、`++i`、`--i`（当前主要用于整型变量）
- 调用：`f(a,b)`、成员访问/调用：`obj.field`、`obj.method(a,b)`
- 分组：`(expr)`

### 5. 控制流（Status: Implemented）
- 条件：
  - `if cond { ... } else if cond { ... } else { ... }`（支持链式 `else if`）
  - 条件也支持括号：`if (cond) { ... }`
- `for`（C 风格，Status: Implemented）：
  - `for (init; cond; inc) { ... }`
  - `for init; cond; inc { ... }`
  - 说明：`init/cond/inc` 之间的分隔符在实现里使用 `;` token（换行也会被当作 `;`）
- `for-in`（Status: Implemented）：
  - `for i in expr { ... }`
- 规划能力（Status: Planned）：
已实现（Status: Implemented）：
- `while cond { ... }`
- `do { ... } while cond`
- `break` / `continue`（在 `for/while/do-while` 内）
- `goto` / label（Lua 风格）：
  - label：`::name::`
  - 跳转：`goto name`

### 6. 函数（Status: Partial）
- 定义：
  - `fn name(a[:Type], b[:Type]) -> Type { ... }`
  - `fn name(a[:Type], b[:Type]) Type { ... }`（语法糖，省略 `->`）
  - 形参类型可省略；当前默认按 `int` 处理（完整类型检查见语义层规划）
- 返回：
  - `return expr`
  - `return`（void）
- 多返回值（Status: Implemented）：
  - 函数返回类型可写成列表：`fn f() -> int, string { ... }`
  - `return` 支持多表达式：`return a, b`
  - 解构声明：`let a, b = f()` / `const a, b = f()`
  - 解构赋值：`a, b = f()`
  - 表达式上下文规则：当 `f()` 返回多个值时，在普通表达式上下文会自动取第一个值（例如 `let x = f()`）
  - `return f()` 转发规则：当当前函数是多返回值函数时，`return f()` 会直接转发 `f()` 的多返回结果
- 闭包/upvalue（Status: Planned）：
  - 需要引入函数值与捕获环境（Analyze + Runtime）
- 内建函数（Status: Implemented）：
  - `print(x)` / `println(x)`（目前支持打印 int/long/double/bool/string 指针）

### 7. struct / object / enum（Status: Partial）

#### 7.1 struct（Status: Implemented）
- `struct T { field: Type = initializer ...; fn method(...) ... }`
- 表达式 `T(...)` 表示构造：返回 **T 值**
  - 构造参数按字段声明顺序依次赋值（多余参数报错）
  - 未提供参数的字段：优先用字段 `initializer`，否则使用零值
- 值语义：
  - `let b = a` 是值拷贝（字段复制）
  - `&a` 取 `a` 的地址，得到引用（类型层面是 `&T`，LLVM 层面是 `T*`）
- 字段访问：`p.x` / `p.x = v`（接收者目前仅支持变量；若变量是 `&T` 则会间接到指向的对象）
- 实例方法：`p.m(a,b)` 编译为 `T__m(&p, a, b)`（方法的 `this` 为 `&T`）
- `this`：仅在 `struct` 实例方法中可用，类型恒为 `&T`；`this` 绑定不可重新赋值，但可通过 `this.field = ...` 修改字段
- `init/deinit`（Status: Partial）：
  - 已支持解析 `init(){...}` / `deinit(){...}` 为方法
  - 自动调用时机/析构语义尚未定义（见内存模型规划）

#### 7.2 impl（Status: Planned，Rust 风格）
- 语法：
  - `impl StructName { fn func(...) ... }`
- 语义：
  - `impl StructName` 中的 `fn` 会作为 `StructName` 的实例方法（与 `struct StructName { fn ... }` 等价）
  - 这些方法内允许使用 `this`（类型恒为 `&StructName`）
  - 若同名方法重复定义（struct 内 vs impl 块，或多个 impl），视为编译错误（第一版）

#### 7.3 object（Status: Implemented）
- `object O { fn f(...) ... }`
- `O.f(a,b)` 编译为 `O__f(a,b)`（静态方法）

#### 7.4 enum（Status: Implemented）
- 声明：`enum E { A, B = 10, C, D = "raw" }`
- 模式：
  - **int-tag enum**：只出现 `= 123`（或全部省略），则 `E.A` 返回 `int tag`
  - **string-tag enum**：只出现 `= "raw"`（或省略），则 `E.A` 返回 `string tag`
  - 不允许同一个 enum 同时混用 int 与 string（会报错，best-effort 继续）
- int-tag 规则：
  - `E.A` 返回 **int tag**（按声明顺序递增；若显式 int 则重置递增基准）
  - 自动生成：`E.toString(tag:int) string`（返回 variant 名字；默认 `"Unknown"`）
- string-tag 规则：
  - `E.A` 返回 **string tag**（若显式 `"raw"` 则用 raw；否则用 variant 名字）
  - 自动生成：`E.toString(tag:string) string`（identity）

### 8. 模块（Status: Implemented）
- 默认全部导出；声明前加 `private` 则不导出（`private fn/struct/object/enum`）
- 语法：
  - `import "relative/or/absolute/path.tua"`：仅执行模块（副作用导入），不引入名字
  - `from "path.tua" import A,B,C`：把导出的符号引入到当前模块作用域
- 规则：
  - 相对路径以当前文件所在目录为基准
  - 若省略后缀，会自动补 `.tua`
  - 模块缓存：同一模块只会被加载/执行一次（顶层语句只会在 `main` 中生成一次）
  - 循环依赖：允许；按依赖优先的加载顺序进行一次性执行（后续可补更精确的初始化时序定义）
- 当前限制：
  - 只支持字符串字面量路径（不支持表达式路径）
  - 仅支持导入 `fn/struct/object/enum`；不支持导入模块级变量

### 9. 数据结构与空值（Status: Planned）
- `null`：空值语义（比较/打印/条件判断/赋值）
- `string`：作为真正的运行时基础类型（不只是 `i8*` 指针）
- `map/table`：键值容器（优先）
