# 重构整改清单（带状态）

更新时间：2026-01-11

## 状态定义

- `DONE`：已完成（有测试/bench 证明）
- `IN-PROGRESS`：进行中（主干可编译）
- `PLANNED`：已确认，尚未开始
- `PARTIAL`：部分完成（还有关键子项缺失）
- `DEFERRED`：暂缓（明确原因/条件）

## P0：回归与安全网（先做）

- `DONE` ASan/UBSan 跑通：已新增 `make tuac-asan/tuac-ubsan` 与 `make test-asan/test-ubsan`，并修复 sanitizer 暴露的问题使其可稳定作为门禁（macOS 下 ASan 需 `detect_leaks=0`）。
- `DONE` tests 可切换编译器：`tests/run.sh` 支持通过 `TUA_TUAC` 指定要运行的 `tuac`（便于 sanitizer/实验编译器/嵌入版验证）。
- `DONE` 嵌入基础 smoke：新增 `make rt-state-smoke-run`，验证 `tua_state_set_current` 下 allocator/panic/loc 的基本行为。
- `PLANNED` 性能回归门禁：为 `bench/run.sh` 增加“基线 + 阈值比较 + fail fast”。
- `PLANNED` 统一 debug/trace 开关：把散落的 `emitDebug/printf` 收敛成可控的宏/flag（并避免影响性能基准）。

## P1：清理遗留与重复逻辑

- `PLANNED` 清理 `src/llvm/llvm.c` 大段注释死代码：删除或迁移到文档/历史记录，保证文件只保留当前路径。
- `PLANNED` `src/parser.c` 错误处理去重：把“报错 + 同步恢复”的重复逻辑封装为少量函数，所有分支复用。

## P1：类型系统去硬编码

- `PLANNED` 消除 `analyzer.c` 里基于名字的硬编码判断（例如 `atIsBytes`）：引入稳定的 `type-id`/intern symbol，并集中维护 `atIs*` 判定规则。
- `PLANNED` 合并 `atIs*` 重复判断：形成统一的 predicate/flag 表，减少多处字符串/结构比较。

## P1：内存管理整改（可定位 + 减少碎片）

- `PLANNED` 编译器侧引入 arena/region：AST/Type/符号表等走 arena，减少 `malloc` 风暴与碎片化。
- `PARTIAL` 统一分配入口：runtime 已落地 `tua_alloc` + `tua_rt_configure`（见 `src/rt/rt_alloc.h`、`src/rt/rt_config.h`），并通过 `src/tuac_alloc.h` 收敛编译器侧分配；同时已引入 `tua_state`（TLS current）作为过渡形态，但真正的“显式传参 per-instance（无 TLS）”仍未实现。
- `PLANNED` 泄漏/释放追踪：至少支持测试进程退出时输出未释放计数（在 debug 模式），并配合 ASan 做回归。
- `PLANNED` 兼顾嵌入场景的 allocator 注入：允许宿主提供 per-instance 分配器（Lua 风格 realloc 签名），并明确跨边界释放必须回到同一 allocator。

## P2：错误处理一致性

- `PLANNED` 统一编译期错误接口：强制带 file/line/col（例如统一走 `compilerErrorAt`），避免多套打印格式。
- `PLANNED` 统一运行时错误策略：区分 `panic` 与错误码返回的边界，并写入规范（尤其是 std/rt 的 API）。

## P2：编译器结构与模块化

- `PLANNED` `main.c`/CLI/前端/后端拆分：入口只负责参数与调度；功能按目录/文件聚合。
- `PLANNED` 收敛“side-channel 元数据”：typed-map/array/slice 等的辅助信息集中管理，减少遗漏路径导致的 bug。
- `PLANNED` 维护 `FILE_STATUS.md`：持续标注“仍在使用/待清理候选”，作为删文件前的依据。

## P2：运行时/Codegen ABI 边界收敛

- `PARTIAL` typed-map 性能优化已引入（包含内联探测循环），但存在 ABI 耦合点：需要把“结构体布局依赖/offset 假设”收敛为显式约束（导出 offset 常量或加静态断言/版本号）。
- `PLANNED` 明确“允许内联/必须走 runtime API”的边界：避免后续优化碎片化成大量特判。

## P3：测试体系重构与补齐

- `PLANNED` tests 目录职责收敛：`tests/` 只保留 core+std；packages 的测试迁移到 `packages/**/tests` 并按类别分目录。
- `PLANNED` 边界/异常输入测试补齐：lexer/parser/utf8/map（rehash/tombstone）/unsafe 等。
- `PLANNED` 最小 fuzz：先从 lexer/parser/utf8 起步（离线脚本即可），后续再做持续化。

## P3：格式化与代码风格落地

- `PARTIAL` 已有 `tools/tua_fmt.py`，但未接入标准流程且 C 侧缺少统一格式化。
- `PLANNED` 引入 `.clang-format` + `make fmt/fmt-check`（先不强制，待收敛后再做门禁）。
- `PLANNED` 增加 `docs/STYLE.md`：命名（如 `tua_state` 风格）、缩进、错误处理、文件拆分规则。

## P3：文档对齐与状态透明

- `PLANNED` 对齐 `docs/SPEC.md` / `docs/ROADMAP.md` / `docs/PACKAGE_SPEC.md` / `docs/PACKAGE_ROADMAP.md`：把 packages（如 JSON）相关从 core roadmap 剥离，保证状态一致、可追踪。
- `PLANNED` 增加“模块状态表”：每个模块的实现/测试/性能/计划一页表，避免“代码已存在但文档标延迟”的不透明。
- `PLANNED` 嵌入能力文档：补齐 `tua_state/tua_config`（allocator/panic/loader/沙箱/线程模型）规范草案，并持续对齐实现（见 `docs/EMBEDDING_ALLOCATOR.md`）。
