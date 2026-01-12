# `Slice<T>` 规范

Status: Frozen

本文件定义 `Slice<T>`：对一段连续内存的“视图类型”，用于零拷贝参数传递、FFI 与 `bytes/string` 的桥接。

语法层不需要新增：`Slice<T>` 按命名泛型类型解析。

---

## 1. 表示与值域
`Slice<T>` 是一个二元结构概念（运行时布局）：
- `data: Ptr<T>`（或 `Ptr<byte>` 等）
- `len: usize`

`len` 表示元素个数（不是字节数；字节数为 `len * sizeof(T)`）。

`Slice<T>` 可以为空（`len == 0`），此时 `data` 允许为 `null`。

---

## 2. 权限（只读/可写）
CAI 不通过两个类型区分只读/可写切片；只有一个 `Slice<T>` 类型。

`Slice<T>` 的读写权限由编译器静态跟踪，为两种“借用权限”：
- **共享只读**（shared, read-only）
- **独占可写**（exclusive, read-write）

该权限不是类型参数的一部分，但属于类型检查的一部分（类似 `Ref<T>` 的共享/独占借用语义，见 `cai/docs/spec/ref.md`）。

### 2.1 权限的产生（最低要求）
下列 API 的权限规则冻结：
- `bytes.slice() -> Slice<byte>`：产生共享只读 slice
- `bytes.mutSlice() -> Slice<byte>`：产生独占可写 slice

当 slice 被传参时，形参的参数模式表达“最低权限要求”：
- `fn f(const s: Slice<T>)`：`s` 在函数体内视为共享只读；允许传入共享或独占 slice
- `fn f(mut s: Slice<T>)`：`s` 在函数体内视为独占可写；仅允许传入独占可写 slice（传入共享 slice 为编译错误）

---

## 3. 边界与索引
不提供 `slice[i]` 语法糖（避免与数组混淆），索引访问通过标准库 API：
- `slice.get(i) -> Option<T>`（只读；仅当 `T` 为 copy type 时允许）
- `slice.getRef(i) -> Option<Ref<T>>`（共享只读引用）
- `slice.getMut(i) -> Option<Ref<T>>`（独占可写引用；仅当 `slice` 自身为“独占可写 slice”时允许调用，否则为编译错误）

---

## 4. 生命周期与借用
`Slice<T>` 是借用视图：
- 创建 slice 必须能静态保证其 owner 在 slice 存活期间不被 move/drop/可能失效的写影响。
- 具体借用规则由 `cai/docs/spec/ownership.md` 冻结；本文件冻结 slice 作为“借用视图”的定位。

补充冻结：
- “借用 slice”（例如由 `bytes.slice()`/`bytes.mutSlice()` 创建）默认**不可逃逸**：禁止存入 struct 字段/全局变量/闭包环境、禁止跨 `await` 存活。
- `embed` 创建的 `Slice<byte>` 具有 `static` 生命周期，允许逃逸（见 `cai/docs/spec/embed.md`）。
