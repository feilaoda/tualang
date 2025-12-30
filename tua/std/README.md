# std 标准库（草案）

当前标准库以源码形式随 `tuac` 一起发布，模块通过 `import` 引用。

## 导入规则

- `import "std/xxx"` / `from "std/xxx" import ...`：从标准库目录解析。
- 标准库目录查找顺序：
  - 环境变量 `TUA_STDLIB_DIR`
  - `tuac` 可执行文件所在目录的 `../std`

## 模块列表

- `std/strconv`：字符串与数字转换（`Int.parse`）
- `std/rt`：运行时绑定（loop/workqueue/deadline/free）
- `std/net`：TCP 异步网络（基于 loop + callback）
- `std/fs/async`：文件系统异步 API（基于 workqueue offload）
