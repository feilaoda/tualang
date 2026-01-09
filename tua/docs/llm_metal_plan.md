# Metal backend plan (record)

Goal: add a selectable Metal compute backend for LLM inference (eventually replacing CPU GEMV/GEMM hot spots and supporting GGUF quantized types).

## Phases

1) **Backend interface**
   - Define a small stable kernel surface (matmul/matvec, rmsnorm, rope, attention score/value ops, softmax, sampling helpers).
   - Provide a runtime backend selector (CPU vs Metal), ideally via a single module boundary.

2) **Weights + memory layout**
   - Make weight storage backend-agnostic: host-side layout + (optional) device-side packed layout.
   - For GGUF quant types (Q4_K_M / Q6_K etc), define explicit decode/pack formats at the boundary.

3) **Metal kernels**
   - Start with the biggest bottlenecks: MLP matvec (gate/up/down) and lm_head projection.
   - Add f16/bf16 paths first; then quantized dot paths if needed.
   - Add multi-threaded CPU fallback for unsupported ops.

4) **Scheduling + overlap**
   - Pipeline: prefill vs decode, async command buffers, overlap upload with compute where possible.
   - KV cache: device-friendly layout (by head, contiguous) to reduce gathers.

5) **Validation + perf regression**
   - Add correctness tests (small tensors, known outputs) for each kernel.
   - Add perf smoke tests with fixed prompt and token count; track tok/s.

## Notes
- Metal should be a swappable backend; avoid leaking MTL types into high-level model code.
- Keep quantization code readable and isolated (ideally in `packages/llm/quant/*` or similar) so CPU/Metal share one reference implementation.

