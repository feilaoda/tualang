# CAI 语言规范

本目录包含 CAI 的“可冻结规范”（Specification），按主题拆分到独立文件中。每个文件应当做到：
- **自包含**：该主题的所有规范集中在该文件内（必要时可少量引用通用术语定义，但不依赖外部项目）。
- **可实现**：规则清晰到足以指导编译器/标准库/工具链实现。

状态标记约定：
- `Status: Frozen`：已冻结；破坏性改动必须走版本升级。
- `Status: Draft`：未冻结；允许调整，但每次调整都必须保持规则自洽、可实现，并在实现前完成冻结。

## 目录
- `cai/docs/spec/lexical.md`：词法与源代码形式规范
- `cai/docs/spec/prelude.md`：预导入与内建符号规范
- `cai/docs/spec/null.md`：`null` 与可空语义规范
- `cai/docs/spec/types.md`：类型系统总览
- `cai/docs/spec/numeric.md`：数值类型与运算规范
- `cai/docs/spec/abi.md`：ABI 与数据布局规范
- `cai/docs/spec/bindings.md`：绑定与赋值（`let/const`、解构）规范
- `cai/docs/spec/expressions.md`：表达式语义（求值顺序、短路、cast、`??`、`? {}`）规范
- `cai/docs/spec/operators.md`：运算符（类型规则与语义）规范
- `cai/docs/spec/controlflow.md`：控制流（`if/match/for/while/do/unsafe/return/break/continue/goto/defer`）规范
- `cai/docs/spec/ownership.md`：所有权、move、drop 与借用生命周期规范
- `cai/docs/spec/async.md`：`async/await` 并发语义规范
- `cai/docs/spec/runtime.md`：运行时（调度、I/O 事件驱动）规范
- `cai/docs/spec/attributes.md`：`@attribute` 注解与元数据规范
- `cai/docs/spec/package.md`：包与构建清单（`cai.toml` / `cai.lock`）规范
- `cai/docs/spec/policy.md`：能力策略与审计规范
- `cai/docs/spec/ffi.md`：FFI（`extern fn`、ABI、类型映射）规范
- `cai/docs/spec/aot.md`：AOT 产物（`exe/staticlib/dylib`）规范
- `cai/docs/spec/linking.md`：链接（静态优先、符号冲突）规范
- `cai/docs/spec/std_error.md`：`std.error`（错误码与错误分类）规范
- `cai/docs/spec/std_runtime.md`：`std.runtime`（运行时接口）规范
- `cai/docs/spec/std_fs.md`：`std.fs`（文件系统）规范
- `cai/docs/spec/std_net.md`：`std.net`（网络）规范
- `cai/docs/spec/std_time.md`：`std.time`（时间）规范
- `cai/docs/spec/std_process.md`：`std.process`（进程）规范
- `cai/docs/spec/std_thread.md`：`std.thread`（线程）规范
- `cai/docs/spec/std_atomic.md`：`std.atomic`（原子）规范
- `cai/docs/spec/std_fs_windows.md`：`std.fs`（Windows 路径映射）规范
- `cai/docs/spec/object.md`：`object`（命名空间）规范
- `cai/docs/spec/visibility.md`：可见性与 `private` 规范
- `cai/docs/spec/diagnostics.md`：诊断与错误输出规范
- `cai/docs/spec/performance.md`：性能语义与优化边界规范
- `cai/docs/spec/error.md`：错误模型（错误码、多返回、guard、运行时错误）规范
- `cai/docs/spec/string.md`：`string` 规范
- `cai/docs/spec/array.md`：数组（`T[]`/`T[N]`）规范
- `cai/docs/spec/slice.md`：`Slice<T>`（由编译器跟踪只读/可写权限）规范
- `cai/docs/spec/bytes.md`：`bytes`（拥有型字节序列）规范
- `cai/docs/spec/embed.md`：资源嵌入（`embed`）规范
- `cai/docs/spec/option.md`：`Option<T>` 规范
- `cai/docs/spec/any.md`：`any`（动态值）规范
- `cai/docs/spec/map.md`：`map`/`map<K,V>` 规范
- `cai/docs/spec/ptr.md`：`Ptr<T>`（限制性指针）规范
- `cai/docs/spec/ref.md`：`Ref<T>` / `&T`（引用/借用）规范
- `cai/docs/spec/box.md`：`std.memory.Box<T>` 规范
- `cai/docs/spec/tensor.md`：`std.tensor`（稳定 ABI 张量）规范
- `cai/docs/spec/fn.md`：函数（`fn`/lambda/多返回/泛型/extern/async）规范
- `cai/docs/spec/import.md`：模块系统与导入（`import/from/as`）规范
- `cai/docs/spec/struct.md`：`struct`（字段/方法/init/deinit/嵌入字段/可见性）规范
- `cai/docs/spec/enum.md`：`enum`（tag/值/布局）规范
- `cai/docs/spec/trait.md`：`trait` 规范（静态分发）
- `cai/docs/spec/impl.md`：`impl` 规范（方法实现/trait 实现/一致性规则）
- `cai/docs/spec/match.md`：模式匹配（`match`）
- `cai/docs/spec/unsafe.md`：`unsafe`
