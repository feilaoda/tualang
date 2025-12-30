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
- [x] 增加语义/类型分析层（第一版）：在 LLVM codegen 前做基础类型推断/检查（Option/map 相关），避免 LLVMVerify 才报错
- [x] 尾递归优化（self tail call）：`return f(args...)` 复用当前栈帧（当函数启用闭包 boxing 时禁用）
- [x] 编译器内建函数：从字符串特判改为 `BuiltinId` 表驱动（便于扩展与跨平台 stdlib/rt 绑定）
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
  - [x] named import + 重命名：`import A as A1, B from "path"`
  - [x] 命名空间导入：`import "path" as ns`，使用 `ns.Name`
- [x] 作用域：块级作用域（变量仅在 `{}` 内可见），支持同名遮蔽（shadowing）
- [ ] 绑定语义：闭包捕获语义（按值/按引用）的完整规则 + 诊断（当前默认按引用捕获）
- [x] 多返回值：在赋值/参数传递中的解构规则（`let a,b = f()` / `a,b = f()`）
- [x] 数据结构：优先实现 `map`（键值容器），后续再补数组
  - [x] 字面量：`{ key: value, ... }`（key 仅常量；重复 key 后者覆盖；允许尾逗号）
  - [x] 类型：`map` 与 `map<K,V>`（K: `string/int/long`；V: `int/long/double/bool/string`，第一版）
  - [x] 读取：`m[k] -> Option<V>`（未命中返回 `None()`；可用 `??` 提供默认值）
  - [x] 写入：`m[k] = v`（当 `m` 为变量且为 `null` 时自动初始化）
  - [x] 内建方法：`len()/hasKey()/get()/delete()/clear()`
  - [x] 遍历：`for k,v in m { ... }`
  - [x] 完整强类型（第一版）：对 `map<K,V>` 写入/字面量做静态检查（禁止写入 `null`/错误类型）
  - [x] 类型推断（第一版）：从字面量推导 `map<K,V>`（仅当 key/value 都是非空字面量且类型一致时）
- [ ] 基础类型：把 `string` 做成真正的运行时基础类型（而不是仅 `i8*`/printf 直出）
- [ ] 空值：实现 `null` 作为空值，并定义比较/打印/条件判断/赋值规则
- [ ] 内存管理（短期）：RC（引用计数）+ 周期处理策略（检测/弱引用/限制形成环）
- [ ] 内存管理（中长期）：后续切换/替换为增量 GC（标记-清扫/分代，配写屏障）

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

#### 11.1 语言/编译器（通用能力，不专属于 LLM）
- [ ] 基础数值类型：`u8/i8/f32`（至少）与明确的溢出/转换规则
- [ ] 高效 `bytes`/`slice<T>` 视图（避免把 `string` 当字节容器）
- [x] 通用 FFI：Tua 侧声明外部符号与签名（例如 `extern fn ...`），避免在编译器里维护函数名白名单
- [ ] 构建/链接：通用方式引入外部库（静态/动态），不为 LLM 单独加 `tuac llm ...` 子命令

#### 11.2 `tua_rt`（跨平台底座）
- [ ] 大文件能力：流式读取 + `mmap`（POSIX 第一版），Windows 先 stub
- [ ] 线程/并行：workqueue + 原子/CPU feature 探测（为 SIMD/量化做准备）
- [ ] RNG：可复现的基础随机数（用于 sampling；也可由 std/llm 自带实现）

#### 11.3 `std`（通用库，LLM 可复用）
- [ ] `std.bytes` / `std.io`：Reader/BufReader、bytes 操作、UTF-8 边界工具
- [ ] `std.json`（轻量实现即可）：模型配置/metadata/推理参数解析
- [ ] Tokenizer：BPE（GPT-2 风格）或 sentencepiece（择一），先做正确性再做性能

#### 11.4 `llm` 外部包（应用代码：模型/推理/采样）
说明：可以是仓库内 `packages/llm`（或 `examples/llm`），也可以是外部独立 repo；核心要求是 **不依赖编译器特判**。
- [ ] 模型格式：先支持一种主流格式（建议 GGUF），能解析 metadata 与 tensor
- [ ] 数学内核：f32 baseline matmul/dot（可先朴素），再逐步并行化/向量化
- [ ] KV cache：数据结构与更新（注意内存占用与布局）
- [ ] Sampling：softmax + temperature + top-k/top-p + RNG（可复现）
- [ ] Runner：提供 `examples/llm/run.tua`（或独立 CLI 工具），通过 `tuac run ...` 运行

#### 11.2 R-llm-1：性能与量化
- [ ] 量化：q4/q8（至少一种）+ 对应 dot kernel
- [ ] SIMD：SSE/AVX/NEON（按平台探测），逐步替换 baseline
- [ ] Prefill/Decode 调度：线程划分、batch、缓存友好

#### 11.3 R-llm-2：工程化与生态
- [ ] `std.http`（可选）：模型/配置加载
- [ ] 流式输出：token-by-token callback/iterator 语义（对接 UI/Agent）
