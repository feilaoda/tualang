# 包与构建清单（`cai.toml` / `cai.lock`）规范

Status: Frozen

本文件定义 CAI 的“包（package）”概念、构建清单 `cai.toml` 与锁文件 `cai.lock` 的最低要求，用于：
- 可复现构建（同源码 + 同锁文件 + 同工具链版本 -> 同依赖解析结果）；
- 依赖管理（本地依赖与可扩展的远程依赖描述）；
- 与模块导入解析联动（见 `cai/docs/spec/import.md`）。

本文件不冻结具体 CLI 子命令名称，但默认以 `caic build` 作为示意。

---

## 1. 包（package）定义

### 1.1 包根目录
包含 `cai.toml` 的目录称为包根目录（package root）。

### 1.2 默认目录布局（冻结）
包根目录的默认布局冻结为：
- `cai.toml`：构建清单
- `cai.lock`：依赖锁文件（自动生成/更新）
- `src/`：源码根
  - `src/main.ai`：默认可执行入口模块（当构建 `exe` 时）
  - `src/lib.ai`：默认库入口模块（当构建 `staticlib/dylib` 时）

工具链允许通过 `cai.toml` 覆盖入口模块（见 3）。

---

## 2. `cai.toml`（构建清单）

### 2.1 文件格式
`cai.toml` 必须为 UTF-8 编码 TOML 文件。

### 2.2 `[package]`（必需）
字段冻结：
- `name: string`（必需）
  - 允许字符集合：`[A-Za-z0-9_-]`；且必须以字母开头。
- `version: string`（必需）
  - 采用语义化版本（SemVer）：`MAJOR.MINOR.PATCH`

包 ID（规范性定义）：
- `PackageId = name + "@" + version`

### 2.3 `[dependencies]`（可选）
`[dependencies]` 是一个从依赖名到依赖描述的映射。

依赖名规则：
- 同 `[package].name` 的字符限制。

依赖描述允许两种形态（冻结）：

1) 版本要求字符串（用于远程/注册表依赖；解析规则见 4）：
```toml
[dependencies]
foo = "1.2.3"
```

2) 路径依赖（用于本地单仓/mono-repo 开发；路径相对包根目录）：
```toml
[dependencies]
foo = { path = "../foo" }
```

冻结约束：
- `path` 依赖目标目录必须包含 `cai.toml`，否则为构建错误。
- 不允许同名依赖重复声明。

保留字段（不冻结具体语义，但保留键名以便将来扩展）：
- `git`、`rev`、`branch`、`tag`、`registry`、`features`
工具链若遇到这些字段但尚未实现其语义，必须报错（不得静默忽略）。

### 2.4 `[build]`（可选）
冻结字段：
- `targets = ["exe" | "staticlib" | "dylib", ...]`

未声明时的默认值冻结为：
- `targets = ["exe"]`

### 2.5 `[entry]`（可选）
用于覆盖默认入口模块。

冻结字段：
- `exe: string`（入口模块相对路径，例如 `"src/main.ai"`）
- `staticlib: string`
- `dylib: string`

未声明时，使用 1.2 的默认入口。

---

## 3. 构建与导入路径联动（冻结）

工具链在构建一个包时，必须构建“包搜索根目录列表”，并把它们传递给导入解析（等价于 `caic --package-dir ...`，见 `cai/docs/spec/import.md`）：

包搜索根目录列表冻结为（按顺序）：
1) 当前包的 `src/` 目录；
2) 每个依赖包的 `src/` 目录（顺序按 `cai.lock` 中的锁定顺序；见 4）。

因此：
- `import "foo/bar.ai"` 会在上述根目录列表中按顺序查找（见 `cai/docs/spec/import.md` 的包路径解析）。
- 工具链不得依赖系统绝对路径导入（见 `cai/docs/spec/import.md`）。

---

## 4. `cai.lock`（锁文件）

### 4.1 目的与生成
`cai.lock` 用于锁定依赖解析结果，保证可复现构建。

冻结规则：
- 若包存在任何 `[dependencies]`，工具链必须生成或更新 `cai.lock`；
- `cai.lock` 必须被纳入版本控制（推荐实践，但不强制）。

### 4.2 文件格式（冻结）
`cai.lock` 必须为 UTF-8 编码 TOML 文件。

### 4.3 锁定条目（冻结字段）
锁文件中每个依赖条目必须包含：
- `name: string`
- `version: string`（解析后的具体版本）
- `source: string`（例如 `path:../foo` 或 `registry:default`）
- `checksum: string`（可选；当 source 为远程时必须存在；当为 path 时可省略）

依赖顺序冻结为：
- `cai.lock` 的依赖列表顺序必须稳定（同输入产生同输出），并用于 3 中的包搜索根目录顺序。

---

## 5. 版本解析（冻结的最小集合）

为降低初期实现复杂度，版本要求字符串的解析规则冻结为：
- 仅支持精确版本：`"MAJOR.MINOR.PATCH"`
- 其他形式（例如 `^1.2`、`>=1.2,<2.0`）为构建错误

> 版本范围、特性开关等属于后续版本升级内容；冻结“只支持精确版本”可以确保早期工具链行为确定。
