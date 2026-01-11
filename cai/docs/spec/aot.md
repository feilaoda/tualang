# AOT 产物（`exe/staticlib/dylib`）规范

Status: Frozen

本文件冻结 CAI 的 AOT（Ahead-Of-Time）编译产物类型，以及产物在“模块初始化/运行时/导出符号”方面的最低要求。

---

## 1. 产物类型

CAI 的 AOT 编译器（例如 `caic`）必须支持以下三类输出：

### 1.1 `exe`（可执行文件）
- 输出一个原生可执行文件。
- 必须包含 CAI 核心运行时与所有静态链接依赖（见 `cai/docs/spec/linking.md`）。
- 程序入口必须触发模块初始化（见 `cai/docs/spec/import.md`），然后调用用户的 `main`。

### 1.2 `staticlib`（静态库）
- 输出一个可被 C/C++ 链接器使用的静态库（例如 `.a`/`.lib`）。
- 必须同时生成一个 C 头文件（`.h`），用于声明可被宿主调用的导出符号（见 3）。

模块初始化：
- `staticlib` 不自动执行模块初始化。
- 工具链必须生成并导出一个初始化入口（规范名冻结）：
  - `extern fn cai_init() -> int`
  - 返回 `0` 表示成功；非 `0` 表示失败（错误码语义见 `cai/docs/spec/error.md`）。
- `cai_init()` 的语义是按 `cai/docs/spec/import.md` 的初始化顺序执行该库包含的所有模块初始化一次。

### 1.3 `dylib`（动态库）
- 输出一个可被系统动态加载的动态库（例如 `.so`/`.dylib`/`.dll`）。
- 必须支持导出符号重命名（见 3）。
- `dylib` 不自动执行模块初始化；宿主必须显式调用 `cai_init() -> int`（见 1.2）完成初始化。

---

## 2. 产物与平台
- CAI 不承诺“一次编译多次运行”；每个目标平台分别编译与链接（见 `cai/docs/spec/ffi.md` 与 `cai/docs/spec/linking.md`）。

---

## 3. C 兼容导出（面向宿主调用）

### 3.1 导出声明方式
导出通过 `@attribute` 完成：
- `@export`：将一个 CAI 函数导出为 C ABI 可调用符号。
- `@export_name(name="...")`：指定导出符号名（默认使用函数标识符名）。

导出规则：
- `@export` 只能用于**顶层** `fn`（不能用于 `extern fn`、lambda、方法）。
- 被导出函数必须是**非泛型**（禁止 `typeParamList`），否则为编译错误。
- 被导出函数的参数与返回必须是 FFI-safe 类型（定义见 `cai/docs/spec/ffi.md`）。

返回值规则更新（冻结）：
- 允许导出多返回；其 ABI 规则按 `cai/docs/spec/abi.md` 的“多返回映射”为 C ABI 可表达的聚合返回。
- `.h` 头文件生成器必须为多返回生成一个具名 `struct` 返回类型，并使用该类型作为函数返回值。

返回 `struct` 命名规则（冻结）：
- 返回结构体名为 `cai_ret_<mangled>`；
- `<mangled>` 的输入为“最终导出符号名”（考虑 `@export_name`），再做如下稳定转换：
  - 将非 `[A-Za-z0-9_]` 的字符替换为 `_`；
  - 若产生冲突，追加稳定后缀（例如 `_2/_3/...`），工具链必须保证不依赖构建机器的随机性。

### 3.2 头文件生成（`staticlib` 最低要求）
生成的 `.h` 文件必须包含：
- 所有 `@export` 函数的原型（按 `cai/docs/spec/ffi.md` 的类型映射）。
- `cai_init() -> int` 的原型。
- `extern "C"` 兼容包装（若生成器支持 C++）。

---

## 4. 资源嵌入（与 `embed` 的关系）
若程序使用 `embed`（见 `cai/docs/spec/embed.md`）：
- `exe/staticlib/dylib` 都必须把被嵌入的资源包含进产物（或其等价的链接单元）中；
- 运行时访问该资源不得依赖外部文件 I/O。
