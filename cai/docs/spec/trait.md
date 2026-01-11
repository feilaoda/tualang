# `trait` 规范

Status: Frozen

本文件定义 CAI 的 `trait`：方法签名、约束（bounds）、静态分发与动态 trait object。

---

## 1. 声明
语法见 `cai/parser.g4`：

```cai
trait HasId {
  fn id() -> int
}
```

trait 体内只允许方法签名（无实现）。

---

## 2. 作为约束（bounds）
在泛型中使用：
- `fn f<T: HasId>(x: T) -> int { ... }`

规则：
- bounds 只作用于方法解析与实例化检查；
- 若类型 `T` 未实现所需 trait，编译错误并给出缺失的 impl 信息。

---

## 3. 静态分发（monomorphization）
当 `T: Trait` 且参数类型为 `T`（非 trait object）时：
- 调用 `x.id()` 静态分发到 `impl` 的具体实现；
- 通过泛型单态化生成实例（见 `cai/docs/spec/fn.md`）。

---

## 4. trait 作为类型
CAI 不支持动态 trait object：
- trait 名称不能作为值类型使用（例如 `fn g(x: HasId)` 为编译错误）。
- trait 仅用于泛型约束与静态分发。

---

## 5. `async` trait method
grammar 允许 `async fn` 出现在 trait 方法签名中。

规则：
- `async fn` 在 trait 中等价于返回某个可等待类型（例如 `Task<T>`），并要求实现端返回相同形态；
