# Embedded Lua Ownership Migration Plan（Tuzi）

状态：`DRAFT`

关联文档：`docs/EMBEDDED_LUA_OWNERSHIP_PROFILE.md`

## 1. 迁移目标

把当前“以严格 move 为主”的实现，迁移到 Tuzi 模型：

- script：**默认借用 + 仅逃逸点 RC**
- system：保留严格所有权/借用路径
- FFI：默认 borrow，`move` 显式接管

## 2. 分阶段实施

进度注记（2026-02）：

- 已完成 profile 开关与第一批语义切换（隐式 move 在 `script` 下关闭，`system` 保持原行为）。
- 逃逸点 RC 插桩仍在后续阶段（P1/P2）。

## P0：语义冻结与开关接入

交付：

- CLI 增加 profile 开关：`--profile script|system`
- 迁移期默认维持 `system`（降低兼容风险），阶段完成后再切换默认到 `script`
- 诊断文本明确区分 script/system 语义

验收：

- 能通过 profile 切换同一段代码的所有权行为

---

## P1：运行时 RC 基础补齐（容器句柄）

交付：

- `map/array/bytes` 增加统一 retain/release 能力
- drop 路径改为 RC 对称释放，不依赖“强制 borrowed”兜底

验收样例：

```tua
fn main() int {
  let a: map = {"x": 1}
  let b = a
  b["x"] = 2
  println(a["x"] ?? 0) // expect 2
  return 0
}
```

---

## P2：编译器 escape 分析插桩

交付：

- 明确 escape 点并插入 retain：
  - `return`
  - 字段写入
  - 容器写入
  - 全局赋值
  - 异步/闭包持有
- 非 escape 局部传递不做 retain（保持零额外开销目标）

验收样例：

```tua
struct Packet { body: bytes }

fn save(p: Packet, b: bytes) {
  p.body = b // escape retain
}
```

---

## P3：script 语义切换（默认借用）

交付：

- script 下关闭隐式 move 赋值（`let b = a`/`a = b`）
- script 下保持 `move` 可用（边界转移）
- system 下保持当前严格语义

验收样例：

```tua
// profile: script
fn main() int {
  let a = bytes(8)
  let b = a
  println(a.len(), b.len())
  return 0
}
```

```tua
// profile: system
fn main() int {
  let a = bytes(8)
  let b = a
  println(a.len()) // expect compile error (use-after-move)
  return b.len()
}
```

---

## P4：边界规则收敛（FFI/async/zero-copy）

交付：

- FFI：borrow 与 move 的诊断统一
- async：保存与释放路径对称
- Slice：逃逸违规保持编译期报错

验收样例：

```tua
extern fn hash(data: bytes, out: &string) int
extern fn send(move data: bytes) int
```

---

## P5：性能收敛与 profile 策略

交付：

- script：减少不必要 retain/release（仅 escape）
- system：继续作为“低开销、强约束”路径
- 增加基准：script/system/LuaJIT 对比

验收：

- script 在常见业务脚本负载下性能可接受
- system 在热点路径不因 script 语义退化

## 3. 测试组织建议

建议新增：

- `tests/profile/script/*.tua`
- `tests/profile/system/*.tua`
- `tests/profile/ffi/*.tua`
- `tests/profile/async/*.tua`

文档样例先保留在：

- `docs/examples/ownership_profile/`

## 4. 最小回归清单（建议）

- script：局部赋值别名、函数传参、字段写入、容器写入
- system：move 后使用报错、显式 move 传参、return owner
- ffi：borrow 参数可复用、move 参数后不可用
- async：回调重复注册/取消/错误路径 release 对称
- slice：正常借用、逃逸报错、owner 提前失效阻止

## 5. 风险与回滚

- 风险：script RC 插桩过多导致性能下降
  - 策略：严格把 RC 限定在 escape 点，并做基准回归
- 风险：双 profile 增加理解成本
  - 策略：默认 script；system 明确用于底层模块
- 风险：FFI 边界泄漏/UAF
  - 策略：签名约束 + 专项测试 + sanitizer 回归
