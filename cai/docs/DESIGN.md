# CAI 语言设计（草案）

> 目标：从 `tua/` 的“可运行原型”演进出一个 **C-speed + AI-native** 的系统级语言。

---

## 1. 从 Tua 总结出来的“已验证设计”

Tua 的定位可以概括为：
- **极简语法（Lua 风格）+ 静态类型 + LLVM JIT/AOT**：快速获得能跑的编译型语言。
- **无 GC**：追求“可预测性能、可控内存、嵌入友好”。
- **分层冻结**：`tua_rt`（小而稳定的 C runtime/平台抽象）+ `std/`（尽量用语言自身实现）+ `packages/`（非核心能力，可选）。
- **默认安全的所有权模型（逐步冻结中）**：
  - 复合类型默认 **move-only**，通过编译期所有权/借用检查避免 UAF/double-free；
  - 函数参数“**默认借用，显式 move**”降低心智负担；
  - `Ref<T>`/`&T`（过渡）区分共享只读与独占可写借用；借用寿命做了 NLL（语句级）。
- **工程现实主义**：把 LLM/JSON 等“应用层能力”放进 `packages/`，性能关键部分可做成独立 C 静态库，通过 `extern fn` 链接（不污染编译器/运行时）。
- **异步与系统能力的最小闭环**：`tua_rt` 自带事件循环 + workqueue + fs/net/time，`std` 提供同步/异步 API。

这些结论对 CAI 的意义是：CAI 的“C 速度”基础设施（LLVM/AOT、无 GC、FFI、runtime 分层与跨平台原语）已经在 Tua 中走通了一大部分；CAI 更应该把精力放在 **冻结语义、提升可维护性/可扩展性**，以及 **把 AI 能力做成语言与生态的第一优先级**。

---

## 2. CAI 的产品目标（What / Why）

### 2.1 性能与部署（C-speed）
- 默认 **AOT** 输出可执行/静态库/动态库；JIT/REPL 作为工具模式可选。
- 零开销抽象：泛型单态化、内联、跨模块优化（逐步推进）。
- 小运行时：不引入 GC；资源释放可预测（RAII/drop）。
- 与 C ABI 无缝互操作：可直接复用系统库/现有推理库（llama.cpp、ggml/gguf、BLAS、Metal/CUDA 等）。

### 2.2 AI-native（AI 能力一等公民）
CAI 不把“AI”理解为编译器偷偷帮你写代码，而是把 AI 能力当作和 `fs/net/time` 同等级的系统能力：
- **模型推理**：本地模型优先（可选远程 provider），支持流式 token 输出。
- **结构化输出**：把“从 LLM 得到结构化结果并校验”变成强类型 API，而不是散落的 JSON 拼接。
- **工具调用（tool calling）**：函数暴露给模型的 schema、权限、幂等性、审计信息明确可控。
- **可复现**：模型版本/权重哈希、采样参数、seed、prompt 模板版本可记录，便于回放与测试。
- **安全边界**：AI 调用是 I/O/外部效应；即使不做“可嵌入”，仍应通过构建/运行参数、配置文件与审计日志做到可控与可追踪（而不是靠隐式默认）。

---

## 3. 语言核心（语法与语义）

### 3.1 语法风格
继承“Lua 风格 + 结构化声明”作为 CAI 的表面语法（学习成本低），同时把“工具/AI”相关语法做成最小增量：
- 语句默认换行分隔（可选 `;`）。
- `struct/enum/trait/impl` + 泛型单态化。
- `Option<T>`/`Result<T, E>`（或“多返回 + 错误码”二选一并冻结）。

### 3.2 内存与资源模型（必须先冻结）
CAI 的“C 速度”要以“没有隐藏开销”为前提，因此必须先冻结：
- **默认 move-only**：复合类型赋值/传参转移所有权；需要共享时显式借用或显式 `clone()`。
- **借用检查**：共享只读 vs 独占可写；与容器（map/array/slice）交互的规则必须清晰（避免隐式产生第二个 owner）。
- **drop/析构**：作用域结束、覆盖赋值、`return` 路径的释放时机确定；支持 `deinit`/资源句柄 `close()` 规范。

### 3.3 并发与异步
建议把异步作为语言级能力（不是库“回调地狱”）：
- `async fn` + `await` + `cancel` token（与 runtime loop/workqueue 对齐）。
- `select`/`race`（可选）用于组合多个异步源（网络/计时器/流式 token）。

### 3.4 错误模型（C 友好且可组合）
CAI 需要一个同时满足“零成本、可读、可与 C 互操作”的错误模型。两条可行路线：
- **沿用 Tua 的多返回 + 错误码**：`fn f(...) -> T, int`，并提供 `? { ... }`/`?` 语法糖做早退出（非常贴合 C/FFI）。
- **引入 `Result<T, E>`**（或 Zig 风格 error union）：更强的类型表达，但需要冻结 ABI/布局与工具链支持。

CAI 采用“多返回 + 错误码”，并把错误码扩展为可选携带结构化信息（例如 `AiError{code, msg, kind, retryable}`），在不牺牲性能的前提下提升可诊断性。

### 3.5 指针与互操作（只提供限制性 `Ptr<T>`）
CAI 不暴露“裸指针类型关键字”（例如 `ptr`/`void*`）作为语言内置类型；指针只以受限形式出现：`Ptr<T>`。

设计目标：
- 保留 C-speed 与 FFI 能力，但避免“地址到处传、随手算指针”的写法变成日常代码风格。
- 让“解引用/内存读写”成为显式、受控、可审计的操作（将来可接入 `unsafe`、静态分析或 sanitizer 友好约束）。

建议约束：
- `Ptr<T>` 仅允许相等比较与空判断；禁止算术、禁止与整数互转。
- 任何内存读写通过 `std.ptr` 的受控 API（例如 `read/write/copy`），而不是语言提供 `*p`、`p[i]` 等直接解引用语法。

FFI 建议形态：
- `extern fn` 的指针参数/返回值统一使用 `Ptr<T>`（而不是 `void*`/裸地址）。
- 需要“指针 + 长度”的场景，统一使用 `Slice<T>`/`Bytes` 这类显式结构（避免 `strlen`、避免 NUL 歧义）。
- 资源所有权不通过“free(ptr)”约定传播；对外部资源使用显式句柄类型（例如 `Handle<T>`）并定义 `close()/deinit` 释放点。

### 3.6 平台与权限（非嵌入式前提）
CAI 的目标是“多平台”，但不要求“一次编译到处运行”：
- 每个目标平台单独编译与链接（macOS/Linux/Windows 各自产物）。
- 权限控制不通过“嵌入式 API”，而通过 `caic` 的构建参数、运行时配置与日志审计实现（例如在生产构建中禁用网络 provider，仅允许本地模型）。

---

## 4. AI 能力的“最小语言增量”

原则：尽量放在 `std/` 与 `packages/`，但允许少量语法糖把“常见 AI 任务”变得可读、可检查、可审计。

### 4.0 使用形态（示意）

```cai
import "std"
import "ai/llm"

struct Todo { title: string, done: bool }

fn main() -> int {
  let m = llm.load("qwen2.5-7b-instruct.gguf", device: "metal") ? { return 1 }
  let t: Todo = m.completeJson<Todo>(prompt("给我一个待办事项")) ? { return 1 }
  println(t.title)
  return 0
}
```

### 4.1 Prompt 模板（推荐做成语法糖）
引入一种“可静态检查占位符”的字符串模板（或沿用现有格式化并增强），目标：
- 变量插入必须是显式且可类型检查；
- 支持把模板编译成 tokens（对本地 tokenizer 可选预编译），减少运行时开销。

### 4.2 结构化输出（强类型 API）
提供标准库约定：
- `model.complete<T>(prompt, options) -> T, AiError`
- `model.completeJson<T>(...)`：自动生成/携带 JSON schema（或自定义 schema），并做严格校验/重试策略。

### 4.3 Tool calling（受控外部效应）
引入注解/属性（语法可选）描述工具：
- schema（参数/返回）、权限（fs/net/process）、幂等性、超时、审计字段；
并让运行时在调用前后可插入 “human-in-the-loop / policy check”。

### 4.4 向量与张量（偏库，但要有好 ABI）
AI 侧常见数据结构应当在 ABI 上稳定且零拷贝友好：
- `bytes`/`slice<T>`/`tensor<T, shape>`（shape 可选作为类型参数或运行时字段）
- 设备抽象：`device = cpu|metal|cuda`（选择策略在库层，避免泄露具体平台句柄到语言核心）

---

## 5. 运行时与生态（实现落地）

建议沿用 Tua 的“分层冻结”，只把名字与边界升级为 CAI：
- `cai_rt/`：C runtime（alloc、错误码、loop、fs/net/time、线程/原子、dlopen/动态加载的可选开关）
- `std/`：语言自实现的通用库（容器、io、utf8、json、ai 基础抽象）
- `packages/`：LLM/embedding/vectorDB/模型格式/量化/后端等“应用层包”；性能热点允许配 `packages/clib/*`。

### 5.0 与 Tua 的复用策略（建议）
为降低风险，建议 CAI 直接复用已有资产，再逐步“抽离/重命名”：
- 复用 `tua/src/rt/*` 作为 `cai_rt` 的起点（偏“跨平台原语库”，而不是“可嵌入 VM API”）。
- 复用 `packages/clib/llm/*` 的内核与“可插拔后端”思路（CPU/Metal），把模型图与采样留在 CAI 包层实现。
- 编译器前端/LLVM 后端先从 `tuac_*` 拆出可复用库，再按 CAI 的语义冻结逐步替换实现细节。

### 5.1 编译器工具链
- `caic`：build/run/test/format（子命令可逐步演进）。
- 可选：语言服务器（LSP）、基准/剖析（perf/samp）、可复现构建（lockfile + artifact cache）。

---

## 6. 推荐路线（从 Tua 到 CAI）

1) **先冻结语义**：所有权/借用/drop、错误模型、模块/包、FFI ABI。
2) **把 AI 能力做成可用的标准库/包**：本地模型加载、流式输出、结构化输出、tool calling。
3) **性能工程**：AOT 默认、LTO/PGO、SIMD/BLAS/Metal 后端（参考 `tua/docs/llm_metal_plan.md` 的分层思路）。
4) **安全与可复现**：通过构建/运行配置做权限控制与审计；记录 prompt/参数/模型版本；建立测试基线。
