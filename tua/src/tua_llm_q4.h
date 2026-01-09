#ifndef TUA_LLM_Q4_H
#define TUA_LLM_Q4_H

#include "rt/rt_err.h"
#include "tua_bytes.h"

#include <stdint.h>

// Q4_0: per-32-value block, symmetric signed int4 with per-block FP16 scale.
//
// Each block stores:
// - d: float16 scale (max_abs / 7)
// - qs[16]: 32 values packed as 2 int4 per byte, stored as unsigned nibble with bias +8.
//
// Dequant: v = (q - 8) * d
// where q is nibble in [0..15].
//
// Storage is row-major blocks: [m rows][n/32 blocks per row].

// Returns the number of bytes for a single row (n values).
// Requirements: n > 0, n % 32 == 0.
int64_t tua_llm_q4_0_row_bytes(int32_t n);

// Returns the number of bytes for a contiguous [m,n] matrix.
// Requirements: m,n > 0, n % 32 == 0.
int64_t tua_llm_q4_0_mat_bytes(int32_t m, int32_t n);

// Pack a BF16 row-major matrix [m,n] into Q4_0 format.
// - src is BF16 elements (2 bytes each), contiguous row-major.
// - dst must have at least tua_llm_q4_0_mat_bytes(m,n) bytes.
tua_err_t tua_llm_q4_0_pack_bf16(tua_bytes* dst, int64_t dst_off,
                                tua_bytes* src, int64_t src_off,
                                int32_t m, int32_t n);

// Dequantize a single row from a Q4_0 matrix into float32.
// - out is float32[n]
// - row is the row index in [0..m)
tua_err_t tua_llm_q4_0_get_row_f32(tua_bytes* out, int64_t out_off,
                                  tua_bytes* a, int64_t a_off,
                                  int64_t row, int32_t n);

// GEMV with Q4_0 weights and F32 activations:
// y[m] = A[m,n] (q4_0) * x[n] (f32), output y is f32.
tua_err_t tua_llm_gemv_q4_0_f32(tua_bytes* y, int64_t y_off,
                               tua_bytes* a, int64_t a_off,
                               tua_bytes* x, int64_t x_off,
                               int32_t m, int32_t n);

// Internal self-test (returns 0 on success). Intended for regression tests.
int32_t tua_llm_q4_0_selftest(void);

// =========================
// Q4_K: per-256-value block, asymmetric uint4 with per-32 subgroup (scale,min).
//
// Layout matches llama.cpp/ggml's "K-quant" style (but we quantize from BF16 at load time):
// - qs: 256 values packed as 4-bit unsigned (0..15)
// - 8 subgroups of 32 values each:
//   - scale[g] stored as 6-bit int, dequant scale = d * scale[g]
//   - min[g] stored as signed 6-bit int, dequant min = dmin * min[g]
// - d and dmin are FP16 base factors shared by the block
//
// Dequant: v = min[g] + q * scale[g]
//
// Requirements for all Q4_K APIs: n > 0, n % 256 == 0.

int64_t tua_llm_q4_k_row_bytes(int32_t n);
int64_t tua_llm_q4_k_mat_bytes(int32_t m, int32_t n);

tua_err_t tua_llm_q4_k_pack_bf16(tua_bytes* dst, int64_t dst_off,
                                tua_bytes* src, int64_t src_off,
                                int32_t m, int32_t n);

tua_err_t tua_llm_q4_k_get_row_f32(tua_bytes* out, int64_t out_off,
                                  tua_bytes* a, int64_t a_off,
                                  int64_t row, int32_t n);

tua_err_t tua_llm_gemv_q4_k_f32(tua_bytes* y, int64_t y_off,
                               tua_bytes* a, int64_t a_off,
                               tua_bytes* x, int64_t x_off,
                               int32_t m, int32_t n);

int32_t tua_llm_q4_k_selftest(void);

#endif
