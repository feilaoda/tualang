## Tua 语言落地路线（以 README 为总纲）

### 0. 先定“最小可运行切片”
- 输入：`.tua` 源码
- 产出：可执行（JIT 或 AOT），并能打印结果、报错定位到行列
- 覆盖 README 的最小子集：`let/const`、基础类型（`int/string/bool/long/double`）、表达式、`if/else if/else`、`for`、`fn`（先只做声明与调用/内建函数）

### 1. 语言定义（README -> 可实现的规格）
- 词法：关键字（`let/const/if/else/for/fn/return`）、字面量（数字/字符串/true/false）、运算符（`+ - * / == != < <= > >= && || !`）、分隔符（`(){};,:`）
- 语法：
  - 变量声明：`let a:int = 10;`、`a:int = 10;`、`let a = 10;`、`const s = "hi";`
  - 条件：`if cond { ... } else if cond { ... } else { ... }`（可选括号：`if (cond) {}`)
  - 循环：`for i=0; i<n; i++ { ... }`（可选括号）
  - 函数：`fn demo(a:int, b:string) -> int { ... }`（可选 `->` 语法糖：`fn demo(...) int {}`)

### 2. 前端（Lexer/Parser/AST）
- Lexer：把 README 的关键字/字面量稳定 token 化（含兼容别名：`let` ~ `var`，`fn` ~ `func`）
- Parser：生成 AST，并保证错误恢复（至少能继续解析到下一个 `;`/`}`）
- AST：先保持简单（表达式、变量、块、if、for、call），后续再扩展 struct/object/enum

### 3. 语义层（类型检查 + 作用域）
- 作用域：块级作用域、const 不可重新赋值
- 类型：显式标注 + 基于字面量的最小推断（先做 `int/string/bool/long/double`）
- 类型错误：在编译期给出清晰报错（行号、token）

### 4. 后端（执行模型二选一）
- 现状：项目已有 LLVM-C JIT 路线（`src/main.c` + `src/llvm/*`）
- 建议：先把 LLVM 路线跑通（更快得到“能跑”的语言），再逐步补上自研 VM/字节码（体积更小、嵌入更方便）

### 5. 标准库与内建函数
- 先内建：`print(x)` / `println(x)`（映射到 `printf`）
- 后续：字符串/数组/Map、错误类型与 `Ok()/Error()`、模块与 package

### 6. 工程化
- CLI：`tuac <file.tua>`（编译 + 运行 + IR/ASM dump）
- 测试：增加 `examples/` 对应的“可运行用例”，并能在脚本中批量跑

