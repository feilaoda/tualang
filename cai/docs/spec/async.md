# `async/await` 并发语义规范

Status: Frozen

本文件定义 CAI 的异步函数、await、任务对象与取消/调度的最小语义。

---

## 1. `async fn`
- `async fn f(...) -> T { ... }` 表示一个异步函数。
- 异步函数调用 `f(...)` 返回一个“可等待对象”（记为 `Task<...>`），而不是立即执行到完成。

任务类型属于标准库，但语言冻结 `await` 的语义边界（见 2），调度模型见 `cai/docs/spec/runtime.md`。

---

## 2. `await`
- `await expr`：
  - `expr` 必须为可等待对象类型；
  - `await` 会暂停当前 async 上下文，直到完成并产生结果。

约束：
- `await` 只能出现在 `async fn` 或 `async lambda` 中；否则编译错误。

调度保证（冻结）：
- `await` 是协作式挂起点：当前任务在此让出执行权，运行时可调度其他任务运行（见 `cai/docs/spec/runtime.md`）。

多返回约定：
- 若 `expr` 的完成结果为多返回（例如由 `async fn` 返回 `T, int`），则 `await expr` 也产生对应的多返回值。

---

## 3. 取消
取消模型冻结为：
- 任务支持协作式取消；
- 取消后的 `await` 必须返回 `std.error.Code.CANCELLED`（见 `cai/docs/spec/std_error.md`），并遵循 `cai/docs/spec/error.md` 的失败返回约定。

---

## 4. `defer` 与 async
- `defer` 绑定到“当前作用域”，与同步函数一致：
  - 当作用域因 `return` 或运行时错误退出而结束时执行；
  - 当 async 函数被取消并退出时同样执行。

---

## 5. 资源与借用限制
为避免跨 await 悬垂引用，规则冻结为：
- 不允许持有 `Ref<T>` 跨越 `await`（编译错误）。
- `await` 点之前必须结束所有借用；编译器可用 NLL 缩短借用生命周期以通过该检查。
