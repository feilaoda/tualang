# 资源嵌入（`embed`）规范

Status: Frozen

本文件定义 `embed`：将外部资源（二进制/模型权重等）在编译期嵌入 AOT 产物，并以零拷贝方式在运行时访问。

---

## 1. 语法
`embed` 是一个表达式关键字：
- `embed "relative/or/package/path.bin"`

其中路径为字符串字面量（必须是编译期常量）。

---

## 2. 路径解析
`embed` 的路径解析冻结为：
- 若路径以 `./` 或 `../` 开头：相对当前源文件所在目录解析；
- 否则：按“包路径”解析规则查找（与 `import` 的包路径解析一致，见 `cai/docs/spec/import.md`）。

若目标文件不存在，为编译错误。

---

## 3. 类型与只读语义
`embed "path"` 的结果类型冻结为：
- `Slice<byte>`

并且该 `Slice<byte>` 的权限冻结为：**共享只读**。

因此：
- 可以传给 `const s: Slice<byte>` 参数；
- 不能满足 `mut s: Slice<byte>`（独占可写）参数要求。

---

## 4. 生命周期（`static`）
`embed` 产生的 slice 具有 `static` 生命周期：
- 允许存入 `struct` 字段、全局变量、闭包环境；
- 允许跨 `await` 存活；
- 允许在任意作用域返回（因为不指向局部栈内存）。

该规则仅对 `embed` 产生的 slice 成立；一般“借用 slice”（例如 `bytes.slice()`/`bytes.mutSlice()`）仍受借用逃逸限制（见 `cai/docs/spec/ownership.md` 与 `cai/docs/spec/slice.md`）。

---

## 5. AOT 产物要求
对使用 `embed` 的程序/库：
- 嵌入资源必须被包含进 `exe/staticlib/dylib` 产物中（见 `cai/docs/spec/aot.md`）。
- 运行时访问 `embed` 资源不得依赖外部文件 I/O。

---

## 6. 内存映射与只读张量
工具链/运行时应当把嵌入资源放入只读段，并以零拷贝方式提供 `data/len`：
- `data` 指向只读内存；
- `len` 为字节长度。

标准库可在此基础上提供“只读张量/权重”视图（例如把一段 `Slice<byte>` 映射为只读 tensor），但该上层 API 不在本文件冻结范围内。
