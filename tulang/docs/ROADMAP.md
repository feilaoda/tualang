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
  - 函数类型（TS 风格）：`(int, int) -> int`、`(int) -> (int, string)`

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

### 7. TODO List（下一阶段，按执行顺序）
- [ ] 冻结核心语义/spec：`struct` 值/引用语义、`enum` tag/raw 语义、`null/nil`、字段/构造参数初始化优先级、`this` 规则
- [x] 函数类型（TS 风格）：`(args) -> ret`（用于闭包变量/参数类型标注）
- [ ] 增加语义/类型分析层（Parse → Analyze → Codegen）：符号表、类型推断/检查、错误定位
- [ ] 强化 `let` 类型推断：覆盖 call/成员访问/条件表达式/函数返回值（减少 codegen 里的特判）
- [ ] 完成 `struct init/deinit` + 内存策略：`init(a,b)`、析构触发点、`free`/资源释放方案
- [ ] 升级 `enum` 模型：显式值/字符串 raw、`toString`/`fromString`、（可选）`println(enum)` 自动字符串化
- [ ] 体验与工程化：统一诊断格式、补 examples 覆盖边界、脚本批量跑 examples

### 8. Lua 能力对齐（如果目标是“具备 Lua 的所有能力”）
- [x] 控制流：`while/do/for`、`break/goto/continue`
- [x] 函数：闭包（upvalue）
- [x] 函数：多返回值
- [x] 模块与加载器：`import` / `from xx import yy`（TypeScript 风格）
- [x] 模块系统细节：默认全部导出；`private fn/struct/...` 不导出；模块作用域/命名空间；相对/绝对路径与扩展名；循环依赖顺序；模块缓存（同一模块只执行一次）
- [x] 作用域：块级作用域（变量仅在 `{}` 内可见），支持同名遮蔽（shadowing）
- [ ] 绑定语义：闭包捕获语义（按值/按引用）的完整规则 + 诊断（当前默认按引用捕获）
- [x] 多返回值：在赋值/参数传递中的解构规则（`let a,b = f()` / `a,b = f()`）
- [ ] 数据结构：优先实现 `map`（键值容器），后续再补数组
- [ ] 基础类型：把 `string` 做成真正的运行时基础类型（而不是仅 `i8*`/printf 直出）
- [ ] 空值：实现 `null` 作为空值，并定义比较/打印/条件判断/赋值规则
- [ ] 内存管理（短期）：RC（引用计数）+ 周期处理策略（检测/弱引用/限制形成环）
- [ ] 内存管理（中长期）：后续切换/替换为增量 GC（标记-清扫/分代，配写屏障）

### 9. 错误与入口（建议）
- [ ] 诊断统一格式：`file:line:col: error: message` + 源码片段 + 指示箭头（词法/语法/语义/运行时一致）
- [ ] 运行时错误：`panic/throw` 语义 + 栈回溯（至少函数名 + 行号）
- [ ] CLI 入口：`tuac run <entry.tua>`（支持 `--module-path`/`--dump-ir`/`--debug`）
- [ ] 构建入口：`tuac build <entry.tua> -o <out>`（先输出 LLVM IR/bitcode，再考虑 AOT/静态库）
