# Tua（嵌入式 Lua，实验性）

[![Ownership Profile Tests](https://github.com/feilaoda/tualang/actions/workflows/profile-tests.yml/badge.svg)](https://github.com/feilaoda/tualang/actions/workflows/profile-tests.yml)

Tua 是面向嵌入式场景的 Lua 风格语言运行时：极简语法 + 类型系统 + LLVM JIT/AOT 编译器，目标是“小运行时、可预测性能、无 GC”。

- 规范：`docs/SPEC.md`
- packages 规划：`docs/PACKAGE_SPEC.md`、`docs/PACKAGE_ROADMAP.md`
- 路线图：`docs/ROADMAP.md`
- 语法参考（ANTLR4）：`tuaparser.g4`（包含 lexer+parser；`tualexer.g4` 已弃用）

## 构建与运行

构建编译器：

```bash
make tuac
```

JIT 运行（解释/即时编译执行）：

```bash
./bin/tuac examples/readme_demo.tua
```

AOT 生成可执行文件：

```bash
./bin/tuac --output /tmp/a.out examples/readme_demo.tua
/tmp/a.out
```

AOT 链接外部库（示例）：

- macOS Accelerate（BLAS）：`./bin/tuac --output /tmp/a.out --link-arg -framework --link-arg Accelerate <file.tua>`
- Linux OpenBLAS（示例）：`./bin/tuac --output /tmp/a.out --link-lib openblas <file.tua>`

运行测试：

```bash
./tests/run.sh
```

仅运行 ownership profile 测试（script/system 对照）：

```bash
./tests/run_profile.sh
```

ANTLR 语法回归（只做 parse，不做编译/运行）：

```bash
./tools/antlr_parse_check.sh
```

标准库目录：
- 在仓库内直接运行 `./bin/tuac` 会自动找到 `./std`
- 如果单独拷贝了 `tuac`，需要设置：`export TUA_STDLIB_DIR=/path/to/std`
- `std/*` 会被自动导入（用户代码一般不需要写 `import "std/..."`；只有外部包/模块才需要显式 `import`）

包搜索路径（用于 `import "json"` / `import "packages/json"` 这类“非相对导入”的查找）：
- 设置：`export TUA_PACKAGE_DIR=/path/to/packages`（可用 `:` 分隔多个目录）
- 查找规则：会尝试 `<root>/<raw>.tua`，以及按最后一段名兜底 `<root>/<name>/<name>.tua`（例如 `import "json"` => `.../json/json.tua`）

## 语法速览（以实现为准）

### 变量与绑定

- `let`：可写绑定
- `const`：只读绑定（不可重新赋值/不可改字段/不可改数组元素）
- 可选语法糖：`a: T = expr` 等价于 `let a: T = expr`（声明一个可写绑定）

```tua
let a: int = 10
a = a + 1

const s: string = "hello"
// s = "x"         // ❌

// 语法糖（等价 let）
x: long = 123
```

### 基础类型（已实现）

- `bool`
- `int`（i32）, `long`（i64）
- `float`（f32）, `double`（f64）
- `string`（UTF-8；语义为不可变值语义；运行时实现计划升级为自动管理的 string 类型，见 `docs/SPEC.md`）
- `ptr`（不透明指针/句柄）
- 无符号整数：`u8/u16/u32/u64/usize`（以及 `isize`）
- 数组：`T[]` / `T[N]`
- 映射：`map` / `map<K,V>`
- `Option<T>`（`Some(v)` / `None()`）

> `interface/class/package/char` 等目前未实现（见 `docs/ROADMAP.md`）。

### 条件与循环

```tua
if a > 10 || (b > 0 && c > 0) {
  // ...
} else if a < 5 {
  // ...
} else {
  // ...
}

for i = 0; i < 10; i++ {
  // ...
}

// for-in：map/array
for v in m { /* ... */ }
for k,v in m { /* ... */ }
for v,i in a { /* ... */ }  // array: v=value, i=index
```

### 函数与多返回

返回类型两种写法都支持：

```tua
fn add(a: int, b: int) -> int { return a + b }
fn add2(a: int, b: int) int { return a + b } // 语法糖

fn pair() -> int, string {
  return 1, "ok"
}

let x, s = pair()
```

### struct / object / enum / trait

```tua
struct Point {
  x: int = 0
  y: int = 0

  fn move(dx: int, dy: int) {
    this.x = this.x + dx
    this.y = this.y + dy
  }
}

let p = Point(1, 2)
p.move(3, 4)
println(p.x)
```

`object` 用作命名空间（类似“静态方法集合”）：

```tua
object StringUtils {
  fn isEmpty(s: string) -> bool {
    return s == null || s == ""
  }
}
```

`enum`：

```tua
enum RoleType { Admin, User }
```

`trait`（静态分发 + 动态 trait object；不需要 `dyn` 关键字）：

```tua
trait HasId { fn id() -> int }

struct S { id: int }
impl S { fn id() -> int { return this.id } }
impl HasId for S {}

fn f<T: HasId>(x: T) -> int { return x.id() }   // 静态
fn g(x: HasId) -> int { return x.id() }         // 动态（trait object）
```

> 目前泛型函数调用需要显式类型参数（例如 `f<S>(s)`），暂不支持 `f(s)` 的自动类型推断（见 `docs/ROADMAP.md`）。

> `init/deinit` 目前仅“可解析为方法”，自动调用时机/析构语义仍在冻结中（见 `docs/SPEC.md` 内存模型章节）。

### 数组与 map

数组字面量使用 `{...}`（与 `map` 共用大括号，是否为 map 由 `key: value` 形式区分）：

```tua
let a: int[] = {1, 2, 3}
a.push(4)
println(a[0])
```

`map`：

```tua
let m: map = {"a": 1}
m["a"] = 2
println(m["a"] ?? 0)
```

当 `map<K,V>` 的 `V` 是结构类型（`struct/map/array` 等）时，推荐用引用读取避免拷贝：

```tua
struct S { a: int, b: int }
let m2: map<string, S> = { "x": S{a: 1, b: 2} }

let r = m2.getMut("x").unwrap()
r.b = r.b + 1
```

`for k,v in map<K,V>`：当 `V` 为结构类型时，`v` 绑定为 `Ref<V>`（可写借用），可直接改 `v` 的字段并回写到 map。

### Ref<T> / &T（引用）

- 语法层统一类型 `Ref<T>`
- 过渡期 `&T` 作为别名解析为 `Ref<T>`（计划废弃）
- 读取：`r.get()`（按值返回）
- 不提供 `*r = v` / `r.set(v)` 这类“写回解引用”

### 闭包与异步回调（callback）

- 闭包默认按引用捕获外层变量（Lua 风格）
- closure env 使用引用计数 box 管理；作用域结束自动 drop
- 对会“保存到未来再调用”的回调（timer/post/async IO），runtime 会 retain 一份回调并在完成/取消/错误路径 release
- 同一个回调可以重复注册（可复用）

```tua
import "std/rt" as rt
import "std/time/async" as time

fn main() {
  let loop = rt.Rt.loopCreate()
  let fired = 0
  let cb = fn() {
    fired = fired + 1
    if fired == 2 { rt.Rt.loopStop(loop) }
  }
  time.TimeAsync.afterMs(loop, 1, cb)
  time.TimeAsync.afterMs(loop, 2, cb)
  rt.Rt.loopRun(loop)
  rt.Rt.loopFree(loop)
}
```

### 打印

```tua
println("x = {}", 123)
println("a {} b {}", 1, 2)
println(123) // 也支持
```

> 格式化打印当前仅支持“字符串字面量”作为 format 参数（非字面量会编译错误）。
