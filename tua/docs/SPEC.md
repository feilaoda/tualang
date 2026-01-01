## Tua 语言规格（工作草案）

本文件用于把 `README.md` 的“描述性语法”落成“可实现的语义约束”，并记录尚未决策的点。

约定：本文用 `Status: Implemented | Partial | Planned` 标记当前实现进度。

### 0. 核心语义冻结（v0）
本节列出已冻结的核心语义点：实现可以逐步补齐，但语义以本文为准；如需破坏性变更，需要先更新 SPEC 并同步 ROADMAP。
- `struct` 值/引用语义（见 7.1）
- `enum` tag/raw 语义（见 7.4）
- `null` 语义（字面量、比较、truthiness、字符串拼接等，见 4/5/9.3）
- 字段/构造参数初始化优先级（见 7.1）
- `this` 规则（可用范围 + 隐式字段访问/赋值，见 7.1）

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
    - 指针/引用类型（`ptr`、`string`、`map`、`&T` 等）仅允许 `==` / `!=` 比较（按指针值）
    - 有序比较（`< <= > >=`）对指针/引用类型无定义，视为编译错误
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
    - 指针/引用类型：`null`（包括 `string/ptr/map/&T` 等）
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
  - 表达式上下文规则：当 `f()` 返回多个值时，在普通表达式上下文会自动取第一个值（例如 `let x = f()`）
  - `return f()` 转发规则：当当前函数是多返回值函数时，`return f()` 会直接转发 `f()` 的多返回结果
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
    - 当函数体内出现匿名函数时，本函数的局部变量/参数会自动使用“heap box”存储以保证被捕获后仍然有效
    - 目前 box/env 使用 `malloc`，尚未引入 RC/GC，因此存在内存泄漏（后续按 roadmap 切换到 RC/增量 GC）
  - 当前限制（后续规划补齐）：
    - 已支持 TS 风格函数类型 `(args) -> ret`（见下）；但完整类型检查/类型推断仍在规划中
- 内建函数（Status: Implemented）：
  - `print(x)` / `println(x)`（目前支持打印 int/long/double/bool/string 指针）

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
- 值语义：
  - `let b = a` 是值拷贝（字段复制）
  - `&a` 取 `a` 的地址，得到引用（类型层面是 `&T`，LLVM 层面是 `T*`）
- 字段访问：`p.x` / `p.x = v`（接收者目前仅支持变量；若变量是 `&T` 则会间接到指向的对象）
- `this`（Frozen）：
  - 仅在 `struct` 实例方法中可用，类型恒为 `&T`
  - `this` 绑定不可重新赋值，但可通过 `this.field = ...` 修改字段
  - 隐式字段访问/赋值（Frozen）：在 `struct` 实例方法中，若标识符既不是局部变量/参数/模块顶层变量，且 `this` 上存在同名字段，则将 `x` / `x = v` 分别解析为 `this.x` / `this.x = v`（便于写 `x` 代替 `this.x`）
- 实例方法调用（Frozen）：
  - `p.m(a,b)` 会被编译为 `T__m(recv, a, b)`，其中 `recv` 恒为 `&T`
    - 若 `p` 为 `T`（值），则 `recv = &p`
    - 若 `p` 为 `&T`（引用），则 `recv = p`
- `init/deinit`（Status: Partial）：
  - 已支持解析 `init(){...}` / `deinit(){...}` 为方法
  - 自动调用时机/析构语义尚未定义（见内存模型规划）

#### 7.2 impl（Status: Implemented，Rust 风格）
- 语法：
  - `impl StructName { fn func(...) ... }`
- 语义：
  - `impl StructName` 中的 `fn` 会作为 `StructName` 的实例方法（与 `struct StructName { fn ... }` 等价）
  - 这些方法内允许使用 `this`（类型恒为 `&StructName`）
  - 若同名方法重复定义（struct 内 vs impl 块，或多个 impl），视为编译错误（第一版）

#### 7.3 object（Status: Implemented）
- `object O { fn f(...) ... }`
- `O.f(a,b)` 编译为 `O__f(a,b)`（静态方法）
- 在 `object O` 的方法体内允许省略 `O.`：
  - 非同名调用：`g(a,b)` 会优先解析为同一个 `object` 内的 `O.g(a,b)`；若不存在该方法，则按普通规则继续解析为模块顶层函数/`extern fn`/导入别名等
  - 同名调用（递归/包装场景）：在 `fn f(){ ... }` 内写 `f()` 会先按普通规则解析模块顶层/`extern fn` 的 `f`；若找不到，再回退解析为 `O.f()`（递归）

#### 7.4 enum（Status: Implemented）
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
  - `import "relative/or/absolute/path.tua"`：执行模块，并把该模块所有可导出符号引入到当前作用域（import-all）
  - `import "path.tua" as ns`：执行模块，但不把符号直接引入当前作用域；后续通过 `ns.Name` 访问（命名空间导入）
  - `import A,B,C from "path.tua"`：把导出的符号引入到当前模块作用域
    - 支持重命名：`import A as A1, B as B1 from "path.tua"`
    - 兼容旧写法：`from "path.tua" import A,B,C`
      - 同样支持 `as`：`from "path.tua" import A as A1, B`
- 规则：
  - 相对路径以当前文件所在目录为基准
  - 若省略后缀，会自动补 `.tua`
  - 标准库路径：以 `std/` 开头的导入路径会从标准库目录解析（见 `std/README.md`）
  - 模块缓存：同一模块只会被加载/执行一次（顶层语句只会在 `main` 中生成一次）
  - 循环依赖：允许；按依赖优先的加载顺序进行一次性执行（后续可补更精确的初始化时序定义）
- 当前限制：
  - 只支持字符串字面量路径（不支持表达式路径）
  - 仅支持导入 `fn/struct/object/enum`；不支持导入模块级变量
  - 任何导入形式（import-all / named import / namespace import）若引入同名符号（或同名命名空间别名）冲突，会直接报错并停止（可用 `as` 或命名空间导入来消歧义）
  - 命名空间导入的成员访问：已支持 `ns.F(...)`、`ns.StructCtor(...)`、`ns.Obj.method(...)`、`ns.Enum.Variant`

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
    - `V` 当前仅允许：`int/long/double/bool/string`
- 字面量（Status: Implemented）：
  - 语法：`{ key: value, ... }`
  - `key` 仅允许常量字面量：`int/long/string`
  - 重复 key：后者覆盖前者
  - 允许尾逗号：`{ "a": 1, }`
- 读取（Status: Implemented）：
  - `m[k] -> Option<V>`（对 `map` 则为 `Option<tua_value>`）
  - key 不存在返回 `None()`；不再提供 `v,ok = m[k]` 多返回形式
  - 若 `m` 为 `null/未初始化`，读取会触发运行时错误（带行号）
- 写入（Status: Implemented）：
  - `m[k] = v`
  - 若 `m` 是变量且当前为 `null/未初始化`，会自动初始化为新 map 再写入
  - 对 `map<K,V>`：写入时会做静态检查（key/value 类型不匹配会编译错误；禁止写入 `null`）
- 内建方法（Status: Implemented）：
  - `m.len() -> int`
  - `m.hasKey(k) -> bool`
  - `m.get(k) -> Option<V>`
  - `m.delete(k) -> bool`
  - `m.clear() -> void`
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
  - `T[N]`：定长数组（`N` 为编译期常量整数）
  - 数组为引用语义：`let b = a` 会共享同一个数组对象；`clone()` 会深拷贝
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

#### 9.3 `null`（Status: Implemented）
- `null` 是“指针空值字面量”（当前实现中等价于 `i8*` 的空指针），用于表示“无指针/无句柄/未初始化引用”等场景
- 赋值与类型（Frozen）：
  - `null` 可赋值给 `string`、`ptr`、`map`、`&T` 等指针/引用类型
  - `nil` 当前不是关键字；内部的 dynamic `nil` 仅用于 `any`/`tua_value`（例如 `None()` 的 payload），不直接暴露为语法字面量
- 运算（Frozen）：
  - truthiness：`null` 为 falsey（例如 `!null == true`）
  - `string + null`：`null` 视为字符串 `"null"`（例如 `"x" + null == "xnull"`）
  - `string == null`：当且仅当该 `string` 值为 `null` 指针时为 `true`（`!=` 反之）
  - 非 `string` 的指针/引用与 `null` 比较：`p == null` 等价于“指针值为 0”（`!=` 反之）

#### 9.4 运行时错误定位（Status: Partial）
- 运行时错误会输出 best-effort 行号（用于定位 `unwrap(None)`、对 `null map` 读写等）。
- 规划：统一诊断格式为 `file:line:col: ...` 并增加栈回溯。
