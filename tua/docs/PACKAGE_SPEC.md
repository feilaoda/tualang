## Tua Packages 规格（工作草案）

本文件用于记录 **不属于语言核心/运行时/标准库** 的内容：`packages/*`（Tua 包模块）与 `packages/clib/*`（可选 C 内核）。

约定：
- 语言核心语义以 `docs/SPEC.md` 为准；本文件不会引入任何“编译器特判”的语义。
- 包模块是可选的：仓库可能只保留 C helper（`packages/clib/*`），不保证存在对应的 `packages/<name>/*.tua`。

### 1. 包搜索路径（Frozen）
包模块的 import 解析不应依赖把 `packages/...` 写死在源码中。推荐通过 `TUA_PACKAGE_DIR`（或未来的嵌入式 ctx 配置）提供包搜索根目录。

默认查找规则（与实现保持一致，见 `docs/SPEC.md` 的模块/导入章节）：
- `TUA_PACKAGE_DIR` 为多路径列表：
  - POSIX：用 `:` 分隔
  - Windows：用 `;` 分隔
- 对非 `std/`、非 `./`/`../` 的导入路径（包导入），对每个根目录按序尝试：
  - `<root>/<raw>.tua`（若 raw 省略 `.tua` 会自动补）
  - 兜底：`<root>/<name>/<name>.tua`（`name` 为 import path 最后一段去掉 `.tua`）
    - 例：`import "json"` -> `<root>/json/json.tua`
    - 例：`import "llm/tokenizer"` -> `<root>/llm/tokenizer.tua` 或 `<root>/tokenizer/tokenizer.tua`（兜底仅按最后一段）

### 2. JSON（包模块，Status: Deferred）
说明：JSON 是通用能力，但当前仓库可能只保留 C 侧 helper（`packages/clib/json`），不保证存在 Tua 侧 `packages/json/*.tua`。

#### 2.1 C helper（Implemented）
位置：`packages/clib/json/*`，导出 `tua_json_*` 符号（见 `packages/clib/json/tua_json.h`）。

典型用法：
- 在 Tua 里 `extern fn tua_json_scan_top_level_long(b: bytes, key: string, outVal: &long) int`
- 链接：`-L build/clib -l tuajson`

错误码约定：遵循 `docs/SPEC.md` 的错误返回约定；`0` 表示成功，非 `0` 表示失败/未命中（具体含义见对应 C 头文件注释）。

#### 2.2 规划的 Tua API（Deferred）
若未来恢复 `packages/json`：
- `Json.parse(s: string) -> any, int`
- `Json.parseBytes(b: bytes) -> any, int`
- `Json.scanTopLevelLong(b: bytes, key: string) -> long, int`

### 3. LLM（包模块，Status: Deferred）
LLM 属于应用层逻辑：模型格式、推理图、KV cache、采样、runner 等均应位于独立包（仓库内或外部 repo），不得进入编译器/运行时/语言规范。

#### 3.1 C kernels（Implemented/Optional）
位置：`packages/clib/llm/*`，导出 `tua_llm_*` / `tuaext_*` 等符号。

原则：
- 仅提供与模型无关的“通用数学/字节/量化内核”或“性能关键解析/转换”；
- 不引入任何 `tuac` 的专用子命令，统一走通用 `extern fn` + `-L/-l/--dlopen`。

### 4. 版本与兼容性
本文件记录的包 API 允许迭代；如需破坏性变更，应同时更新 `docs/PACKAGE_ROADMAP.md` 并在 README 中标注。
