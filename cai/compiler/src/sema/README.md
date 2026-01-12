# Sema（语义分析）目录

本目录用于存放 CAI 编译器的语义分析实现：
- 符号解析（imports/名称绑定/重名规则）
- 类型检查
- `async/await/yield` 语义约束
- 错误模型（`Res,int` 与 guard `? {}`）检查
- 借用/所有权检查（基于规范冻结点）

Status: Draft

