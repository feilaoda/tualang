# 嵌入场景：可注入分配器（草案）

目标：让 Tua 可以像 Lua 一样被宿主程序嵌入，并允许宿主提供自定义内存分配策略（统计/限额/专用堆/调试追踪）。

## 核心原则

- **per-instance**：分配器必须挂到实例（例如 `tua_state` / `tua_compiler`），不要用全局变量。
- **同堆释放**：由某个实例分配的内存，必须由该实例的分配器释放；禁止宿主用 `free()` 直接释放 Tua 返回的指针。
- **一元入口**：分配/释放/重分配统一走一个函数（realloc 风格），避免宿主需要同时提供 malloc/free/realloc 三套入口。
- **OOM 可控**：嵌入场景不应默认 `abort()`；至少提供可注入的 panic/oom handler 或错误码回传路径。

## 除 allocator 外，还需要补齐什么（嵌入能力清单）

- **实例化/去全局化**：把会影响行为的全局状态收敛到 `tua_state` / `tua_compiler`（alloc/panic、日志开关、包搜索路径、临时缓冲、统计、随机 seed 等），保证可同时创建多个实例且互不干扰。
- **错误回传模型**：为嵌入提供“非 abort”路径（例如 `tua_error { code, msg, file, line, col }`），并定义错误字符串/缓冲的分配与释放规则（必须回到实例 allocator）。
- **模块加载可控**：环境变量（`TUA_STDLIB_DIR/TUA_PACKAGE_DIR`）之外，还要支持 API 配置搜索路径/多路径优先级，并支持宿主自定义 loader（从内存、只读资源包等），以及禁用磁盘加载。
- **线程/重入模型**：明确 `tua_state` 是否线程安全（通常不是），是否允许多 state 并行，宿主如何加锁；明确回调（如 panic/loader）是否允许再次进入 Tua。
- **能力开关/沙箱面**：嵌入时通常要可禁用文件/网络/dlopen 等；建议把权限开关放到 `tua_config` 里（默认 `tuac` 开放，embed 默认更保守）。
- **跨边界资源管理**：FFI “不透明句柄”建议配套 `*_new/*_free` 规范或 finalizer 约定，避免泄漏与双重释放。
- **生命周期（重要）**：当 runtime 对象（如 `tua_workqueue_t`）在后台线程执行回调时，必须保证它捕获/依赖的 `tua_state` 在对象销毁前一直存活；否则后台线程可能访问已释放的 allocator/panic/配置。

## C API（建议形态）

参考 Lua：`alloc(ud, ptr, old_sz, new_sz)`。

```c
typedef void* (*tua_alloc_fn)(void* ud, void* ptr, size_t old_sz, size_t new_sz);
typedef void (*tua_panic_fn)(void* ud, const char* msg);

typedef struct tua_allocator {
    tua_alloc_fn alloc;
    void* ud;
} tua_allocator;

typedef struct tua_config {
    tua_allocator allocator;   // `.alloc == NULL` => 默认系统 malloc/realloc/free
    tua_panic_fn panic;        // NULL => 默认打印并 exit(1)
    void* panic_ud;
} tua_config;
```

语义约定：

- `alloc(ud, NULL, 0, n)`：分配 `n` 字节
- `alloc(ud, p, old, n)`：将 `p` 从 `old` 字节重分配到 `n` 字节（允许返回新地址）
- `alloc(ud, p, old, 0)`：释放 `p`

实现备注（当前仓库状态）：

- 目前 runtime 已提供统一入口 `tua_alloc()` 以及可注入 allocator（见 `src/rt/rt_alloc.h`），并提供 `tua_config/tua_rt_configure`（见 `src/rt/rt_config.h`）。
- 由于 runtime 尚未全量追踪每个块的 `old_sz`，调用方可能传 `old_sz=0`（表示 unknown）；自定义 allocator 需要能接受该情况。
- 目前注入点支持两层：
  - 进程全局默认（`tua_rt_configure`）：用于 CLI/默认行为；
  - 线程局部 current state（`tua_state_set_current`）：同一进程不同线程可并行跑不同配置；同线程可切换 current state（需要宿主自己保证不交错执行）。

## 需要暴露的释放函数（跨边界安全）

当 Tua 返回“宿主可持有”的指针（例如 `tua_dostring` 将来返回的错误信息、或 FFI 生成的临时缓冲），必须提供对应的释放入口：

```c
void tua_free(tua_state* s, void* p, size_t old_sz);
```

约束：

- `tua_free` 必须使用 `s` 的 allocator；宿主不得用 `free()` 释放该指针。

## Arena/Region 的兼容要求

即使引入 arena/region：

- arena 的“底层大块申请/扩容/释放”也必须走实例 allocator；
- arena 内部的小对象生命周期仍由 arena 控制，但不影响宿主统计/限额能力。

## 实施顺序建议

1. 在 runtime（`src/rt/rt_alloc.c`）引入 allocator 结构与默认实现；
2. 在编译器侧统一改用 `tua_alloc_*`，把 `malloc/free` 收敛掉；
3. 增加 `tuac` 的默认配置（使用系统 allocator），并在 tests/bench 中保持行为不变；
4. 增加一个小的嵌入示例（后续做，不影响当前阶段性能优化）。
