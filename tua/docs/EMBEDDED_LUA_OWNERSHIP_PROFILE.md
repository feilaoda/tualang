# Embedded Lua Ownership Profile（Tuzi 草案）

状态：`DRAFT`（目标语义；不代表当前实现已全部支持）
当前实现阶段：已支持 `--profile script|system`；默认仍为 `system`，`script` 先以显式开关启用。

当前已落地（第一批）：

- `script` 下关闭隐式 move：`let b = a`、`a = b`、`obj.field = b`、`m[k] = b`、`{k: b}`、`[b]`。
- `system` 下保持原有隐式 move 与 use-after-move 诊断。

## 1. 背景

Tua 要作为嵌入式 Lua/脚本语言使用时，核心诉求是：

- 业务脚本写起来接近 Lua（默认简单）
- 边界语义可控（FFI/异步/零拷贝）
- 保留系统层精细控制能力（性能/安全）

为此定义 Tuzi 风格所有权模型：**默认借用，逃逸才做 RC**。

## 2. Tuzi 核心原则

### 2.1 Borrow by Default（默认借用）

默认情况下，以下操作都按“借用视图/别名”处理，不转移所有权：

- `let b = a`
- `fn f(x: T)` 的调用 `f(a)`
- 普通局部传递与读取

目标：业务代码 90% 场景不需要写 `move`。

### 2.2 Escape-only RC（仅逃逸点 RC++）

只有对象“逃逸当前局部语境”时，编译器才插入 retain（RC++）：

1. `return a`
2. `global = a`
3. `obj.field = a`
4. `m[k] = a` / `arr.push(a)` 等容器持有
5. 闭包/异步回调保存到未来执行

对应释放由 drop/release 路径对称处理（RC--）。

补充（容器索引语义，Frozen 目标）：

- script 模式下保留脚本风格 API：普通读写继续用 `m[k]` / `m[k]=v` / `a[i]` / `a[i]=v`。
- 当 `v` 是非 Copy 值（如 `struct/map/array/bytes`）并执行 `m[k]=v` 或 `a[i]=v`：
  - 容器获得一份 owner（已是共享 owner 则 retain；否则装箱后持有）。
  - 源变量 `v` 不失效（不要求用户理解 move-only）。
- 覆盖/删除/clear/free 必须对称 release 旧值。

### 2.3 `move` 只用于显式所有权转移

`move` 保留，但收敛为“边界关键字”：

- `extern fn send(move data: bytes)`（FFI 接管）
- 系统层明确 owner 迁移
- 特殊零拷贝/生命周期控制点

## 3. 双 Profile 设计

### 3.1 `script` profile（默认）

- 按 Tuzi 模型执行：默认借用 + 仅逃逸点 RC。
- 用户只在边界写 `move`。
- 目标体验：接近 Lua（能跑、易写、少心智负担）。

### 3.2 `system` profile（严格）

- 保留现有严格路径：move-only + borrow 检查。
- 用于 std/runtime/性能热点。
- 允许开发者显式换取更可预测的低开销行为。

## 4. 规则对比（摘要）

| 操作 | script（Tuzi） | system |
|---|---|---|
| `let b = a`（容器/对象） | 默认借用别名（a/b 可继续用） | move（a 失效） |
| `m[k]=v` / `a[i]=v`（`v` 为 struct 等非 Copy） | 容器持有 owner（retain/装箱），`v` 仍可用 | move（`v` 失效） |
| `fn f(x: T)` 调用 | 默认借用传递 | 默认借用传递（更严格 borrow 约束） |
| `return a` | 触发逃逸 retain（或 move 语义等价） | 走严格 owner 规则 |
| `obj.field = a` | 逃逸 retain | 可保持 move 语义 |
| `move a` | 仅边界推荐使用 | 常用控制手段 |
| FFI 传参 | 默认 borrow，`move` 显式接管 | 同左 |

## 5. 边界约定（必须明确）

### 5.1 FFI

- 默认：borrow（callee 不得释放、不应缓存到未来）
- 接管：`move`

```tua
extern fn crc32(data: bytes) long
extern fn send_packet(move data: bytes) int
```

### 5.2 异步

- 保存到 future/回调上下文即“逃逸”，必须 retain。
- 完成/取消/错误路径必须对称 release。

### 5.3 零拷贝

- 统一用 `Slice<byte>` 表达借用视图。
- Slice 不拥有底层内存，禁止非法逃逸。

## 6. 当前实现与目标差距（简述）

当前实现仍以“系统层语义”为主，主要差距：

- 局部赋值对 move-only 值仍是 move 语义（非默认借用）
- 后端仍有 move-nulling 路径（赋值后源置空）
- map/array/bytes 还缺统一 RC 句柄模型
- `string`/closure env 有 RC，但容器值 RC 仍是局部覆盖

因此本文件描述的是**目标语义**，实现需分阶段推进。

## 7. 语义样例（节选）

完整样例目录：`docs/examples/ownership_profile/`

### S01 script 下局部赋值默认借用

```tua
// profile: script
let a: map = {"x": 1}
let b = a
b["x"] = 2
println(a["x"] ?? 0) // expect 2
```

### S02 script 下函数参数默认借用

```tua
// profile: script
fn touch(m: map) {
  m["k"] = 42
}

fn main() int {
  let m: map = {}
  touch(m)
  println(m["k"] ?? 0) // expect 42
  return 0
}
```

### S03 FFI 显式 move 边界

```tua
extern fn send_packet(move data: bytes) int

fn main() int {
  let p = bytes(128)
  let rc = send_packet(move p)
  // p 之后不可再用
  return rc
}
```

### S04 逃逸点：字段/容器写入

```tua
struct Packet {
  body: bytes
}

fn main() int {
  let p = Packet{ body: bytes(32) }
  let b = bytes(16)
  p.body = b      // script: 逃逸 retain
  let m: map = {}
  m["buf"] = b   // script: 逃逸 retain
  return 0
}
```

### S05 system 严格 move 保留

```tua
// profile: system
fn consume(move b: bytes) int {
  return b.len()
}

fn main() int {
  let x = bytes(12)
  let n = consume(move x)
  return n
}
```

## 8. 最小验收标准

- script 下：常见业务代码不需要显式 `move`。
- script 下：仅在 escape 点出现 RC 插桩。
- system 下：现有严格 move/borrow 行为保持兼容。
- FFI/async/Slice 三条边界规则保持一致且可测试。
