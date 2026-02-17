## Tua Packages Roadmap（Deferred）

本文件用于承载 **包模块（packages）相关** 的规划与 TODO（当前聚焦 JSON）。

注意：这些任务不应阻塞语言核心/运行时/标准库的演进；核心路线请看 `docs/ROADMAP.md`。

### A. JSON（packages/json）
- [ ] 恢复 `packages/json` Tua 封装（当前仓库可能仅保留 `packages/clib/json`）
- [ ] `Json.parse/parseBytes`：最小 DOM（map/array/scalar）
- [ ] scan API：顶层标量与 key-path
- [ ] 与 `packages/clib/json` 的快路径对齐（优先走 C 扫描/跳过）
