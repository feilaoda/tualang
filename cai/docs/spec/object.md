# `object`（命名空间）规范

Status: Frozen

本文件定义 `object`：用于组织“静态函数集合/命名空间”。

---

## 1. 声明
语法：
```cai
object Math {
  fn add(a: int, b: int) -> int { return a + b }
}
```

---

## 2. 语义
- `object` 不可实例化。
- `object.Name` 作为限定名用于访问其内部声明。
- `private` 规则与模块可见性一致（见 `cai/docs/spec/visibility.md`）。
