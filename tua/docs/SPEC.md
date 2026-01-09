## Tua 语言规格（工作草案）

本文件用于把 `README.md` 的“描述性语法”落成“可实现的语义约束”，并记录尚未决策的点。

约定：本文用 `Status: Implemented | Partial | Planned` 标记当前实现进度。

语法参考（ANTLR4）：
- `tuaparser.g4`：参考语法（lexer+parser，面向工具/文档）
- 说明：语义与边界以 `src/lexer.c`/`src/parser.c` 和本文为准；`tualexer.g4` 为历史遗留，已弃用

### 0. 核心语义冻结（v0）
本节列出已冻结的核心语义点：实现可以逐步补齐，但语义以本文为准；如需破坏性变更，需要先更新 SPEC 并同步 ROADMAP。
- `struct` 的 move/引用语义（见 7.1）
- `enum` tag/raw 语义（见 7.4）
- `null` 语义（字面量、比较、truthiness、字符串拼接等，见 4/5/9.3）
- 字段/构造参数初始化优先级（见 7.1）
- `this` 规则（可用范围 + 隐式字段访问/赋值，见 7.1）

### 0.1 分层与归属（Frozen）
本仓库约定三层：`tua_rt`（C 运行时/平台抽象）/ `std`（Tua 通用库）/ `llm`（应用包）。本节冻结“能力归属”，避免把 LLM 逻辑耦合进编译器或运行时。
- [`tua_rt`] 只放通用跨平台原语与稳定 ABI：内存/句柄/文件/时间/线程/原子/workqueue/CPU feature/`mmap`
- [`std`] 只放通用可复用能力：`bytes/io/json/tokenizer/utf8` 等，不绑定具体模型结构
- [`llm`] 放模型格式、推理图、KV cache、采样、runner；可选引入通用 C 数学内核（但不新增“llm 专用 builtin”）

### 1. 词法与分隔（Status: Implemented）
- 语句以换行分隔；不要求每句以 `;` 结尾（实现层面：换行会被当作 separator token）。
- 注释：
  - `//` 单行注释
  - `/* ... */` 多行注释

### 2. 类型系统（Status: Implemented）
- 基础类型：
  - 整数（有符号）：`i8/i16/int/long/isize`（其中 `int`=i32，`long`=i64）
  - 整数（无符号）：`u8/u16/u32/u64/usize/byte`（其中 `byte`=u8）
  - 浮点：`f16/float/f32/double/f64/bf16`（其中 `float`=f32，`double`=f64）
  - FP8：`f8/bf8`（当前作为“8-bit 存储类型”，只允许与 `u8/byte` 互转；标量算术/比较暂不支持）
  - 其他：`bool/string/ptr/any/void`
  - `ptr`：裸指针（当前实现中等价于 `i8*`），主要用于运行时/FFI 句柄与不透明指针
  - `any`：动态值（当前实现中为 `tua_value`），用于 `map` 等“动态容器”的 value 存储
  - 当前运行时表示：`string` 仍等价于 C 字符串指针（`i8*`）；“真正的 string 类型”见后续规划
- 命名类型：`T`（用于 `struct T`；值类型，默认 move-only）
- 引用值类型（Status: Partial）：
  - 当前实现：`&T`（LLVM 后端中表现为 `T*` 指针）
  - 规划（Frozen，Status: Planned）：语法层只保留一个引用类型 `Ref<T>`（一等引用值，可用于变量/参数/返回值类型）；`&T` 作为过渡期类型别名解析为 `Ref<T>`，最终会废弃类型写法 `&T` 以减少歧义
  - 说明（Frozen）：表达式 `&x` 产生“引用值”（类型为 `Ref<T>`；过渡期也可写成 `&T`）

### 3. 变量与赋值（Status: Implemented）
- 变量声明：
  - `let name[:Type] = expr`
  - `const name[:Type] = expr`
  - `let name = expr` 会进行类型推断（Status: Partial）：
    - 语义层（analyzer）已支持：字面量/容器字面量 + **`fn` 调用的声明返回类型**（`let x = f()` 取第 1 返回；含导入/跨模块调用）
    - 其余场景仍可能依赖 codegen 的 LLVM 侧“从 initializer 推断 LLVMType”的兜底（后续会逐步收敛到语义层）
  - 若无法推断，当前默认按 `int` 处理（后续语义层会补齐错误提示）
- 可修改性（Frozen）：
  - `let`：绑定可修改（可重新赋值；且允许通过该绑定修改其指向/拥有的对象内容）
  - `const`：绑定不可修改（不可重新赋值；且禁止通过该绑定修改对象内容，例如 `a.field = ...` / `a[i] = ...` / `a.push(...)`）
  - 语言不提供 `mut` 关键字；“不可修改”统一用 `const` 表达
- `const` 视图（Frozen，Status: Partial）：
  - `const view = x`：当 `x` 是 move-only 值（如 `struct/map/array`）时，创建 `x` 的共享只读视图（shared borrow），`x` 仍可被只读借用/传给只读函数，但在 view 存活期间禁止 move 与写
  - `const v = <non-lvalue/new value>`：仍然是普通的 `const` 拥有型绑定（不是视图）
- 引用绑定（Frozen）：
  - `let r = &x`：对 `x` 的引用值（默认可写/独占借用，见 10）
  - `const r = &x`：对 `x` 的只读引用值（共享借用，见 10）
  - 注意：`const r = x`（move-only）是“只读视图”；`const r = &x` 才是“引用值”
- 赋值：
  - `name = expr`
  - `obj.field = expr`
- `private` 修饰符：
  - `struct`/`object` 中的 `private fn` 已支持解析
  - 模块导出：默认导出全部顶层符号；`private fn/struct/object/enum` 不导出（Status: Implemented）
#### 3.1 标识符解析（Frozen）
当一个标识符 `x` 出现在表达式或赋值左侧时，按以下优先级解析：
1) 当前块作用域链上的局部变量/形参（含 shadowing）
2) 当前模块的顶层绑定（含导入引入的符号；同名冲突为编译错误）
3) 若处于 `struct` 实例方法体内，且 `this` 存在同名字段 `x`，则将 `x`（或 `x = v`）解析为 `this.x`（或 `this.x = v`）
若均不命中，则为未定义标识符（编译错误）。

### 4. 表达式（Status: Implemented）
- 字面量：整数/长整数/浮点/字符串/`true`/`false`/`null`
- 一元运算：`-x`、`!x`、`&x`（取址目前仅支持变量）
- 二元运算：`+ - * /`、`== != < <= > >=`、`&& ||`（`&&/||` 为短路求值）
  - 数值提升（第一版）：
    - 有符号整数：按位宽提升到至少 `int(i32)`，再做运算
    - 无符号整数：按位宽提升到至少 `u32`，再做运算
    - 浮点：按精度提升（`f64 > f32 > f16/bf16`；`f16` 与 `bf16` 混合时提升到 `f32`）
  - 有符号/无符号混用（第一版）：算术与有序比较（`< <= > >=`）不允许隐式混用，需要显式 cast
  - 逻辑运算（Frozen）：
    - `!x`：对 `x` 做 truthiness 转换得到 `bool` 后取反
    - `x && y` / `x || y`：两侧都先做 truthiness 转换为 `bool`，并进行短路求值；结果类型为 `bool`
  - `string`（当前为 `i8*`）特殊规则（Frozen）：
    - `string + string -> string`：字符串拼接；`null` 视为字符串 `"null"`（例如 `"x" + null == "xnull"`）
    - `string == string` / `!=`：内容比较；若任一侧为 `null`，则仅当两侧都为 `null` 才相等
  - 指针比较（Frozen）：
    - 指针/引用类型（`ptr`、`string`、`map`、`Ref<T>` 等；过渡期 `&T` 亦同）仅允许 `==` / `!=` 比较（按指针值）
    - 有序比较（`< <= > >=`）对指针/引用类型无定义，视为编译错误
- 位运算/移位（Status: Implemented，高优先级）
  - 目标：提供 PRNG/bytes/量化等所需的确定性 bit-level 计算能力
  - 运算符：
    - 一元：`~x`（bitwise not）
    - 二元：`a & b`（and）、`a | b`（or）、`a ^ b`（xor）
    - 移位：`a << n`、`a >> n`
  - 类型规则：
    - 仅作用于整数类型：`i8/i16/int/long/isize/u8/u16/u32/u64/usize/byte`
    - `bool` 不参与位运算（`&&/||/!` 仍为逻辑运算）
    - 有符号/无符号混用禁止：需要显式 cast 统一到同一 signedness
  - 结果类型与提升：
    - 与算术同一套整数提升：按位宽提升到至少 `int(i32)` 或 `u32`，并保持 signedness
    - 结果按目标整型位宽截断（two's complement 位模式）
  - 移位规则（Frozen 预期语义，待实现）：
    - `n` 接受任意整数；先转换为无符号并进行掩码：`n = n & (bits-1)`（保证跨平台无 UB）
    - `>>`：对无符号整数为逻辑右移；对有符号整数为算术右移（符号扩展）
- 运算符优先级（Status: Implemented）
  - 预期顺序（从高到低）：一元（`~ ! - &`）> `* /` > `+ -` > `<< >>` > `&` > `^` > `|` > `&&` > `||` > `??`
- `??`（空值合并，Status: Implemented）：
  - 仅支持 `Option<T> ?? T -> T`，右结合；当左侧为 `None()` 时才会求值右侧（短路）
- 自增自减：`i++`、`i--`、`++i`、`--i`（当前主要用于整型变量）
- 调用：`f(a,b)`、成员访问/调用：`obj.field`、`obj.method(a,b)`
- 分组：`(expr)`
- 布尔上下文与 truthiness（Frozen）：
  - `if/while/do-while/for` 的条件、`!`、`&&`、`||` 都会将表达式做 truthiness 转换为 `bool`
  - falsey：
    - `false`
    - 数值类型：`0` / `0.0`
    - 指针/引用类型：`null`（包括 `string/ptr/map/Ref<T>` 等；过渡期 `&T` 亦同）
    - `Option<T>`：`None()`（等价于 `opt.isSome() == false`）
    - `any`：`nil` 与 `bool(false)`（其他 dynamic 值均为 truthy）
  - truthy：除上述 falsey 之外的所有值

### 5. 控制流（Status: Implemented）
- 条件：
  - `if cond { ... } else if cond { ... } else { ... }`（支持链式 `else if`）
  - 条件也支持括号：`if (cond) { ... }`
- `for`（C 风格，Status: Implemented）：
  - `for (init; cond; inc) { ... }`
  - `for init; cond; inc { ... }`
  - 说明：`init/cond/inc` 之间的分隔符在实现里使用 `;` token（换行也会被当作 `;`）
  - 初始化语句（Frozen）：
    - `for let i = 0; ...`：声明一个循环局部变量 `i`
    - `for i = 0; ...`：语法糖，等价于 `for let i = 0; ...`（在 `for` initializer 位置，`i=expr` 会被当作声明而不是对外层变量赋值）
- `for-in`（Status: Implemented）：
  - 目前仅支持 **map/array 变量**：`for v in m { ... }` / `for a,b in m { ... }`
    - `m` 必须是变量名，且类型为 `map`/`map<K,V>` 或数组 `T[]`/`T[N]`（当前不支持对任意表达式 for-in）
    - 若 `m` 是 `map`：
      - `for v in m { ... }`：`v` 绑定为 value（类型为 `any`/`tua_value`）
      - `for k,v in m { ... }`：`k` 绑定为 key（类型为 `any`/`tua_value`），`v` 绑定为 value（类型为 `any`/`tua_value`）
    - 若 `m` 是 `map<K,V>`：
      - 当 `V` 为标量（`int/long/float/double/bool/string`）：
        - `for v in m { ... }`：`v` 绑定为 value（类型为 `V`，按值拷贝）
        - `for k,v in m { ... }`：`k` 绑定为 key（类型为 `K`，按值拷贝），`v` 绑定为 value（类型为 `V`，按值拷贝）
      - 当 `V` 为非标量（`struct/map/array`，以及嵌套）：
        - `for v in m { ... }`：`v` 绑定为 value 的可写引用（类型为 `Ref<V>`，exclusive borrow）
        - `for k,v in m { ... }`：`k` 绑定为 key（类型为 `K`，按值拷贝），`v` 绑定为 value 的可写引用（类型为 `Ref<V>`，exclusive borrow）
      - 约束（Frozen）：当 `for-in` 产生的 `Ref<V>` 存活时，禁止对 `m` 执行可能使 element 地址失效的操作（如 `delete/clear/rehash/insert`）；由借用检查器保证
    - 若 `m` 是数组：
      - `for v in m { ... }`：`v` 绑定为 element（类型为 `T`）
      - `for v,i in m { ... }`：`v` 绑定为 element（类型为 `T`），`i` 绑定为 index（类型为 `int`）
    - 迭代顺序：map 未定义；array 为从 `0` 到 `len-1`
    - 循环期间修改 `m` 的行为未定义（建议不要在迭代时 mutate）
- 规划能力（Status: Planned）：
已实现（Status: Implemented）：
- `while cond { ... }`
- `do { ... } while cond`
- `break` / `continue`（在 `for/while/do-while` 内）
- `goto` / label（Lua 风格）：
  - label：`::name::`
  - 跳转：`goto name`
 - `Option<T>` 作为条件（Status: Implemented）：
   - `if opt { ... }` 等价于 `if opt.isSome() { ... }`

### 6. 函数（Status: Partial）
- 定义：
  - `fn name(a[:Type], b[:Type]) -> Type { ... }`
  - `fn name(a[:Type], b[:Type]) Type { ... }`（语法糖，省略 `->`）
  - 形参类型可省略；当前默认按 `int` 处理（完整类型检查见语义层规划）
- 形参修饰（Status: Implemented，语义已冻结）：
  - 语言不提供 `mut` 关键字；参数“可写/只读”用 `let/const` 表达
  - 默认：`fn f(x: T)` 等价于 `fn f(const x: T)`（只读借用，见 10.4）
  - 可写借用：`fn f(let x: T)`（独占借用，允许写字段/元素，见 10.4）
  - 所有权转移：`fn f(move x: T)`（取得所有权，可返回/存储，见 10.4）
  - 说明（Frozen）：参数名本身视为 `const` 绑定（不可 `x = other`），但当 `x` 为可写借用时允许改字段/元素（例如 `x.field = ...`）
- 外部声明（FFI，Status: Implemented）：
  - `extern fn name(a:Type, b:Type) -> Type`
  - `extern fn name(a:Type, b:Type) Type`（语法糖，省略 `->`）
  - `extern fn symbol(a:Type, b:Type) Type as localName`（语法糖）：声明外部符号 `symbol`，并自动生成模块内联的转发函数 `localName(...)` 以便避免命名冲突/递归歧义
  - 说明：
    - `extern fn` 只声明签名，不包含函数体；用于链接到 `tua_rt` 或系统库符号
    - `extern fn` 默认不导出（等价于模块内部绑定）；如需对外提供 API，请写一个普通 `fn` 包装后再导出
- 类型转换（Status: Implemented）：
  - `(T)expr`：显式转换（不检查/不报错）；用于数值类型之间的截断/扩展/浮点转换
  - `expr as T`：可检查转换，返回 `Option<T>`：
    - 整数窄化（如 `x:long; x as int`）：超范围返回 `None()`
    - 有符号/无符号互转：按目标范围检查（例如 `(-1) as u8` 返回 `None()`）
    - 浮点转整数（如 `x:double; x as int`）：NaN 或超范围返回 `None()`；否则按“向 0 截断”转换并返回 `Some(v)`
- 返回：
  - `return expr`
  - `return`（void）
- 多返回值（Status: Implemented）：
  - 函数返回类型可写成列表：`fn f() -> int, string { ... }`
  - `return` 支持多表达式：`return a, b`
  - 解构声明：`let a, b = f()` / `const a, b = f()`
  - 解构赋值：`a, b = f()`
  - 语义层补充（Status: Partial）：当 RHS 是本模块已知 `fn` 调用时，解构会按声明返回列表逐项推断/校验，并在 arity 不匹配时报错
  - 表达式上下文规则：当 `f()` 返回多个值时，在普通表达式上下文会自动取第一个值（例如 `let x = f()`）
  - `return f()` 转发规则：当当前函数是多返回值函数时，`return f()` 会直接转发 `f()` 的多返回结果
  - 错误码返回约定（Frozen）：
    - 当函数需要通过返回值携带错误码时，**错误码必须放在最后一个返回值**，且类型为 `int`
    - 约定：`err == 0` 表示成功；`err != 0` 表示失败（具体错误含义由函数约定/文档说明）
    - 返回顺序采用 Go 风格：`(out1, out2, ..., err)`；调用侧推荐写法：`let out, err = f(...)`
    - 示例：`fn readFile(path:string) -> string, int`
    - 标准库遵循该约定的例子：`std/bytes.Bytes.getU8(b: bytes, idx: long) -> int, int`（返回的 u8 以 `int` 表示；`err` 为最后一个 `int`）
    - 兼容性说明：历史 API 可能仍使用 `err` 作为第一个返回值，后续会逐步迁移到“`err` 最后”以统一与 Guard Block 语义
  - Guard Block（`? { ... }`，Status: Implemented）：
    - 语法：`callExpr ? { ... }`（仅允许对函数/方法调用使用）
    - 要求：`callExpr` 必须是**多返回**（返回值个数 >= 2），且最后一个返回类型必须为 `int`（错误码）
    - 语义：先执行 `callExpr`；若 `err != 0` 则执行 `{ ... }`；否则 Guard 表达式的返回值为 `callExpr` 的**除最后一个 `err` 之外的所有返回值**（即“丢弃尾部错误码”）
      - 在普通表达式上下文仍遵循“多返回取第一个值”规则（例如 `let x = f() ? { ... }` 取第一个非 err 返回值）
      - 在解构/`return f()` 转发等“多返回上下文”中，会保留多个非 err 返回值（例如 `let a,b = f() ? { ... }`，要求 `f() -> A,B,int`）
    - 控制流约束（Frozen）：`{ ... }` 必须包含 `return` / `break` / `continue` 之一，否则编译器报错（避免“吞掉错误码但继续执行”）
- 闭包/upvalue（Status: Implemented，第一版）：
  - 匿名函数表达式：
    - `fn (params...) -> T { ... }`
    - `fn (params...) T { ... }`（省略 `->` 语法糖）
    - 可省略返回类型，默认 `void`
  - 闭包值：匿名函数可赋值给变量：`let f = fn(x:int) -> int { return x + 1 }`
  - 捕获语义（Lua 风格）：闭包默认按引用捕获外层变量（读写共享），例如：
    - `let x = 0; let inc = fn() -> int { x = x + 1; return x }`
  - 调用：
    - `f(args...)`
    - 立即调用：`(fn(...) -> T { ... })(args...)`
  - 实现策略（当前 LLVM-JIT 版本）：
    - 逃逸分析（第一版，Status: Implemented）：仅将**被闭包捕获**的局部变量/参数放入 “heap box”，未捕获的局部仍保持栈分配（降低闭包带来的全局 boxing 开销）
    - box/env 使用引用计数 box（`tua_box_alloc/inc/dec`）；closure drop 时释放 env（无 GC）
    - 对于会“保存到未来再调用”的回调（如 `afterMs/post/*_async_cl`），runtime 在注册时会 retain 一份回调 env，并在触发/取消/错误路径 release（避免回调引用外层变量时产生悬垂）
  - 当前限制（后续规划补齐）：
    - 已支持 TS 风格函数类型 `(args) -> ret`（见下）；但完整类型检查/类型推断仍在规划中
- 内建函数（Status: Implemented）：
  - `print(x)` / `println(x)`（目前支持打印 int/long/double/bool/string 指针）
  - 格式化打印（Status: Implemented）：
    - `print("x {} y {}", a, b)` / `println("x {} y {}", a, b)`
    - 使用 `{}` 作为占位符；占位符数量必须与后续参数数量一致，否则编译错误
    - 当前限制：仅支持**字符串字面量**作为 format 参数（非字面量会编译错误）

#### 6.1 函数类型（TypeScript 风格，Status: Implemented）
- 语法形态：`(argType1, argType2, ...) -> retType`（或语法糖：`(argType1, ...) retType`）
  - 多返回写法：`(int) -> (int, string)`（函数类型的多返回必须用括号分组，避免与外围语法的 `,` 歧义）
- 用法示例：
  - 变量类型标注：`let f: (int, int) -> int = fn(a:int, b:int) -> int { return a + b }`
  - 多返回：`let g: (int) -> (int, string) = fn(x:int) -> int, string { return x, "ok" }`
  - 作为参数：`fn apply(x:int, f:(int)->int) -> int { return f(x) }`
  - 作为返回值：`fn makeAdder(n:int) -> (int)->int { return fn(x:int)->int { return x + n } }`，并支持 `makeAdder(5)(10)` 这种“返回闭包再调用”的写法

#### 6.2 命令行参数（Status: Implemented）
- 内建全局变量：`ARGV: string[]`
- 下标约定：
  - `ARGV[0]`：程序名（等价于 C 的 `argv[0]`）
  - `ARGV[1]`：第一个用户入参

### 7. struct / object / enum（Status: Partial）

#### 7.1 struct（Status: Implemented）
- `struct T { field: Type = initializer ...; fn method(...) ... }`
- 表达式 `T(...)` 表示构造：返回 **T 值**
  - 构造参数按字段声明顺序依次赋值（多余参数报错）
  - 字段初始化优先级（Frozen）：构造参数 > 字段 `initializer` > 零值
  - 零值（Frozen）：数值为 `0`/`0.0`，`bool` 为 `false`，指针/引用为 `null`，聚合类型按字段递归零值
- 表达式 `T{ field: expr, ... }` 表示按字段名构造：返回 **T 值**
  - 字段可以乱序/可省略；未提供的字段同样按 `initializer` / 零值规则初始化
  - 未知字段名或重复字段名会报错
- move / 引用语义（Frozen）：
  - `let b = a`：移动 `a -> b`（所有权转移），`a` 之后不可再用（编译期错误）
  - 在以下位置使用 move-only 变量同样会发生移动（并把源变量置为 moved-from）：
    - **struct 构造/字面量**：`T{ f: x }`
    - **字段赋值**：`obj.f = x`
    - **下标赋值**：`m[k] = x` / `a[i] = x`
    - **map/array 字面量**：`{ "k": x }` / `[x]`
  - 需要复制时必须显式：`a.clone()`（Planned：默认深拷贝字段；并对资源字段做正确的所有权处理）
  - `let r = &a` / `const r = &a`：取 `a` 的引用（类型层面是 `Ref<T>`；过渡期可写 `&T`；LLVM 层面是 `T*`）
- 引用读取（Status: Implemented）：
  - `r.get()`：读取 `Ref<T>` 指向的值（按值返回；对标量是拷贝，对指针/句柄是复制句柄）
  - 不提供 `*r` / `*r = v`（避免引入额外的指针解引用/写入语法；对容器/对象的修改应通过其自身 API/字段完成，或通过原容器写回）
- `get()` 的语法糖（Status: Implemented）：
  - 当接收者是 `Ref<T>` 变量时：`r.get().x` / `r.get().x = v` 等价于 `r.x` / `r.x = v`
- 字段访问：`p.x` / `p.x = v`（接收者目前仅支持变量；若变量是 `Ref<T>`（过渡期 `&T`）则会间接到指向的对象）
- `this`（Frozen）：
  - 仅在 `struct` 实例方法中可用，类型恒为 `Ref<T>`（过渡期 `&T`）
  - `this` 绑定不可重新赋值；字段是否可写取决于接收者是否为 `let` 绑定（`const` 接收者上写字段为编译错误，Planned：由借用检查器统一判定）
  - 隐式字段访问/赋值（Frozen）：在 `struct` 实例方法中，若标识符既不是局部变量/参数/模块顶层绑定（函数/类型/对象等），且 `this` 上存在同名字段，则将 `x` / `x = v` 分别解析为 `this.x` / `this.x = v`（便于写 `x` 代替 `this.x`）
- 实例方法调用（Frozen）：
  - `p.m(a,b)` 会被编译为 `T__m(recv, a, b)`，其中 `recv` 恒为 `Ref<T>`（过渡期 `&T`）
    - 若 `p` 为 `T`（值），则 `recv = &p`
    - 若 `p` 为 `Ref<T>`（引用值），则 `recv = p`
- 嵌入与提升（Frozen，Status: Implemented，Go 风格但 **无子类型**）：
  - 目的：复用字段/方法，但不引入 `extends`/继承链；不提供隐式 upcast；保持布局可预测
  - 语法：在 `struct` 字段列表中允许嵌入另一个 `struct` 类型：
    - `...Base`：嵌入 `Base`
    - `...Base as base2`：嵌入并显式指定字段名（用于命名冲突/多次嵌入）
  - 降糖（Frozen）：
    - `...Base` 等价于新增一个字段 `base: Base`，并将该字段标记为 `embedded`
    - 默认字段名规则：将类型名首字母小写（`Base -> base`）；若重名必须使用 `as`
  - 字段提升（Frozen）：
    - 对表达式 `x.f`：若 `x` 的类型 `T` 没有字段 `f`，则在 `T` 的 `embedded` 字段中递归查找 `f`
    - 若唯一匹配，则将 `x.f` 解析为 `x.<embeddedPath>.f`（例如 `x.id` -> `x.base.id`）
    - 若存在多个匹配则报错（歧义），要求显式写出路径（如 `x.base1.id` / `x.base2.id`）
    - 若 `T` 自己存在同名字段 `f`，则永远优先使用 `T.f`（遮蔽 embedded）
  - 方法提升（Frozen）：
    - 对调用 `x.m(...)`：若 `T` 上没有实例方法 `m`，则在 `embedded` 字段类型中递归查找 `m`
    - 若唯一匹配，则将 `x.m(...)` 解析为 `x.<embeddedPath>.m(...)`，并以嵌入字段的地址作为 `recv`
    - 歧义/遮蔽规则与字段提升一致
  - 与 trait 的关系（Planned，扩展点）：
    - promotion **不等于** “自动实现 trait”：嵌入只影响字段/方法解析与 receiver 重写，不产生子类型/隐式 upcast
    - Status: Implemented（v0）：trait 满足性按 trait 规则检查；是否“满足”取决于方法集是否存在（可包含 promoted methods）
    - Status: Implemented（v1）：静态分发采用泛型单态化（例如 `fn f<T: Trait>(x: T)`）
    - Status: Implemented（v2，第一版）：动态分发采用 trait object（fat pointer / vtable）；`TraitName` 可作为值类型使用（不要求显式 `dyn` 关键字）
  - 重要：`Child` **不是** `Base` 的子类型；`let b: Base = child` 是类型错误（Planned：完善类型检查报错信息）
- `init/deinit`（Status: Partial）：
  - 已支持解析 `init(){...}` / `deinit(){...}` 为方法
  - 自动调用时机/析构语义尚未定义（见内存模型规划）

#### 7.2 trait（Status: Implemented v2，静态 + 动态）
- 目标：提供“结构体满足某能力”的编译期契约；支持 **静态分发**（泛型单态化）与 **动态分发**（trait object / vtable）
- 声明：
  - `trait TraitName { fn m(a: T, ...) -> R }`
  - trait 方法目前仅支持 **签名**（不支持默认实现/方法体）
- 实现声明（满足性检查 + 可选提供方法体）：
  - `impl TraitName for StructName {}`：允许空实现块（仅触发检查/记录 impl pair，用于泛型 bounds）
  - `impl TraitName for StructName { fn m(...) ... }`：
    - 只能实现 trait 中声明的方法（多余方法为编译错误）
    - 方法体会被编译为 trait 命名空间下的函数：`StructName__TraitName__m`（不会加入 `StructName` 的固有方法集）
    - 该方法体用于静态/动态分发（见下）；对具体类型调用 `x.m()` 默认仍解析为固有方法（trait 方法体只在 trait 分发路径生效）
- 满足性（Frozen）：
  - 对每个 trait 方法 `m`：
    - 若 `impl Trait for Struct {}` 内提供了 `m` 的方法体，则视为已实现
    - 否则要求 `Struct` 的 **固有方法集**存在 `m`（含 promoted methods），且签名一致
  - 固有方法集：
    - `struct StructName { fn ... }` + `impl StructName { ... }`
    - 以及通过 `...Base` 的方法提升得到的 promoted methods
  - 遮蔽/歧义规则与“方法提升”一致：自身同名优先；多个 embedded 命中时报歧义错误
- v1 静态分发（Implemented）：
  - 泛型 bounds：`fn f<T: Trait>(x: T)` 在实例化时要求存在 `impl Trait for ConcreteType {}`
  - 在泛型函数体内，若 `x` 的静态类型为 `T` 且 `T: Trait`，则 `x.m(...)` 按 trait 分发：
    - 若存在 `ConcreteType__Trait__m`，则调用该函数（trait impl 方法体）
    - 否则回退到固有/提升方法解析（调用 `ConcreteType__m` 或 promoted method）
  - 约束：在 `T: Trait` 上调用的方法必须在 `Trait` 中声明，否则为编译错误
- v2 动态分发（Implemented，第一版；无 `dyn` 关键字）：
- `TraitName` 在类型位置表示 trait object（接口值）：一个 fat pointer `{ data: ptr, vtable: ptr }`
  - vtable 布局（冻结 ABI，第一版）：`{ drop(i8*), m0(i8*, ...), m1(i8*, ...), ... }`（方法顺序按 trait 声明顺序）
  - 对 `let x: TraitName = S{...}`：
    - 要求存在 `impl TraitName for S {}`
    - 生成 owning trait object：把 `S` box 到堆上，`drop` 负责递归 drop 后 `free`
  - 对 `fn f(x: TraitName) { ... }` 的调用：
    - 传入具体 `S` 时，默认构造非 owning view（`data = &s`），调用结束不释放
    - 传入 `move s` 时，构造 owning object（box 到堆上），由被调函数的作用域 drop 释放
  - 方法调用：`x.m(...)` 会通过 `vtable` 做间接调用
- 自动“静态优先 / 动态降级”（Implemented，第一版）：
  - 若存在 `fn f(x: TraitName) { ... }`，编译器会额外注册一个等价的合成泛型模板 `fn f<T0: TraitName>(x: T0) { ... }`
  - 调用 `f(S{...})` 时若可推断出具体类型，则优先走静态单态化；当参数实参本身是 trait object（或无法推断）时回退到动态版本
- `Ref<TraitName>`（例如来自 `map.get()`）支持直接调用 trait 方法：`r.m()` 等价于对 `r.get()` 得到的 `{data,vtable}` 做一次 vtable 分发（实现上为编译期重写）
- 当前限制（Planned）：
  - trait 泛型（`trait Trait<T>`）、多重 bound、默认方法、`dyn` 显式关键字与 object-safety 规则仍待完善

#### 7.3 impl（Status: Implemented，Rust 风格）
- 语法：
  - `impl Type { fn func(...) ... }`
    - `Type` 可以是用户 `struct`，也可以是内建类型：`bytes`/`map`/`ptr`、标量（`int/long/string/bool/...`）、以及数组别名 `array<T>`（等价于 `T[]`）
  - 泛型 impl（Partial）：
    - `impl<T> Option<T> { ... }` / `impl<T> Slice<T> { ... }` / `impl<T> array<T> { ... }`
- 语义：
  - `impl TypeName` 中的 `fn` 会作为 `TypeName` 的实例方法（与 `struct TypeName { fn ... }` 等价，若该类型可声明）
  - 这些方法内允许使用 `this`：
    - 对 `struct`：`this` 的类型恒为 `Ref<StructName>`（过渡期 `&StructName`）
    - 对内建 handle 类型（例如 `bytes`/`map`）：`this` 的类型为 `TypeName`（handle 本身即为引用语义）
  - 若同名方法重复定义（struct 内 vs impl 块，或多个 impl），视为编译错误（第一版）
  - 与 intrinsic/builtin 的关系（Planned，逐步下放）：
    - 方法调用解析优先级：trait 分发（泛型 bound 场景）> 固有方法（`struct/impl`）> 编译器 intrinsic（仅作为缺省/fallback）
    - 目标：`std` 通过 `impl bytes/map/...` 提供大多数“自带方法”，编译器只保留少数必须内建的 intrinsic（例如 slice 构造/边界检查等）

#### 7.4 object（Status: Implemented）
- `object O { fn f(...) ... }`
- `O.f(a,b)` 编译为 `O__f(a,b)`（静态方法）
- 在 `object O` 的方法体内允许省略 `O.`：
  - 非同名调用：`g(a,b)` 会优先解析为同一个 `object` 内的 `O.g(a,b)`；若不存在该方法，则按普通规则继续解析为模块顶层函数/`extern fn`/导入别名等
  - 同名调用（递归/包装场景）：在 `fn f(){ ... }` 内写 `f()` 会先按普通规则解析模块顶层/`extern fn` 的 `f`；若找不到，再回退解析为 `O.f()`（递归）

#### 7.5 enum（Status: Implemented）
- 声明：`enum E { A, B = 10, C, D = "raw" }`
- 说明（Frozen）：当前 `enum` 在运行时不引入“独立枚举值类型”；`E` 是命名空间，`E.A` 的值直接是 `int` 或 `string`（取决于 enum 模式）
- 模式：
  - **int-tag enum**：只出现 `= 123`（或全部省略），则 `E.A` 返回 `int tag`
  - **string-tag enum**：只出现 `= "raw"`（或省略），则 `E.A` 返回 `string tag`
  - 不允许同一个 enum 同时混用 int 与 string（会报错，best-effort 继续）
- int-tag 规则：
  - `E.A` 返回 **int tag**（按声明顺序从 `0` 递增；若显式 int 则以该值为当前值并继续递增）
  - 自动生成：`E.toString(tag:int) string`（返回 variant 名字；默认 `"Unknown"`）
- string-tag 规则：
  - `E.A` 返回 **string tag**（若显式 `"raw"` 则用 raw；否则用 variant 名字）
  - 自动生成：`E.toString(tag:string) string`（identity）

### 8. 模块（Status: Implemented）
- 默认全部导出；声明前加 `private` 则不导出（`private fn/struct/object/enum`）
- 语法：
  - `import "relative/or/absolute/path.tua"`：执行模块，并把该模块所有可导出符号引入到当前作用域（import-all）；同时自动生成默认命名空间别名（见下）
  - `import "path.tua" as ns`：执行模块，但不把符号直接引入当前作用域；后续通过 `ns.Name` 访问（命名空间导入）
  - `import A,B,C from "path.tua"`：把导出的符号引入到当前模块作用域
    - 支持重命名：`import A as A1, B as B1 from "path.tua"`
    - 兼容旧写法：`from "path.tua" import A,B,C`
      - 同样支持 `as`：`from "path.tua" import A as A1, B`
- 规则：
  - 相对路径以当前文件所在目录为基准
  - 若省略后缀，会自动补 `.tua`
  - 标准库路径：以 `std/` 开头的导入路径会从标准库目录解析（见 `std/README.md`）
  - 模块缓存：同一模块只会被加载/编译一次（仅声明；无顶层可执行代码）
  - 循环依赖：允许；由于模块顶层无可执行语句，不存在“初始化顺序”问题
  - 模块顶层限制（Frozen，Status: Implemented）：
    - 模块顶层只允许**声明语句**：`import/from`、`fn`、`struct`、`object`、`enum`、`trait`、`impl`、`impl Trait for Struct`（以及 `private` 修饰的这些）
    - 禁止模块顶层出现任何可执行语句：`let/const`、表达式语句（含顶层 `assert`/`print`）、`if/for/while/do/goto/label/return/break/continue` 等
    - 约束目的：消除模块级初始化时机/顺序问题；所有执行都必须显式放进 `fn main(...) {}` 或其他函数中
  - 程序入口（Status: Implemented）：
    - 程序启动时，不执行任何模块顶层语句（因为模块顶层仅允许声明）；随后（可选）调用一次“用户 `main`”作为入口函数
    - 入口默认选择：**编译命令行指定的 source file 所属模块**内的 `fn main`（若其签名匹配下述允许形式）
    - 允许的入口签名（Frozen，v1）：
      - `fn main() {}`
      - `fn main(args: string[]) int {}`：其返回的 `int` 作为进程退出码（0=成功）
    - `fn main() int {}` 不作为入口（它只是一个普通函数，可被显式调用）
    - 当存在多个入口候选（例如入口模块不包含 `main`，但导入模块包含），需要使用 CLI `--entry <module>` 指定入口模块
      - `--entry` 的 `<module>` 使用与 `import "..."` 相同的路径解析规则（相对入口模块目录；支持 `std/`）
    - `main` 是保留标识符（Frozen，v1）：
      - 仅允许用于上述入口函数名 `fn main ...`
      - 任何其他场景（变量/参数/struct/object/enum/trait 名称，或 `fn main` 但签名不匹配）均为编译错误
- 当前限制：
  - 只支持字符串字面量路径（不支持表达式路径）
  - 仅支持导入 `fn/struct/trait/object/enum`；不支持导入模块级变量
  - 任何导入形式（import-all / named import / namespace import）若引入同名符号（或同名命名空间别名）冲突，会直接报错并停止（可用 `as` 或命名空间导入来消歧义）
  - 命名空间导入的成员访问：已支持 `ns.F(...)`、`ns.StructCtor(...)`、`ns.Obj.method(...)`、`ns.Enum.Variant`
  - 默认命名空间别名（Implemented）：
    - 对于 `import "path"`（无 `as` / 无 `from`），编译器会用 **路径最后一段文件名（去掉 `.tua`）** 生成一个默认命名空间别名
      - 例：`import "std/bytes"` 可用 `bytes.Bytes.xxx` 访问
    - 若文件名不是合法标识符，会做最小的“标识符化”处理（非法字符替换为 `_`；首字符为数字或**保留字/内建类型 token**则前缀 `_`）
      - 例：`import "modules/string"` 会生成 `_string` 命名空间别名（因为 `string` 不是合法标识符 token）
    - 若该默认别名与当前作用域已有符号冲突，会报错并提示用 `as` 重命名

### 8.5 泛型与单态化（Status: Partial，高优先级）
- 目标：提供通用类型参数（函数/struct/trait），并采用 **编译期单态化**（monomorphization）实现零成本抽象
- 术语：
  - **泛型定义**：带类型参数的 `fn/struct/trait`
  - **实例化**：在某个具体类型实参（如 `T=int`）下生成专用版本
  - **单态化**：把泛型调用在编译期展开成对实例化版本的直接调用
- 第一版建议切片（Status: Implemented v0/v1）：
  - 仅支持 **函数泛型**：`fn f<T>(...) ...`
  - 调用处支持 **显式类型实参**：`f<int>(1)`
  - v0.5 语法糖：支持 `f(1)` 的类型实参推断（当可唯一推断时）；否则要求写出显式 `f<T>(...)`
    - 当前推断限制（Implemented）：仅对形如 `x: T` 或 `x: Ref<T>`（过渡期 `&T`）的参数做推断；当 `T` 仅出现在 `Option<T>`/`map<K,T>` 等嵌套位置时需要显式写出 `f<T>(...)`
  - 实现方式：每组类型实参生成一个单态化实例函数（`__G__...` 形式的内部符号名），并缓存复用
- v1 约束（Status: Implemented）：
  - 语法：`fn g<T: Trait>(x: T) -> ...`
  - 语义：实例化 `g<Concrete>(...)` 时，要求存在 `impl Trait for Concrete {}`（否则编译错误）
- 当前限制（Planned）：
  - bounds 限制：目前仅支持单个 trait bound（`T: Trait`），且要求显式 `impl Trait for Concrete {}`（尚未支持结构化/自动满足或多重 bound）
  - 暂不支持泛型 `struct` / 泛型 `trait`
  - 暂不支持实例方法泛型（`x.m<T>(...)`）
  - `ns.f<T>(...)`（命名空间导入）支持；其余复杂成员路径的泛型调用后续补
- 关键语义（Planned）：
  - 每组类型实参只生成一次实例（缓存）
  - 约束不满足时报错（在调用点或实例化点给出清晰诊断）
  - 递归/循环实例化必须可诊断并终止（避免无限展开）

### 9. 数据结构、Option 与空值（Status: Partial）

#### 9.1 `Option<T>`（Status: Implemented）
- 目的：显式表达“可能不存在”的值，避免把 `null` 扩散到所有类型（Rust 风格）。
- 类型：`Option<T>` 为命名泛型类型（第一版，仅用于编译期/LLVM 类型层）。
- 构造：
  - `Some(x)`：产出 `Option<T>`
  - `None()`：产出 `Option<any>`（`Option<tua_value>`），可通过赋值/显式类型让其转成 `Option<T>`
- 方法：
  - `opt.isSome() -> bool`
  - `opt.isNone() -> bool`
  - `opt.unwrap() -> T`（若为 `None`，运行时错误）
  - `opt.unwrapOr(default:T) -> T`
- `if let`（Status: Implemented）：
  - 语法：`if let Some(x) = opt { ... } else { ... }`
  - 目前仅支持 `Some(<identifier>)` 模式；`x` 仅在 then 分支可见
  - 等价于：先求值 `opt`，若 `isSome()` 则在 then 分支内绑定 `x = opt.unwrap()` 并执行；否则走 else（若存在）
- 运算：
  - `opt ?? default`：当 `opt` 为 `Some(v)` 时返回 `v`，否则返回 `default`
    - 左操作数必须是 `Option<T>`；`1 ?? 0` 这类写法会编译期报错
- 相等性：
  - 允许 `Option<T> == Option<T>` / `!=`：`None == None`；`Some(a) == Some(b)` 比较 payload（`string` 为内容比较）
  - 禁止 `Option<T>` 与非 `Option` 直接比较（例如 `opt == 1`）；需用 `unwrap()/isSome()` 或与 `Some(...)` 比较
  - 禁止把 `Option<T>` 直接赋给 `T`（例如 `let x:int = m["a"]`）；需用 `unwrap()` 或 `??` 提供默认值

#### 9.2 `map` / `map<K,V>`（Status: Partial）
- 目标：先提供“可用的键值容器”，再逐步补齐语义层与内存模型。
- 类型形态：
  - `map`：不带类型参数的 map（value 在 runtime 中以 `tua_value` 存储）
  - `map<K,V>`：带类型参数的 map（第一版）
    - `K` 当前仅允许：`string/int/long`
    - `V` 允许：
      - 标量（Copy）：`int/long/double/bool/string`
      - 复合/引用型（move-only）：`struct`、`map`、数组 `T[]/T[N]`（以及它们的嵌套组合）
      - trait object（move-only）：`TraitName`（见 7.2，运行时为 `{data,vtable}`）
- 字面量（Status: Implemented）：
  - 语法：`{ key: value, ... }`
  - `key` 仅允许常量字面量：`int/long/string`
  - 重复 key：后者覆盖前者
  - 允许尾逗号：`{ "a": 1, }`
  - `value` 为 move-only 变量时会发生移动（`{ "k": x }` 会移动 `x`）；Copy type 则按值复制
  - 当 `V` 为 trait object（`map<K, TraitName>`）时：
    - 允许写入具体 `struct` 值（例如 `Girl{...}`），前提是存在 `impl TraitName for Girl {}`；编译期会把 `Girl` box 成 owning trait object 再存入 map
- 读取（Status: Implemented）：
  - 对 `map`（无类型参数）：`m[k] -> Option<any>`（即 `Option<tua_value>`）
  - 对 `map<K,V>`：
    - 当 `V` 为标量（Copy：`int/long/double/bool/string`）：`m[k] -> Option<V>`
    - 当 `V` 为 move-only（包含但不限于 `struct`、`map`、数组 `T[]/T[N]`、`bytes`、trait object 等）：`m[k]` **不提供按值读取**；编译期报错并提示使用 `get/getMut`（避免隐式产生第二个 owner 导致 double-free/UAF）
  - key 不存在返回 `None()`；不再提供 `v,ok = m[k]` 多返回形式
  - 若 `m` 为 `null/未初始化`，读取会触发运行时错误（带行号）
- 写入（Status: Implemented）：
  - `m[k] = v`
  - 若 `m` 是变量且当前为 `null/未初始化`，会自动初始化为新 map 再写入
  - 对 `map<K,V>`：写入时会做静态检查（key/value 类型不匹配会编译错误；禁止写入 `null`）
  - 当 `V` 为 trait object（`TraitName`）时：允许写入具体 `struct` 值并自动 box（同上）
- 内建方法（Status: Implemented）：
  - `m.len() -> int`
  - `m.hasKey(k) -> bool`
  - `m.delete(k) -> bool`
  - `m.clear() -> void`
- 借用读取（Status: Partial，配合 `Ref<T>`）：
  - 目标：支持“零拷贝读取/原地修改”，并为 `bytes/slice`、模型权重 mmap 等场景铺路
  - `m.get(k) -> Option<Ref<V>>`：返回 value 的共享只读引用（shared borrow）
  - `m.getMut(k) -> Option<Ref<V>>`：返回 value 的独占可写引用（exclusive borrow）
  - 兼容性：旧方法名 `getRef/getRefWrite` 已移除（编译期报错并提示改为 `get/getMut`）
  - 约束（Frozen）：当 `Ref<V>` 存活时，禁止对 `m` 执行可能使 element 地址失效的操作（如 `delete/clear/rehash/insert`）；由借用检查器保证
  - 当前实现（Status: Implemented）：
    - 对 `map`（无类型参数）：返回 `Option<Ref<any>>`（即指向 runtime `tua_value` 的引用）
    - 对 `map<K,V>`：
      - 当 `V` 为 move-only（`struct/map/array`）时：返回 `Option<Ref<V>>`（可用于字段写/调用容器方法）
      - 当 `V` 为 trait object（`TraitName`）时：返回 `Option<Ref<TraitName>>`，允许在 `Ref` 上直接调用 trait 方法（零拷贝分发）
      - 当 `V` 为标量（Copy）时：`get/getMut` 暂不支持（第一版），请用 `m[k]`
  - 用法（Status: Implemented）：
    - `let p = m.get("k").unwrap(); let v = p.get()`（读 `tua_value`）
    - 不提供 `*p = v` / `p.set(v)` 原地写回（第一版）；需要写回时用 `m[k] = v`（且要求没有存活的 element ref）
- 遍历（Status: Implemented）：
  - `for v in m { ... }` / `for k,v in m { ... }`：见 5（`for-in`）
- key 规范化（Status: Implemented）：
  - `int` 与 `long` 作为 key 归一到同一 int64 域，因此 `1` 与 `1L` 视为同一个 key
- 待补齐（Status: Planned）：
  - 更完整的语义层：让非字面量表达式也能参与 `map<K,V>` 推断/检查（避免依赖 LLVM 类型）
  - 支持可空 value 的明确语义（例如 `map<K, Option<V>>` 的存储/区分规则）

#### 9.2.1 数组：`T[]` / `T[N]`（Status: Implemented）
- 类型：
  - `T[]`：动态数组
  - `array<T>`：动态数组的具名别名（等价于 `T[]`，方便在泛型/impl 语法中使用）
  - `T[N]`：定长数组（`N` 为编译期常量整数）
  - 数组为“引用型容器”（运行时为句柄/指针）：`let b = a` 移动句柄（所有权转移），`a` 之后不可再用；`clone()` 会深拷贝
- 字面量：
  - `[]` / `[e1, e2, ...]`：数组字面量（动态数组）
  - `{...}` 也可用于数组字面量（见 9.2.2）
  - 定长数组初始化（Frozen）：
    - `let a:T[N] = {x}`：填充语法糖，等价于把所有元素都初始化为 `x`
    - `let a:T[N] = {x1, x2, ...}`：按顺序初始化前 `k` 个元素，其余元素为零值
    - `let a:T[N]`：默认初始化为全零值数组
- 操作：
  - 下标读写：`a[i]` / `a[i] = v`（默认带 null/bounds 检查；越界或对 `null` 访问会触发运行时错误）
  - `a.len() -> int` / `len(a) -> int`
  - `a.clone() -> T[]`
  - `a.push(v)`：仅对动态数组 `T[]` 有效；对定长数组为编译错误

#### 9.2.2 Brace literal：`{...}`（Status: Implemented）
`{...}` 在语法上是 brace literal，根据内容与上下文分辨为 map 或 array：
- 非空 `{...}`：
  - 若第一个元素形如 `<constKey> : <expr>`，则为 map 字面量（`constKey` 仅允许 `int/long/string` 字面量）
  - 否则为数组字面量（元素为一般表达式）
- 空 `{}`：
  - 语法上保持为“未定型 brace literal”，由类型上下文决定
  - 若存在期望类型且为数组类型，则 `{}` 表示空数组
  - 否则默认 `{}` 表示空 map

#### 9.2.3 `bytes` 与 `Slice<T>`（Status: Partial；`bytes` 已实现，`Slice<T>` v1 已实现）
- 目标：为二进制解析（GGUF/量化/bit-pack）与大文件 mmap 提供“零拷贝读取”的基础类型。
- `bytes`（拥有所有权，move-only，Implemented）：
  - 运行时表示：`tua_bytes*`（见 `src/tua_bytes.h`）
  - 内容为原始字节序列，可包含 `0`，不要求 UTF-8/NUL 结尾
  - drop（RAII）：作用域结束/覆盖赋值/`return` 路径自动释放（free 或 munmap）
  - API（v0，先在 `std` 侧落地；后续可加 `b.len()/b.get()` 语法糖）：
    - `std/bytes.tua`：`Bytes.mmapFile/len/isReadonly/getU8/setU8/copy/fromString/new` + `readU16LE/readU32LE/readU64LE/readI16LE/readI32LE/readI64LE`
    - `tua_bytes_mmap_file/tua_bytes_len/tua_bytes_get_u8/tua_bytes_set_u8/tua_bytes_copy/tua_bytes_free`
    - 便捷（FFI，Implemented）：`tua_str_from_bytes_copy(b, off, len) string`（拷贝字节区间生成 NUL 结尾 string；用于 JSON/GGUF 等解析）
    - bit-cast（FFI，Implemented）：`tua_f32_from_u32_bits(bits) double` / `tua_f64_from_u64_bits(bits) double`（用于 GGUF 等二进制格式）
- `std/io.tua`（基于 `bytes` 的最小 Reader 抽象，Status: Implemented v0）：
  - `trait Reader { fn read(dst: bytes, off: long, n: long) -> int, long }`（`n==0 && err==0` 表示 EOF）
  - `Cursor`：内存 buffer reader（拥有 `bytes` 所有权，读取会前进 position）
  - `BufReader`：对任意 `Reader` 做缓冲（v0 为字节拷贝实现；后续在 `tua_rt` 加 memcpy 优化）
  - 工厂函数：`Io.cursorFromBytes(move b)` / `Io.bufReader(move inner, cap)`
  - `MmapFileReader`：mmap-backed Reader（拥有 `bytes` 映射；自动 unmap），工厂 `Io.mmapFileReader(path)`
- `Slice<T>`（借用视图，指针 + 长度，Implemented v1）：
  - 语义：`Slice<T>` 是借用值（非 owning），其可写性由绑定的 `const/let` 决定（与 `Ref<T>` 一致）
  - 生命周期/借用检查（NLL v0，语句级）：当 `Slice<T>` 存活时，禁止对其 owner 执行 move/可能失效的写；借用在“最后一次使用”后结束
  - 运行时布局（ABI）：`{ T* data, long len }`
  - 方法（第一版）：
    - `s.len() -> long`
    - `s.get(i: long) -> T`（当前仅保证 `T` 为标量时可用；越界触发运行时错误）
    - `s.set(i: long, v: T) -> int`（需要 `s` 为可写绑定；当前仅支持标量元素；越界触发运行时错误）
    - `a.slice(off: long, n: long) -> Slice<T>`（数组切片；越界触发运行时错误）
    - `b.slice(off: long, n: long) -> Slice<byte>`（bytes 切片；越界触发运行时错误；写回通过 `tua_bytes_set_u8` 返回 err，支持 readonly mmap 失败返回）
  - 当前实现（v1）：
    - `bytes.slice(off, n) -> Slice<byte>`
    - `array.slice(off, n) -> Slice<T>`
    - `Slice<T>.len/get/set`（`set` 仅标量元素；`Slice<byte>` 写回通过 `tua_bytes_set_u8` 返回 err）
    - `Slice<T>` 当前按 **move-only** 处理（避免隐式 copy 导致 borrow 生命周期变长且难以静态追踪）

#### 9.2.4 `std/utf8` 与 `json`（Status: Implemented v0）
- `std/utf8`：
  - `Utf8.isValid(b: bytes) -> bool`
  - `Utf8.decode1(b: bytes, off: long) -> int, int, long`（err, codepoint, sizeBytes）
  - `Utf8.isBoundary/clampBoundaryBefore/clampBoundaryAfter`（文本切片安全边界）
- `json`（包模块，位于 `packages/json/json.tua`，推荐 `import "json"`）：
  - `Json.parse(s: string) -> int, any`
  - `Json.parseBytes(b: bytes) -> int, any`
  - 产出：`map` / `any[]` / `string` / `long` / `double` / `bool` / `null`

#### 9.2.5 `packages/llm/gguf`（Status: Implemented v0）
- 归属：`llm` 层（应用包），不引入编译器/运行时特判
- API（v0）：
  - `GGUF.parseBytes(move b: bytes) -> int, GgufFile`
  - `GGUF.parseMmap(path: string) -> int, GgufFile`
- 解析范围（v0）：magic/version/n_tensors/n_kv + KV metadata（值类型：u8/i8/u16/i16/u32/i32/u64/i64/f32/f64/bool/string/array）
- 暂不做：tensor infos + tensor data layout（见 ROADMAP 的 GGUF v1）

#### 9.3 `null`（Status: Implemented）
- `null` 是“指针空值字面量”（当前实现中等价于 `i8*` 的空指针），用于表示“无指针/无句柄/未初始化引用”等场景
- 赋值与类型（Frozen）：
  - `null` 可赋值给 `string`、`ptr`、`map`、`Ref<T>` 等指针/引用类型（过渡期 `&T` 亦同）
  - `nil` 当前不是关键字；内部的 dynamic `nil` 仅用于 `any`/`tua_value`（例如 `None()` 的 payload），不直接暴露为语法字面量
- 运算（Frozen）：
  - truthiness：`null` 为 falsey（例如 `!null == true`）
  - `string + null`：`null` 视为字符串 `"null"`（例如 `"x" + null == "xnull"`）
  - `string == null`：当且仅当该 `string` 值为 `null` 指针时为 `true`（`!=` 反之）
  - 非 `string` 的指针/引用与 `null` 比较：`p == null` 等价于“指针值为 0”（`!=` 反之）

#### 9.4 运行时错误定位（Status: Partial）
- 运行时错误会输出 best-effort 行号（用于定位 `unwrap(None)`、对 `null map` 读写等）。
- 规划：统一诊断格式为 `file:line:col: ...` 并增加栈回溯。

### 10. 内存模型（Status: Partial，高优先级）
目标：**安全第一、无 GC、无手动内存管理**，并尽量做到“默认安全、特殊显式、性能可预测”。

#### 10.0 当前实现覆盖（Status: Partial）
- 已实现（编译期）：
  - move-only（`struct/map/array`）：`let b = a` / `a = b` 会转移所有权；move 后再使用为编译错误
  - `const` 不可修改：禁止重绑定、字段写、下标写、`++/--`
  - 借用检查（第一版）：`let r = &x` 独占可写借用；`const r = &x` 共享只读借用；禁止冲突借用；被借用期间禁止 move/写
  - 引用/借用值逃逸检查（第一版）：禁止 `return &local`；禁止把 `&local` 或借用值（如 `Ref/Slice`）赋给外层变量（包括经由中间变量转手）
- 已实现（运行时/编译器插桩，第一版）：
  - `map/array/bytes/trait object/closure` 自动释放：作用域结束、覆盖赋值、`return` 路径会 drop；move 会把源 slot 置 `null`（避免 UAF/double-free）
- 未实现（Planned）：
  - `struct deinit`/closure env 的自动 drop；deep drop（容器元素级析构）；跨线程数据竞争规则

#### 10.1 总体原则（Frozen）
- 编译期阻止所有内存安全问题：悬垂引用/重复释放/数据竞争
- 不引入运行时 GC（无 stop-the-world）
- 语言层不提供 `free/delete` 给“语言拥有的值”（如 `struct/map/array`）；FFI 返回的裸指针/缓冲区需要配套释放函数（短期在 `std/rt` 暴露，后续用 `bytes/string` 的 drop 收敛）
- 零成本抽象：大部分检查在编译期完成；运行时只保留必要的边界检查（如数组越界）

#### 10.2 所有权与移动（Status: Partial）
- 默认：**所有权唯一且不可复制（move-only）**；赋值会转移所有权：
  - `let b = a` 会移动 `a -> b`，`a` 之后不可再用（编译期报错）
- 例外（Copy types，Status: Implemented）：标量数值与 `bool` 等按值复制（不涉及释放），其余类型默认 move-only
- 函数返回：返回值总是“拥有所有权”的值（不能返回对局部变量的引用）
  - 允许返回：新创建的值、从参数显式 move 进来的值

#### 10.3 借用与引用（Status: Partial）
- 引用是借用，不拥有资源；引用不触发释放
- 借用规则（借用检查器，第一版已实现）：
  - 引用不能比所有者活得久（防悬垂引用）
  - 独占借用：同一时间只能存在一个“可写引用”（通过 `let r = &x` 产生）
  - 共享借用：允许多个“只读引用”（通过 `const r = &x` 产生）
  - 不允许“可写引用”与“只读引用”同时存在于同一所有者上
  - 借用寿命（Frozen，Status: Implemented, NLL 第一版）：当编译器可证明时，借用在“最后一次使用”后结束（而不是必须到词法作用域末尾）；当前实现粒度为“语句级”（borrow 至少存活到包含最后一次 use 的那条语句结束）
- 表达方式（Frozen/Planned）：
  - 语言不提供 `mut` 关键字，也不引入 `&mut T` 或 `RefMut<T>` 的语法区分
  - 语法层引用类型统一为 `Ref<T>`；过渡期 `&T` 作为别名解析为 `Ref<T>`（类型写法 `&T` 最终废弃）
  - 可写/只读由“借用能力”决定，并由编译器跟踪（不是靠 `let/const` 绑定去“升级权限”）：
    - `let r = &x`：得到 **独占/可写** 的 `Ref<T>`（exclusive borrow）
    - `const r = &x`：得到 **共享/只读** 的 `Ref<T>`（shared borrow）
  - 升级禁止（Frozen）：共享引用不能因为被 `let` 绑定而变成可写（例如把共享 `Ref<T>` 传给需要独占借用的参数是编译错误）
  - 降级/重借用（Frozen，Status: Planned）：可以从独占引用生成共享引用（reborrow），但在共享引用存活期间原独占引用被冻结（禁止写）
    - 例：`let r = &x; foo(r)` 其中 `foo(const a: Ref<T>)` 会触发重借用；`foo` 返回前 `r` 不可写
    - 例：`let r = &x; const s = r` 产生共享 `s`，`s` 存活期间 `r` 不可写
  - 引用读取（Status: Implemented）：
    - `r.get()`：读取引用指向的值（按值返回）
    - 不提供“写回解引用”（如 `*r = v` / `r.set(v)`）；需要写回时应通过原容器/原 owner 的写入语法完成

#### 10.4 函数参数的所有权界定（Frozen，默认安全）
为减少心智负担，采用“默认借用、显式 move”的规则：
- `fn f(x: T) { ... }`：默认等价 `fn f(const x: T)`，`x` 是只读借用（共享借用）；函数内不能把 `x` move/返回，也不能写字段/元素
- `fn f(let x: T) { ... }`：`x` 是可写借用（独占借用）；允许写字段/元素，但同一时间禁止其它借用
- `fn f(move x: T) { ... }`：`x` 被 move 进来，函数成为 owner，可返回/存储
- 调用规则（Frozen）：
  - 传只读借用：`f(x)`（`x` 保持可用）
  - 传可写借用：`f(x)`（`x` 仍可用，但在借用存活期间受借用检查器约束）
  - 传所有权：必须显式 `f(move x)`（move 后 `x` 不可再用）
- 示例（Frozen）：
  - `fn take(move user: User) -> User { return user }` ✅
  - `fn consume(user: User) -> User { return user }` ❌（user 为借用，不能返回）

#### 10.5 生命周期推断（Status: Partial）
- 编译器自动推断生命周期，用户通常不需要手动标注
- 已实现的禁止规则（第一版）：
  - 禁止返回局部变量引用：`fn invalid() -> Ref<User> { let u = ...; return &u }` ❌（悬垂引用；过渡期写 `&User` 亦同）
  - 禁止引用逃逸到外层作用域（跨 block 赋值逃逸）
- 从借用对象读取字段并返回（设计选择，Planned）：
  - 若返回类型是拥有所有权的类型（如 `string`），则需要 `clone()` 或由编译器按默认规则隐式 clone（需在 SPEC 冻结：偏“简单直观” vs “显式控制”）

#### 10.6 分配策略：栈默认、堆按需（Planned）
- 默认尽可能栈分配；对象在不逃逸时可被分配在栈/寄存器（逃逸分析）
- 当值逃逸到返回值/闭包/堆容器等场景时自动转为堆分配（对用户透明）
- 容器/动态对象（如 `string/bytes/map/array/closure env`）在 drop 时自动释放（无手动 free）

#### 10.7 资源释放与析构（Status: Partial）
- 作用域结束时自动 drop（RAII 风格）
- 第一版已覆盖 `map/array` 的 drop（作用域/覆盖赋值/return 路径）；并补齐 `closure env` 的 drop（闭包/回调不再泄漏）
- `deinit`/析构的语义与调用时机需要统一（与错误路径 panic/throw 下的保证一起冻结）
- 闭包 boxing：捕获变量 box/env 的所有权归 closure；closure drop 时释放 env（无 GC）
- 回调（callback）共享/托管规则（Frozen）：
  - 变量作用域结束：`cb` 这个绑定离开作用域时 drop，`cb.env` 做一次 `rc--`
  - 注册到异步/延迟执行：runtime 在注册时对回调做一次 retain（`rc++`），保存到 future/ctx/handle
  - 回调触发完成后：runtime 释放保存的那份（`rc--`）
  - 取消/错误/loopFree：只要 runtime 不会再调用该 callback，就必须在对应路径释放那份引用（`rc--`）

#### 10.8 并发与数据竞争（Planned）
- 当语言暴露线程/并发时，借用规则必须扩展到跨线程：禁止未同步的共享可变状态（数据竞争编译期阻止）
- 设计方向：类似 Rust 的 `Send/Sync` 能力边界（具体形式待定）

### 11. ABI / FFI 约定（Status: Planned）
本节解释“语言 ABI 是什么、怎么定义”：ABI（Application Binary Interface）是 **编译产物与运行时/外部库在二进制层面的契约**，包括调用约定、类型布局、参数/返回值传递方式、以及跨边界的所有权规则。

#### 11.1 调用约定（Planned）
- `extern fn` 默认使用目标平台的 C ABI（由 LLVM 默认 calling convention 决定），用于对接 `tua_rt` 与系统库。

#### 11.2 类型布局（Planned，现状以实现为准）
- 标量：
  - `int/i32 -> int32_t`，`long/i64 -> int64_t`，`float/f32 -> float`，`double/f64 -> double`，`bool -> i1`（对外通常按 `int32_t` 约定）
  - `ptr -> void*`（不透明指针/句柄）
  - `string -> char*`（UTF-8，当前约定为 NUL 结尾；`null` 表示空指针）
- 引用：`Ref<T> -> T*`（过渡期 `&T` 等价 `Ref<T>`）
- 动态值：`any -> tua_value`（见 `src/tua_map.h`）
- 容器句柄：
  - `map -> tua_map*`（见 `src/tua_map.h`）
  - `T[]/T[N] -> tua_array*`（见 `src/tua_array.h`，其中 `elem_size/fixed_len` 描述元素大小与定长信息）
- 二进制：
  - `bytes -> tua_bytes*`（见 `src/tua_bytes.h`）
  - `Slice<T> -> { T* data, int64_t len }`
- `Option<T>`：当前实现为 `{ i1 ok, T payload }` 的二元结构体（Planned：冻结更严格的跨边界表示与 nil 规则）
- 多返回：当前实现为“LLVM struct 返回”（JIT/AOT 具体 ABI 取决于平台对结构体返回的约定）

#### 11.3 跨 ABI 的所有权规则（Planned，先冻结最小集）
- `extern fn` 的参数默认是“借用”（callee 不应释放传入的 `map/array/string/ptr`），除非该函数名/文档明确标注为“接管所有权”。
- 术语约定（Frozen）：
  - borrow（借用）：callee 仅在本次调用期间可读/可用；不得释放、不得把指针/句柄缓存到返回值/全局/异步回调
  - own（拥有）：callee 取得释放责任；caller 在调用后不得再使用该对象（逻辑上 moved-from）
  - retain（保留/逃逸）：callee 需要把借用对象保存到未来时，必须**显式复制/增加引用计数**，并在未来的完成/取消/错误路径对称释放
- `bytes`/`Slice<T>`：
  - `bytes` 参数默认按借用传递（callee 不得释放；如需持有必须 clone/retain 并定义清晰释放约定）
  - `Slice<T>` 仅是视图（borrow）；callee 不得缓存其 `data` 指针到返回值/全局（否则会悬垂）
- `map/array/bytes` 句柄类型（Frozen）：
  - 句柄本身是“拥有型对象指针”（例如 `tua_map*`/`tua_array*`/`tua_bytes*`）；但传入 `extern fn` 时默认按 borrow 处理：callee 不得调用 `*_free`
  - 若需要跨调用保存句柄：
    - `array`：使用 `tua_array_clone` 拷贝出新的 owning 句柄
    - `bytes`：当前建议用 `tua_bytes_from_copy` 拷贝出新的 owning 句柄（后续可补 `tua_bytes_clone`）
    - `map`：当前无通用 clone API；如需保存应在 `std` 层定义明确的复制策略或改为传入“业务层快照结构”
- `string`（Frozen，现状兼容）：
  - `string` 对外是 `char*` 且默认按 borrow 处理：callee 仅可读取；不得写入、不得释放、不得保存到未来
  - 若需要返回“拥有型字符串”，必须提供配套释放函数并在 `std` 层封装；在未冻结“真正 string 类型 + drop”之前，跨 ABI 建议优先返回 `bytes`/`Slice<byte>`（带长度）而不是裸 `char*`
- 对于 `extern fn` 的 callback 参数：
  - 默认视为“借用回调值”；callee 不得直接 drop 参数本身
  - 若 callee 需要把回调保存到未来（escaping callback），必须显式 retain 一份，并在完成/取消/错误路径 release
  - 回调 ABI（Frozen，现状以实现为准）：closure 目前对外等价于 `{ void* fn, void* env }`；若需要 retain，等价于对 `env` 调用 `tua_box_inc(env)`，release 等价 `tua_box_dec(env)`（见 `src/rt/rt_box.h` 与平台绑定实现）
- 分配器域（Planned）：
  - 跨 ABI 传递的“可释放内存”（例如 `extern fn` 返回的 buffer/string/数组指针）必须清晰标注“由谁释放、用哪个 free”
  - 推荐统一使用 `tua_malloc/tua_free`（见 `src/rt/rt_alloc.h`）以便未来替换分配器；如使用系统 `malloc/free` 必须在函数名/文档中明确并提供对应释放函数
- 由 `extern fn` 返回的指针/缓冲区必须提供配套释放函数（例如 `tua_free`、`tua_fs_string_array_free`），并在 `std` 层封装为更安全的 API（Planned：用 `bytes/string` 的 drop 消除用户手动释放）。
