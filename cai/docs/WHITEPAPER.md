# Cai 语言：面向开发者的先进性与“获得感”白皮书

Status: Draft

本文面向“从 Python + C/C++ 生态迁移到 Cai”的开发者，描述 Cai 在性能之外提供的“获得感”：把旧生态中极度痛苦、甚至难以团队化落地的能力，变成语言/工具链/标准库的默认能力。

本文不是语言语法规范；其中内容会按 **冻结层级**逐步转入 `cai/docs/spec/*`（语言语义）与 `cai/docs/COMPILER.md`（编译器管线约束）。

---

## 0. 冻结层级（重要）

为避免“愿景写成规范”导致不可实现或约束过早，本文把能力分为 3 个层级：

1) **Frozen 语义承诺**：写入 `cai/docs/spec/*` 的内容，属于语言/标准库可依赖语义，不得随意改变。
2) **Frozen 工具链合同**：写入 `cai/docs/COMPILER.md` 或专门的工具链合同文档，冻结输出/接口形态（例如 MIR dump 格式或诊断稳定字段）。
3) **Tooling Goal（目标能力）**：本文中的目标/体验承诺，允许在实现期调整；但一旦落地并稳定，会升级到 1) 或 2)。

---

## 1. 编译期显存预警：告别 OOM

### 1.1 目标体验（Tooling Goal）
在 `caic build` 阶段，编译器对“目标设备（GPU/加速器）上的峰值显存占用”给出预警与建议：
- 在不运行代码的情况下，尽可能在编译期发现 OOM 风险；
- 超标时给出可操作建议（缩短生命周期、切分 batch、换精度路由等）。

### 1.2 必须加边界（避免不可信承诺）
“准确预判峰值显存”只有在一定前提下可证明；否则只能是估算或需要插桩验证：
- 动态 shape、数据依赖分支、第三方库内部缓存/碎片、设备 allocator 策略等，都会使“静态准确峰值”不成立。

### 1.3 建议的落地形态（可升级为 Frozen 工具链合同）
输出分为两档：
- **可证明上界**：仅针对 shape 可静态界定（或可推导上界）的张量路径，给出“必不低估”的上界与触发点。
- **估算与插桩**：对无法证明的路径给出估算，并提供一键插桩模式采集真实峰值，与静态估算做差分。

与既有规范的关联：
- 形状/步长的可表示与可验证：`cai/docs/spec/tensor.md`
- 借用/生命周期与释放边界：`cai/docs/spec/ownership.md`、`cai/docs/spec/ref.md`、`cai/docs/spec/slice.md`
- 编译器分层：`cai/docs/COMPILER.md`

---

## 2. “逻辑热重载”：GB 级权重零重载

### 2.1 目标体验（Tooling Goal）
在开发模式下，代码逻辑与权重（尤其是已驻留显存/大页内存的权重）物理分离：
- 修改 `async fn preprocess()` 或推理管线中的逻辑后，可热更新；
- 已加载的巨大权重保持不动，仅更新逻辑指令流；
- 快速获得实验反馈，缩短迭代周期。

### 2.2 必须加边界（否则会变成“不可用的承诺”）
热重载的可行性取决于是否能冻结以下边界：
- 逻辑的可替换单元（例如 `dylib` 级别或函数表级别）；
- ABI/状态布局变化的限制（例如禁止更改某些导出符号签名、禁止更改持久状态布局）；
- 资源与所有权的责任边界（权重驻留由 runtime 管理，重载只替换纯逻辑）。

与既有规范的关联：
- 产物类型与初始化入口：`cai/docs/spec/aot.md`
- 链接与符号：`cai/docs/spec/linking.md`
- 资源嵌入与只读视图：`cai/docs/spec/embed.md`、`cai/docs/spec/slice.md`

---

## 3. 自动算子提升（Auto-Kernel Lifting）

### 3.1 目标体验（Tooling Goal）
开发者用可读的 Cai 循环/张量 API 编写自定义算子，编译器自动把它提升为高性能内核：
- 无需手写 CUDA C++、Triton 或内联汇编；
- 编译器负责并行映射、向量化、局部内存优化等；
- 失败时给出原因（为何无法提升）与可修复建议。

### 3.2 必须是“受限子集”（建议未来冻结）
自动提升必须建立在明确可分析的语义上，否则会“悄悄改语义”：
- 仅允许无隐藏副作用的计算（或副作用受严格约束）；
- 仅允许可证明的别名关系（依赖借用/权限语义）；
- shape/stride/边界必须可推导或有上界；
- 并行语义必须显式（建议通过 `@kernel`/`@pure` 等属性冻结）。

与既有规范的关联：
- MIR 主管线与 tensor 使用 MLIR：`cai/docs/COMPILER.md`
- 性能可依赖语义：`cai/docs/spec/performance.md`

---

## 4. 原生 LLM 结构化协议（Native Structured LLM Support）

### 4.1 目标体验（Tooling Goal）
把“文本 -> 对象”的流程变成强类型、可验证、可复现的调用：
- 模型输出直接约束为目标类型；
- 自动生成/携带 schema；
- 自动校验与可控重试；
- 与工具调用（tool calling）共享同一套 schema 与审计边界。

示意（以现有语法风格表述）：

```cai
struct UserProfile { name: string, age: int }

fn main() -> int {
  let profile = std.ai.complete<UserProfile>(model, prompt) ? { return 1 }
  println(profile.name)
  return 0
}
```

### 4.2 必须避免“语言核心膨胀”
该能力更适合落在 `std.ai` 与工具链：
- 语法层保持最小化；
- 通过 `@attribute`（例如 `@tool`）描述工具 schema 与安全元数据；
- 通过标准库完成 schema 生成、校验、重试与可复现日志。

与既有规范的关联：
- 错误模型与 guard：`cai/docs/spec/error.md`、`cai/docs/spec/expressions.md`
- `@attribute`：`cai/docs/spec/attributes.md`
- `requires` 不作为语言语法：`cai/docs/SYNTAX.md`

---

## 5. 跨平台“零成本”迁移（开发者体验）

### 5.1 目标体验（Tooling Goal）
同一套源码：
- 在 macOS 上默认路由到 Metal；
- 在 Linux 服务器上默认路由到 CUDA；
- 开发者无需为后端差异写两套业务逻辑。

### 5.2 与 Cai 的 AOT 定位一致的表述
Cai 不承诺“一次编译多次运行”；跨平台体验应当表述为：
- **同一源码**，针对不同目标平台分别构建；
- 构建/运行配置确定时，后端路由结果可复现。

与既有规范的关联：
- 平台分别编译/链接：`cai/docs/spec/ffi.md`、`cai/docs/spec/aot.md`
- 编译器分层与 tensor lowering：`cai/docs/COMPILER.md`

---

## 6. 极致调试器：MIR 态查看器

### 6.1 目标体验（Tooling Goal）
调试器不仅展示变量值，还能可视化：
- Tensor 生命周期（何时分配/释放/驻留）；
- 所有权转移与借用关系；
- 导致显存峰值的路径与原因。

### 6.2 走“工具链合同”而不是语言规范
该能力应通过工具链合同冻结（例如 `caic mir ...` 的输出格式、可稳定字段），避免把调试器实现细节写入语言语义规范。

与既有规范的关联：
- MIR 管线：`cai/docs/COMPILER.md`
- 性能语义：`cai/docs/spec/performance.md`
- 所有权/借用语义：`cai/docs/spec/ownership.md`

---

## 7. 建议补充的“获得感”能力（同样重要）

以下能力往往比单点性能更能驱动团队迁移：

1) **可复现 AI 运行（Reproducible Runs）**
   - 自动记录模型/权重哈希、tokenizer 版本、采样参数、seed、prompt 模板版本、工具调用轨迹；
   - 一键回放与差分对比（同输入不同版本的输出差异）。

2) **能力边界与审计默认化**
   - 不依赖语言语法 `requires`；
   - 通过构建/运行配置 + 审计日志把网络/进程/文件写等外部效应纳入可控边界。

3) **跨库零拷贝的标准化通道**
   - 以稳定 ABI 的视图结构在库与库之间传递（不复制数据、明确生命周期责任）。

