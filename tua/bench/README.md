# Benchmarks

目标：用一组常见场景对比 Tua（AOT）与等价 C 代码的性能趋势。

## 场景

- `arith_int`：整数循环 + 简单运算（避免被优化掉）
- `dot_f32_1024`：1024 长度 float 点积（重复多次）
- `bytes_scan_1m`：扫描 1 MiB bytes 统计某个字节出现次数
- `bytes_scan_1m_slice`：扫描 1 MiB `Slice<byte>`（避免每字节 FFI/getU8）
- `map_lookup_8k`：8k 键空间的随机查表（Tua: `map<int,int>`；C: 线性探测哈希表）
- `map_lookup_foo_8k`：8k 键空间的随机查表（Tua: `map<int,Foo>`；C: 线性探测哈希表（值为 `{a:int}`））
- `intintmap_lookup_8k`：8k 键空间的随机查表（Tua: `packages/map/IntIntMap`；C: 同 `map_lookup_8k` 基线）
- `json_scan_top_long`：扫描 JSON 顶层字段 `id`（输入：`bench/data/scan_top_level.json`）

## 运行

在仓库根目录：

```bash
./bench/run.sh
```

或用 Makefile 快捷目标：

```bash
make bench
```

只跑一个用例：

```bash
./bench/run.sh --bench arith_int --iters 100000000
```

说明：
- 脚本会设置 `TUA_STDLIB_DIR` 和 `TUA_PACKAGE_DIR`，并用 `--llvm-O3 --no-loc` AOT 编译 `bench/tua/bench.tua`。
  - `--no-loc` 会关闭运行时定位插桩（`tua_set_loc`），否则 tight loop 会被行号/列号更新严重拖慢（你看到的巨大差距主要来自这里）。
- 脚本还会额外构建：
  - `--llvm-native` 版本（`bench/bin/bench_tua_native`）：用本机 CPU/特性（SIMD 等）做更接近“上限”的对比；生成的可执行文件不保证跨机器可用。
  - `--unchecked-index` 版本（`bench/bin/bench_tua_unchecked`）：用于观察“无边界检查”的上限性能（越界访问属于 UB）。
- 你也可以手动运行：
  - Tua：`./bin/tuac --llvm-O3 --no-loc --output bench/bin/bench_tua bench/tua/bench.tua && bench/bin/bench_tua --all`
  - Tua（native）：`./bin/tuac --llvm-O3 --no-loc --llvm-native --output bench/bin/bench_tua_native bench/tua/bench.tua && bench/bin/bench_tua_native --all`
  - Tua（unchecked）：`./bin/tuac --llvm-O3 --no-loc --unchecked-index --output bench/bin/bench_tua_unchecked bench/tua/bench.tua && bench/bin/bench_tua_unchecked --all`
  - C：`cc -O3 -DNDEBUG -std=c11 bench/c/bench.c -o bench/bin/bench_c && bench/bin/bench_c --all`
  - C（native）：`cc -O3 -DNDEBUG -march=native -std=c11 bench/c/bench.c -o bench/bin/bench_c_native && bench/bin/bench_c_native --all`

## LuaJIT 对比

先跑 LuaJIT 基准：

```bash
./bench/run_luajit.sh --luajit /Users/feilaoda/workspace/lowcodecloud/luajit/src/luajit
```

或：

```bash
make bench-luajit LUAJIT_BIN=/Users/feilaoda/workspace/lowcodecloud/luajit/src/luajit BENCH_ROUNDS=3
```

做 Tua vs LuaJIT 对比（默认 5 轮取中位数，输出 CSV）：

```bash
./bench/compare_luajit.sh \
  --luajit /Users/feilaoda/workspace/lowcodecloud/luajit/src/luajit \
  --rounds 5 \
  --profile native
```

或：

```bash
make bench-compare \
  LUAJIT_BIN=/Users/feilaoda/workspace/lowcodecloud/luajit/src/luajit \
  BENCH_ROUNDS=5 \
  BENCH_PROFILE=native
```

说明：
- `bench/compare_luajit.sh` 默认会先构建（调用 `bench/run.sh --all`），然后分别跑 Tua 与 LuaJIT。
- 结果会打印到终端，同时写入 `bench/bin/compare_tua_luajit.csv`。
- LuaJIT 脚本在 `bench/luajit/bench.lua`，包含 warmup，减少首次 trace 带来的抖动。
- 如果只想复用已构建产物，可加 `BENCH_NO_REBUILD=1`。
