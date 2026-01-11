## Tua Packages Roadmap（Deferred）

本文件用于承载 **包模块（packages）相关** 的规划与 TODO：JSON/Tokenizer/LLM 等。

注意：这些任务不应阻塞语言核心/运行时/标准库的演进；核心路线请看 `docs/ROADMAP.md`。

### A. JSON（packages/json）
- [ ] 恢复 `packages/json` Tua 封装（当前仓库可能仅保留 `packages/clib/json`）
- [ ] `Json.parse/parseBytes`：最小 DOM（map/array/scalar）
- [ ] scan API：顶层标量与 key-path
- [ ] 与 `packages/clib/json` 的快路径对齐（优先走 C 扫描/跳过）

### B. Tokenizer（可能位于 packages/tokenizer 或 packages/llm/tokenizer）
- [ ] BPE：加载真实模型 tokenizer.json 并与参考实现对齐
- [ ] pre_tokenizer regex：Unicode 类别完整对齐（可选 ICU/C 表）
- [ ] decoder/byte-fallback：对齐 HF ByteLevel 行为
- [ ] 性能：减少分配/复制、热点下沉到 C、可复用 scratch

### C. LLM（packages/llm 或外部 repo）
原则：不引入编译器特判；一切通过 `extern fn` + 通用链接/加载能力完成。
- [ ] 模型格式：GGUF v1（tensor infos + data layout）
- [ ] SafeTensors：完善视图/转换/缓存策略
- [ ] 数学内核：BLAS/Accelerate 接入与可替换后端接口
- [ ] KV cache：抽象接口 + 至少一种实现
- [ ] Sampling：softmax + temperature + top-k/top-p + 可复现 RNG
- [ ] Runner：最小 CLI / 嵌入式 API 示例

