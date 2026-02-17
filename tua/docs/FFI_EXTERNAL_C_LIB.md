# 外部 C 类库（不进编译器仓库代码）

目标：把性能关键逻辑写在独立的 C 库里，通过 `extern fn` 在 Tua 中调用；同时 C 代码可以 `#include` Tua 运行时的头文件（`src/*.h`），但不需要把 C 源码合进 `tuac`/运行时实现里。

仓库内置的“可选 C 内核”也遵循这个约定：

- `packages/clib/llm/*`：LLM kernels（导出 `tua_llm_*`），构建：`./tools/build_tuallm_clib.sh`，使用：`-L build/clib -l tuallm`
- `packages/clib/json/*`：JSON scan/parse helpers（导出 `tua_json_*`），构建：`./tools/build_tuajson_clib.sh`，使用：`-L build/clib -l tuajson`

## 1) 编译 C 静态库（推荐）

仓库已提供脚本 `tools/build_clib.sh`，用于把一个或多个 `.c` 编译为 `lib<name>.a`：

`./tools/build_clib.sh tuaext examples/ffi_extlib_sum.c`

默认输出到 `build/clib/libtuaext.a`，并自动加上 `-I<repo>/src`，因此你的 C 代码可以直接：

- `#include "tua_array.h"`
- `#include "tua_map.h"`
- `#include "tua_bytes.h"`

如果你希望脚本/构建系统自动发现头文件目录，可以用 `tuac` 查询：

- `./bin/tuac --print-ffi-include-dir`（输出 `src` 目录）
- `./bin/tuac --print-ffi-cflags`（输出 `-I...`）
- `./bin/tuac --print-ffi-ldflags`（可选：构建可 `dlopen` 的 shared lib 时需要的链接参数；macOS 下会输出 `-Wl,-undefined,dynamic_lookup`）

也可以通过环境变量覆盖：

- `TUA_SRC_DIR=/path/to/tua/src`
- `OUT_DIR=/tmp/out`
- `CC=clang`
- `CFLAGS='-O2 -fPIC -I... ...'`

## 2) 在 Tua 中声明并调用

示例见 `examples/ffi_extlib_sum.tua`：

- `extern fn ext_sum_i64(ids: long[], outErr: &int) long`

注意：`long[]` 在 C ABI 中就是 `tua_array*`，数组元素类型通过 `elem_size` 区分（`long` => 8）。

## 3) 运行/链接方式

### JIT（默认 `tuac <file.tua>`）

方式 A：显式加载库（最明确）：

- `TUA_STDLIB_DIR="$PWD/std" ./bin/tuac --dlopen build/clib/libtuaext.a examples/ffi_extlib_sum.tua`

方式 B：用 AOT 的 `-L/-l` 语法（tuac 会在 JIT 启动前自动 `dlopen`；macOS/Linux 支持 `.a`）：

- `TUA_STDLIB_DIR="$PWD/std" ./bin/tuac -L build/clib -l tuaext examples/ffi_extlib_sum.tua`

提示：所有编译/链接类参数（`-L/-l/--link-arg/--dlopen/...`）必须放在源文件路径之前；源文件之后的参数会被当作脚本 `args` 传给运行时。

### AOT（生成可执行文件）

- `TUA_STDLIB_DIR="$PWD/std" ./bin/tuac --output bin/ffi_demo -L build/clib -l tuaext examples/ffi_extlib_sum.tua`

或直接把 `.a` 当成原始链接参数透传：

- `TUA_STDLIB_DIR="$PWD/std" ./bin/tuac --output bin/ffi_demo --link-arg build/clib/libtuaext.a examples/ffi_extlib_sum.tua`

## 4) 约定提醒

- `string` 在 C ABI 下是 `const char*`（UTF-8；语义为不可变值语义；运行时实现计划升级为自动管理的 string 类型，见 `docs/SPEC.md`。在语义冻结前，跨 ABI 的“谁负责释放”必须在函数文档中明确）
- 推荐：FFI 新接口优先用“带长度”的类型（例如 `Slice<byte>`），形如 `{uint8_t* data, int64_t len}`，避免 `strlen` 开销/内嵌 NUL/编码歧义
- `bytes` 是 `tua_bytes*`，`map` 是 `tua_map*`，`ptr` 是 `void*`
- `extern fn` 默认按“借用”语义使用传入的 `map/array/bytes/string`（callee 不应释放传入对象），除非你明确设计为“接管所有权”

## 5) 示例：为 tokenizer_bpe 提供 decode2（外部库）

示例实现：`packages/clib/llm/tua_extbpe.c`，导出符号：

- `tuaext_bpe_decode_ids_to_string(ids: long[], idToToken: map<long, string>, outErr: &int) string`

说明：
- 本仓库可能不包含对应的 Tua 封装模块（`packages/*.tua` 可能被暂时移除）；你可以在自己的包里写一个薄封装（`extern fn` + 小量 glue）。
- packages 级别的 tokenizer/llm 规划已迁移到 `docs/PACKAGE_SPEC.md` 与 `docs/PACKAGE_ROADMAP.md`。

构建（生成 `build/clib/libtuaextbpe.a`）：

- `./tools/build_clib.sh tuaextbpe packages/clib/llm/tua_extbpe.c`
