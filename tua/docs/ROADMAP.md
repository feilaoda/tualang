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
- 作用域：块级作用域、`const` 不可修改（含不可重新赋值与禁止经由该绑定写字段/元素）
- 类型：显式标注 + 基于字面量的最小推断（先做 `int/string/bool/long/double`）
- 类型错误：在编译期给出清晰报错（行号、token）

### 4. 后端（执行模型二选一）
- 现状：项目已有 LLVM-C JIT 路线（`src/main.c` + `src/llvm/*`）
- 建议：先把 LLVM 路线跑通（更快得到“能跑”的语言），再逐步补上自研 VM/字节码（体积更小、嵌入更方便）

### 5. 标准库与内建函数
- 先内建：`print(x)` / `println(x)`（映射到 `printf`）
- [x] `print/println` 格式化（第一版）：`println("a {} b {}", x, y)`（占位符 `{}` 数量必须匹配参数个数）
- 后续：字符串/数组/Map、错误类型与 `Ok()/Error()`、模块与 package

### 6. 工程化
- CLI：`tuac <file.tua>`（编译 + 运行 + IR/ASM dump）
- 测试：增加 `examples/` 对应的“可运行用例”，并能在脚本中批量跑
- [x] tests 目录按主题重构命名：`map_* / array_* / trait_* / generic_* / ...`（runner 仍按 `aot_*`/`fail_*` 识别）

### 7. TODO List（下一阶段，按执行顺序）
- [x] 冻结核心语义/spec：`struct` 值/引用语义、`enum` tag/raw 语义、`null/nil`、字段/构造参数初始化优先级、`this` 规则（见 `docs/SPEC.md`）
- [x] 位运算/移位（高优先级）：`~ & | ^ << >>`（含优先级/结合性、与无符号语义的交互、测试用例）
- [ ] 泛型 + 单态化（高优先级）：通用类型参数（函数/struct/trait）+ 编译期实例化（monomorphization）（当前：函数泛型已完成；`struct/trait` 泛型待做）
  - [x] v0 语法与 AST：`fn f<T>(...)` + 调用处显式 `f<int>(1)`（先不做推断）
  - [x] v0 单态化与缓存：同一组类型实参只生成一次实例；支持跨模块导入后的实例化
  - [x] v1 约束（bounds）：支持 `T: Trait`（基于 `impl Trait for Struct` 记录 + 实例化时检查）
  - [x] v0.5 类型实参推断（语法糖）：允许 `id(10)` 在可唯一推断时等价 `id<int>(10)`；否则报错要求显式 `id<T>(...)`
  - [x] 诊断：缺失类型实参/无法推断/约束不满足/递归实例化循环等
    - [x] 推断失败原因（missing/conflict/trait object）提示（第一版）
    - [x] 推断限制提示：当 `T` 仅出现在 `Option<T>`/`map<K,T>` 等嵌套位置时无法推断（第一版）
    - [x] 实例化回溯（generic instantiation stack，第一版）
    - [x] 实例化深度上限（防止 runaway monomorphization 崩溃/爆符号）
  - [ ] 泛型 `struct`（Planned）：`struct Box<T> { ... }`
  - [ ] 泛型 `trait`（Planned）：`trait Iter<T> { ... }` / associated types
- [ ] 冻结内存模型/spec（高优先级，无 GC/无手动 free）：见 `docs/SPEC.md` 的“内存模型”
  - [x] 所有权（第一版）：默认唯一、move-only（`struct/map/array`）；移动后不可用（move checker）
  - [x] 借用（第一版）：`const r = &x` 共享、`let r = &x` 独占；禁止冲突借用/被借用时写或 move
  - [x] 函数参数：默认只读借用（`fn f(x:T)` 等价 `fn f(const x:T)`）；可写借用用 `fn f(let x:T)`；取得所有权用 `fn f(move x:T)`（降低 90% 代码心智负担）
  - [x] `const` 视图（第一版）：`const view = x` 对 move-only 值创建共享只读视图；view 存活期间禁止 move/写
  - [x] 引用类型语法（`language/spec`）：类型层统一 `Ref<T>`；`&T` 为过渡别名并逐步废弃
  - [x] 重借用（reborrow，`language/spec`）：允许独占 -> 共享降级；共享存活期间冻结原独占引用（诊断要清晰）
  - [x] 借用寿命（NLL v0，语句级）：借用在“最后一次使用”后结束（不必延伸到词法作用域末尾）
  - [x] `bytes` 所有权 + drop + `mmap` 生命周期 + FFI 释放（LLM/IO/ABI 的核心前置）
  - [ ] `Slice<T>` 借用视图（指针+长度）+ NLL 规则 + ABI 冻结
    - [x] v0：`bytes.slice(off,n) -> Slice<byte>` + `Slice.len/get` + borrow 阻止 move（语句级 NLL）
    - [ ] v1：可写 slice（`set`）、更完整的 copy/借用语义、以及 array->slice 等扩展
- [x] 引用读取（第一版）：`r.get()`（替代 `*r`；不提供 `*r = v` 形式写回）
- [x] drop（第一版）：`map/array` 在作用域结束/覆盖赋值/`return` 路径自动释放（RAII）
- [x] move 规则补齐（第一版）：`struct` 字面量/字段赋值、`m[k]=v`、`{k:v}`/`[v]` 会移动 move-only 值并在 codegen 置空源 slot（避免 UAF/double-free）
- [x] 逃逸分析（第一版）：闭包仅 boxing 被捕获的局部/参数；栈默认、堆按需（后续可继续细化临时对象内联/寄存器化）
  - [ ] 并发：数据竞争编译期阻止（后续结合线程能力定义规则边界）
- [x] 函数类型（TS 风格）：`(args) -> ret`（用于闭包变量/参数类型标注）
- [x] 增加语义/类型分析层（第一版）：在 LLVM codegen 前做基础类型推断/检查（Option/map 相关），避免 LLVMVerify 才报错
- [x] 尾递归优化（self tail call）：`return f(args...)` 复用当前栈帧（当函数启用闭包 boxing 时禁用）
- [x] 编译器内建函数：从字符串特判改为 `BuiltinId` 表驱动（便于扩展与跨平台 stdlib/rt 绑定）
- [x] `struct` 嵌入字段 + 字段/方法提升（Go 风格但无子类型）：`...Base` / `...Base as name`，遮蔽优先、歧义报错、支持嵌套提升
- [x] trait v0（language/spec + compiler）：`trait` 声明 + `impl Trait for Struct` 满足性检查（方法集检查，含 promoted methods），支持跨模块导入/导出
  - [x] trait v1：静态分发（泛型单态化，例如 `fn f<T: Trait>(x: T)`；支持 `impl Trait for Struct { fn ... }` 提供 trait 方法体并在 bound 上分发）
  - [x] trait v2（第一版）：动态分发 trait object（fat pointer/vtable），并支持“静态优先、必要时自动降级为动态”（不要求显式 `dyn` 关键字）
- [ ] 强化 `let` 类型推断：覆盖 call/成员访问/条件表达式/函数返回值（减少 codegen 里的特判）
- [ ] 完成 `struct init/deinit` + 内存策略：`init(a,b)`、析构触发点、`free`/资源释放方案
- [ ] 升级 `enum` 模型：显式值/字符串 raw、`toString`/`fromString`、（可选）`println(enum)` 自动字符串化
- [ ] 体验与工程化：
  - [x] 统一诊断格式（基础版）：`file:line:col: error: message`（词法/语法/语义/模块导入/运行时）
  - [ ] 补 examples 覆盖边界
  - [ ] 脚本批量跑 examples

### 8. Lua 能力对齐（如果目标是“具备 Lua 的所有能力”）
- [x] 控制流：`while/do/for`、`break/goto/continue`
- [x] 函数：闭包（upvalue）
- [x] 函数：多返回值
- [x] 模块与加载器：`import` / `from xx import yy`（TypeScript 风格）
- [x] 模块系统细节：默认全部导出；`private fn/struct/...` 不导出；相对/绝对路径与扩展名；循环依赖顺序；模块缓存（同一模块只执行一次）
  - [x] import-all：`import "path"`
  - [x] 默认命名空间别名：按路径末段生成；若为保留字/内建类型 token 自动前缀 `_`
  - [x] named import + 重命名：`import A as A1, B from "path"`
  - [x] 命名空间导入：`import "path" as ns`，使用 `ns.Name`
- [x] 作用域：块级作用域（变量仅在 `{}` 内可见），支持同名遮蔽（shadowing）
- [ ] 绑定语义：闭包捕获语义（按值/按引用）的完整规则 + 诊断（当前默认按引用捕获）
- [x] 多返回值：在赋值/参数传递中的解构规则（`let a,b = f()` / `a,b = f()`）
- [x] 数据结构：优先实现 `map`（键值容器），后续再补数组
  - [x] 字面量：`{ key: value, ... }`（key 仅常量；重复 key 后者覆盖；允许尾逗号）
  - [x] 类型：`map` 与 `map<K,V>`（K: `string/int/long`；V: 标量 + `struct/map/array`，第一版）
  - [x] 读取：`m[k] -> Option<V>`（未命中返回 `None()`；可用 `??` 提供默认值）
  - [x] 写入：`m[k] = v`（当 `m` 为变量且为 `null` 时自动初始化）
  - [x] 内建方法：`len()/hasKey()/get()/delete()/clear()`
  - [x] 借用读取（`language/spec` + `tua_rt`）：`getRef()/getRefWrite()`（支持 `map` 与 `map<K,V>` 的 move-only value：`struct/map/array`；标量 value 仍按 `get/m[k]`）
  - [x] 遍历（map）：`for v in m { ... }` / `for k,v in m { ... }`
  - [x] 遍历（array）：`for v in a { ... }` / `for v,i in a { ... }`
  - [x] 完整强类型（第一版）：对 `map<K,V>` 写入/字面量做静态检查（禁止写入 `null`/错误类型）
  - [x] 类型推断（第一版）：从字面量推导 `map<K,V>`（仅当 key/value 都是非空字面量且类型一致时）
- [ ] 基础类型：把 `string` 做成真正的运行时基础类型（而不是仅 `i8*`/printf 直出）
- [ ] 空值：实现 `null` 作为空值，并定义比较/打印/条件判断/赋值规则
- [ ] 内存模型 v1：
  - [x] closure env drop + 回调 retain/release（无 GC）
  - [ ] `struct deinit` + deep drop（容器元素级析构）
- [ ] std 中“裸指针 free”收敛为明确的资源句柄 `close()` 或拥有型 `bytes/string` drop（`bytes` 已完成，`string` 待做）

### 9. 错误与入口（建议）
- [x] 诊断统一（基础版）：`file:line:col: error: message`（词法/语法/语义/模块导入/运行时一致）
- [ ] 诊断增强：源码片段 + 指示箭头（词法/语法/语义/运行时一致）
- [x] 运行时错误：输出行号（best-effort，先解决“哪一行炸了”）
- [ ] 运行时错误：`panic/throw` 语义 + 栈回溯（至少函数名 + 行号）
- [ ] CLI 入口：`tuac run <entry.tua>`（支持 `--module-path`/`--dump-ir`/`--debug`）
- [ ] 构建入口：`tuac build <entry.tua> -o <out>`（先输出 LLVM IR/bitcode，再考虑 AOT/静态库）

### 10. 运行时（`tua_rt`）与标准库（跨平台 + 异步 IO）

目标：**不自写 libc**，Linux/macOS 先直接使用系统 libc + POSIX；Windows 先预留接口与占位实现，后续补齐 Win32/IOCP 后端；上层 `std/` 尽量用 Tua 实现，只依赖稳定的 `tua_rt` C API。

#### 10.1 总体决策（已冻结）
- [x] 架构分层：`tua_rt`（C 运行时/平台抽象） + `std/`（Tua 标准库）
- [x] 平台推进顺序：Unix（Linux/macOS）优先；Windows 延后但接口预留
- [x] 异步 IO：自研事件循环与后端（不依赖 libuv），但参考其“loop + backend + workqueue”分层
- [x] `std.time`：提供 monotonic/real time 与 sleep（第一版）

#### 10.2 `tua_rt` 接口冻结（R0）
- [x] 新增 `src/rt/` 目录结构：`rt.h` + `platform/posix` + `platform/win32`（先占位）
- [x] 统一错误码：`TUA_E_*`（映射 errno；Windows 后续映射 GetLastError/WSAGetLastError）
- [ ] 统一句柄模型：文件/目录/Socket/线程句柄类型（避免上层直接依赖 fd/HANDLE）
  - [x] 基础：引入 `tua_handle_t` + `tua_io_start_handle`（loop watcher 先完成）
  - [x] 网络：补齐 `tua_tcp_socket_handle`/`tua_tcp_listener_handle`（先提供 accessor，后续再移除 fd 暴露）
  - [x] 兼容期收敛：std/rt 与测试不再依赖 fd API（逐步废弃 `tua_io_start`/`tua_tcp_socket_fd`）
  - [ ] 破坏性升级：移除 fd API（只保留 handle API）
  - [ ] 文件/目录句柄：补齐 `tua_file_t`/`tua_dir_t`（或统一 handle+kind），避免上层依赖 fd/`DIR*`
- [x] 路径与编码约束：上层一律 UTF-8（已写约束）
- [ ] Windows 路径：UTF-16 转换 + 测试用例
- [x] 取消模型：`tua_cancel_t`（为 async/协程预留）
- [x] 超时/deadline：统一 deadline 类型与辅助函数（为 async/协程预留）

#### 10.3 线程与同步（R1-thread）
- [x] `rt_alloc`：`malloc/free/realloc` 抽象（后续可切换 mimalloc/jemalloc）
- [ ] `rt_thread`：TLS/atomics（POSIX=pthread；Windows 后续补）
- [x] `rt_thread`：thread/mutex/cond（POSIX=pthread；Windows 后续补）
- [x] `rt_time`：monotonic/realtime/sleep（macOS 用 mach/gettimeofday，其他用 clock_gettime）

#### 10.4 文件系统（同步，R1-fs-sync）
- [x] `rt_fs`：readFile/writeFile/stat/mkdir/readdir/realpath（POSIX 后端，第一版）
- [x] `std.fs`（Tua）：同步文件系统 API（第一版，readFile/writeFile/stat/mkdir/readdir/realpath）

#### 10.5 事件循环（R2-loop）
- [x] `rt_loop`：基础 loop API（create/run/stop）
- [x] `rt_loop`：timer（单调时钟驱动）
- [x] `rt_loop`：fd watcher（readable/writable）
  - [x] Linux 后端：epoll（已实现，待 Linux 环境验证）
  - [x] macOS 后端：kqueue
  - [x] 兜底后端：poll（用于 bring-up/回归）
- [x] `rt_loop`：跨线程唤醒（pipe），支持从其他线程 post 到 loop
- [x] `rt_task`：任务队列（post 到 loop 线程执行）
  - [x] `std.time/async`：`afterMs` 定时回调（第一版）
  - [x] repeating timer：`everyMs` + cancel（为心跳/重试/调度提供基础能力）
  - [x] loop post 绑定：`tua_loop_post_cl`（Tua 侧可直接投递任务到 loop）

#### 10.6 异步网络（R3-net-async）
- [x] `rt_net`：non-blocking socket + loop 集成（connect/accept/read/write）
- [x] 超时（第一版）：connect/read/write deadline
- [x] DNS/解析（第一版）：线程池 offload（getaddrinfo）
- [x] `std.net`（Tua）：TCP/UDP/Addr/Resolver 的跨平台语义封装

#### 10.7 异步文件（R4-fs-async）
说明：Unix 常规文件不适合用“就绪事件”做真正 async，第一版采用 **线程池 offload**，API 仍保持 async 形态。
- [x] `rt_workqueue`：固定大小线程池 + 任务投递
- [x] `rt_fs_async`：POSIX 线程池 offload
  - [x] readFile/writeFile
  - [x] stat/readdir
- [x] `std.fs.async`（Tua）：高层 async 文件 API
- [ ] 后续优化（可选）：Linux io_uring / macOS 特定方案（不阻塞主线）

#### 10.8 Windows（R5-win32，占位 -> 以后落地）
- [x] 先提供 win32 stub：编译通过但返回 `TUA_E_NOTSUP`（保证扩展点固定）
- [ ] `rt_loop` Windows：先落地最小 loop（IOCP + post + timer），再逐步补齐 watcher/IO
- [ ] `rt_loop` Windows 后端：IOCP
- [ ] `rt_net` Windows：Winsock + IOCP
- [ ] `rt_fs_async` Windows：Overlapped I/O + IOCP（或线程池过渡）

### 11. LLM 推理（CPU-first，先跑通再优化）

目标：先在 macOS/Linux 上跑通 **本地推理**（CPU），不依赖大型外部运行时；Windows 后续对齐。

原则：**不把应用（LLM）逻辑耦合进编译器**。
- 编译器只提供通用语言能力与通用 FFI/构建能力；不新增任何 “llm 专用 builtin”。
- `tua_rt` 只提供跨平台底座（内存/线程/文件/时间/句柄/loop）；不内置模型/推理算法。
- `std` 提供可复用的通用库（bytes/io/json/tokenizer 等）；LLM 逻辑优先放到独立的 `llm` 包/库中（Tua + 可选 C 内核）。

#### 11.0 LLM 跑通所需基础能力（分层归属）
说明：本节不重复列项；以下各层的 TODO 为唯一事实来源：
- `language/spec`：见 7（内存模型）与 11.1（通用语言/编译器能力）
- `tua_rt`：见 11.2
- `std`：见 11.3
- `llm`：见 11.4

#### 11.1 语言/编译器（通用能力，不专属于 LLM）
- [x] 基础数值类型 + 溢出/转换规则（第一版）
  - [x] 整数：`i8/i16/int(i32)/long(i64)/isize`、`u8/u16/u32/u64/usize/byte`
  - [x] 浮点：`f16/f32/f64/bf16`（`float`=f32，`double`=f64）
  - [x] FP8 类型名保留：`f8/bf8`（当前仅作为“8-bit 存储类型”，只允许与 `u8/byte` 互转；标量算术/比较暂不支持）
  - [x] 转换语义：
    - [x] `(T)expr`：不检查（截断/扩展/浮点转换）
    - [x] `expr as T`：可检查转换，返回 `Option<T>`（范围检查；NaN/超范围为 `None()`）
  - [ ] 后续：FP8（E4M3/E5M2 等）具体格式与算术/向量化支持（为 AI 量化做准备）
- [ ] 高效 `bytes`/`slice<T>` 视图（避免把 `string` 当字节容器；`bytes` v0 已落地，`slice<T>` 待实现）
- [x] 通用 FFI：Tua 侧声明外部符号与签名（例如 `extern fn ...`），避免在编译器里维护函数名白名单
- [ ] 构建/链接：通用方式引入外部库（静态/动态），不为 LLM 单独加 `tuac llm ...` 子命令
  - [x] CLI：`tuac` 支持 `--link-search/-L`、`--link-lib/-l`、`--link-arg`（透传到系统链接器）
  - [x] AOT：`tuac --output a.out` 时把外部库链接进最终可执行文件（macOS/Linux）
  - [x] JIT：把外部库加载进宿主 `tuac`（或 `dlopen`）以便 JIT 解析符号
    - [x] macOS：`--dlopen libfoo.a` 内部用 `-Wl,-force_load` 构建临时 `.dylib` 并加载
    - [x] Linux：`--dlopen libfoo.a` 内部用 `--whole-archive/--no-whole-archive` 构建临时 `.so` 并加载
    - [x] 运行时 `--dlopen <path>`（POSIX）加载 `.so/.dylib`（Windows 先占位）
    - [x] `--check-extern`（JIT）：启动前用 `dlsym` 预检本次编译单元里的 `extern fn` 符号是否可解析
  - [x] 诊断：链接失败/缺符号时给出清晰报错（包含缺失符号与 `extern fn` 声明位置，并提示 `-L/-l/--link-arg` 或 `--dlopen`）

#### 11.2 `tua_rt`（跨平台底座）
- [ ] 大文件能力：流式读取 + `mmap`（POSIX 第一版），Windows 先 stub
- [ ] 线程/并行：workqueue + 原子/CPU feature 探测（为 SIMD/量化做准备）
- [ ] RNG：可复现的基础随机数（用于 sampling；也可由 std/llm 自带实现）

#### 11.3 `std`（通用库，LLM 可复用）
- [x] `std.strconv`：`Int.parse`（基于 `tua_parse_int`）与 `Int.toString`（基于 `tua_int_to_string_alloc`；返回值可用 `Rt.free` 释放）
- [x] `std.bytes` / `std.io`（v0）：`bytes` v0 + LE 读取工具 + `Reader/Cursor/BufReader`（std 版拷贝实现）
- [x] `std.bytes` / `std.io`（v1）：range copy/memcpy 优化（`Bytes.copy`）+ mmap-backed file reader（避免逐字节 set/get）
- [x] `std.utf8`（v0）：UTF-8 校验 + codepoint 解码 + 边界切分（用于 tokenizer、文本切片）
- [x] `std.json`（v0）：最小 JSON 解析（用于模型配置/metadata/推理参数）
- [ ] Tokenizer：BPE（GPT-2 风格）或 sentencepiece（择一），先做正确性再做性能

#### 11.4 `llm` 外部包（应用代码：模型/推理/采样）
说明：可以是仓库内 `packages/llm`（或 `examples/llm`），也可以是外部独立 repo；核心要求是 **不依赖编译器特判**。
- [ ] 模型格式：先支持一种主流格式（建议 GGUF）
  - [x] GGUF（v0）：解析 header + KV metadata（先不做 tensor）
  - [ ] GGUF（v1）：解析 tensor infos + tensor data layout
- [ ] 数学内核：f32 baseline matmul/dot（可先朴素），再逐步并行化/向量化
- [ ] KV cache：数据结构与更新（注意内存占用与布局）
- [ ] Sampling：softmax + temperature + top-k/top-p + RNG（可复现）
- [ ] Runner：提供 `examples/llm/run.tua`（或独立 CLI 工具），通过 `tuac run ...` 运行

#### 11.5 R-llm-1：性能与量化
- [ ] 量化：q4/q8（至少一种）+ 对应 dot kernel
- [ ] SIMD：SSE/AVX/NEON（按平台探测），逐步替换 baseline
- [ ] Prefill/Decode 调度：线程划分、batch、缓存友好

#### 11.6 R-llm-2：工程化与生态
- [ ] `std.http`（可选）：模型/配置加载
- [ ] 流式输出：token-by-token callback/iterator 语义（对接 UI/Agent）
