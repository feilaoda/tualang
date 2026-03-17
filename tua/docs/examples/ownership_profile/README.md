# ownership_profile 示例测试（草案）

这些 `.tua` 文件用于解释 `script/system` 双 profile 的目标语义。

说明：

- 这是设计样例，不是当前 CI 的正式测试集。
- 部分 case 描述的是“目标行为”，在当前编译器版本上可能尚未通过。
- 每个文件头部都有 `expect-target` 注释，表示该草案完成后的预期结果。

目录：

- `script/*`：偏业务脚本风格（默认共享句柄，边界显式 move）
- `system/*`：偏系统模块风格（严格所有权/借用）

