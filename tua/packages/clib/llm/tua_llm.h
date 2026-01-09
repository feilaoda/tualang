#ifndef TUA_LLM_H
#define TUA_LLM_H

#include "rt/rt_err.h"
#include "tua_array.h"
#include "tua_bytes.h"
#include "tua_map.h"

#include <stdint.h>

// Elementwise kernels on contiguous float32 buffers stored in `bytes`.
// All offsets are in bytes; counts are in elements (float32).

// Configure maximum thread usage for underlying math kernels (best-effort).
// - `n<=0` means "runtime default".
tua_err_t tua_llm_set_threads(int32_t n);
// Returns the current configured thread count (0 means "runtime default").
int32_t tua_llm_get_threads(void);

// GEMV with BF16 weights and F32 activations:
// y[m] = A[m,n] (bf16) * x[n] (f32), output y is f32.
tua_err_t tua_llm_gemv_bf16_f32(tua_bytes* y, int64_t y_off,
                               tua_bytes* a, int64_t a_off,
                               tua_bytes* x, int64_t x_off,
                               int32_t m, int32_t n);

// GEMV with FP16 weights and F32 activations:
// y[m] = A[m,n] (fp16) * x[n] (f32), output y is f32.
tua_err_t tua_llm_gemv_f16_f32(tua_bytes* y, int64_t y_off,
                              tua_bytes* a, int64_t a_off,
                              tua_bytes* x, int64_t x_off,
                              int32_t m, int32_t n);

// RMSNorm with BF16 weights and F32 activations.
tua_err_t tua_llm_rmsnorm_f32_bf16w(tua_bytes* out, int64_t out_off, tua_bytes* x, int64_t x_off,
                                   tua_bytes* w_bf16, int64_t w_off, int32_t n, float eps);

// RMSNorm with FP16 weights and F32 activations.
tua_err_t tua_llm_rmsnorm_f32_f16w(tua_bytes* out, int64_t out_off, tua_bytes* x, int64_t x_off,
                                  tua_bytes* w_f16, int64_t w_off, int32_t n, float eps);

// Per-head RMSNorm for Q/K with BF16 weight (length head_dim).
tua_err_t tua_llm_qk_rmsnorm_inplace_f32_bf16w(tua_bytes* x, int64_t x_off, int32_t n_heads, int32_t head_dim,
                                              tua_bytes* w_bf16, int64_t w_off, float eps);

// Per-head RMSNorm for Q/K with FP16 weight (length head_dim).
tua_err_t tua_llm_qk_rmsnorm_inplace_f32_f16w(tua_bytes* x, int64_t x_off, int32_t n_heads, int32_t head_dim,
                                             tua_bytes* w_f16, int64_t w_off, float eps);

tua_err_t tua_llm_add_inplace_f32(tua_bytes* dst, int64_t dst_off, tua_bytes* src, int64_t src_off, int64_t n);
tua_err_t tua_llm_rmsnorm_f32(tua_bytes* out, int64_t out_off, tua_bytes* x, int64_t x_off,
                              tua_bytes* w, int64_t w_off, int32_t n, float eps);
tua_err_t tua_llm_qk_rmsnorm_inplace_f32(tua_bytes* x, int64_t x_off, int32_t n_heads, int32_t head_dim,
                                        tua_bytes* w, int64_t w_off, float eps);
tua_err_t tua_llm_silu_mul_f32(tua_bytes* out, int64_t out_off, tua_bytes* gate, int64_t gate_off,
                              tua_bytes* up, int64_t up_off, int64_t n);
tua_err_t tua_llm_silu_mul2_f32(tua_bytes* out, int64_t out_off,
                               tua_bytes* gate_up, int64_t gate_off, int64_t up_off,
                               int64_t n);
tua_err_t tua_llm_rope_inplace_f32(tua_bytes* x, int64_t x_off, int32_t n_heads, int32_t head_dim,
                                  int32_t pos, float theta);

// Matrix-vector multiply for float32 buffers stored in `bytes`.
// A is row-major [m,n] contiguous; x is [n]; y is [m].
// Offsets are in bytes.
tua_err_t tua_llm_gemv_f32(tua_bytes* y, int64_t y_off,
                          tua_bytes* a, int64_t a_off,
                          tua_bytes* x, int64_t x_off,
                          int32_t m, int32_t n);

// Q4_0 helpers and GEMV:
tua_err_t tua_llm_q4_0_pack_bf16(tua_bytes* dst, int64_t dst_off,
                                tua_bytes* src, int64_t src_off,
                                int32_t m, int32_t n);
tua_err_t tua_llm_q4_0_get_row_f32(tua_bytes* out, int64_t out_off,
                                  tua_bytes* a, int64_t a_off,
                                  int64_t row, int32_t n);
tua_err_t tua_llm_gemv_q4_0_f32(tua_bytes* y, int64_t y_off,
                               tua_bytes* a, int64_t a_off,
                               tua_bytes* x, int64_t x_off,
                               int32_t m, int32_t n);
int32_t tua_llm_q4_0_selftest(void);

// Build BPE merge pair maps from a `string[]` of merge lines (`"A B"`) and the vocab map.
// Returns 0 on success and writes (rankMap, mergeIdMap, count).
int32_t tua_llm_bpe_merges_lines_pair_maps(tua_array* merges, tua_map* vocab,
                                          tua_map** out_rank, tua_map** out_merge_id, int64_t* out_count);

// Apply GPT-style repetition penalty in-place to logits:
// for each unique token id in the last `last_n` ids, adjust:
//   if logit > 0: logit /= penalty
//   else:         logit *= penalty
// `penalty <= 1` disables (no-op).
tua_err_t tua_llm_repetition_penalty_f32(tua_bytes* logits, int64_t logits_off, int32_t n,
                                        tua_array* ids, int32_t last_n, float penalty);

// Apply no-repeat n-gram blocking by setting logits for disallowed next tokens to -INFINITY.
// If `ngram <= 1`, does nothing.
tua_err_t tua_llm_no_repeat_ngram_f32(tua_bytes* logits, int64_t logits_off, int32_t n,
                                     tua_array* ids, int32_t ngram);

// Sets a single logit to -INFINITY (best-effort; ignores out-of-range ids).
tua_err_t tua_llm_logit_ban_id_f32(tua_bytes* logits, int64_t logits_off, int32_t n, int64_t id);

// Attention for a single token (decode step). KV must be float32 and stored in a contiguous layout:
// kv[K] layout: k_base + layer*layer_bytes + t*token_bytes + kv_head*head_dim*4
// kv[V] layout: v_base + layer*layer_bytes + t*token_bytes + kv_head*head_dim*4
// q is [n_heads * head_dim] float32.
// out is [n_heads * head_dim] float32.
// scratch must hold at least (pos+1) float32 values.
tua_err_t tua_llm_attn_decode_f32(tua_bytes* out, int64_t out_off,
                                 tua_bytes* q, int64_t q_off,
                                 tua_bytes* kv, int64_t k_base, int64_t v_base,
                                 int64_t layer_bytes, int64_t token_bytes,
                                 int32_t layer, int32_t pos,
                                 int32_t n_heads, int32_t n_kv_heads, int32_t head_dim,
                                 float scale,
                                 tua_bytes* scratch, int64_t scratch_off, int64_t scratch_floats);

// Attention decode with BF16 KV cache:
// - q/out are float32, KV is BF16.
tua_err_t tua_llm_attn_decode_kv_bf16_f32(tua_bytes* out, int64_t out_off,
                                         tua_bytes* q, int64_t q_off,
                                         tua_bytes* kv, int64_t k_base, int64_t v_base,
                                         int64_t layer_bytes, int64_t token_bytes,
                                         int32_t layer, int32_t pos,
                                         int32_t n_heads, int32_t n_kv_heads, int32_t head_dim,
                                         float scale,
                                         tua_bytes* scratch, int64_t scratch_off, int64_t scratch_floats);

// Attention decode with FP16 KV cache:
// - q/out are float32, KV is float16.
tua_err_t tua_llm_attn_decode_kv_f16_f32(tua_bytes* out, int64_t out_off,
                                        tua_bytes* q, int64_t q_off,
                                        tua_bytes* kv, int64_t k_base, int64_t v_base,
                                        int64_t layer_bytes, int64_t token_bytes,
                                        int32_t layer, int32_t pos,
                                        int32_t n_heads, int32_t n_kv_heads, int32_t head_dim,
                                        float scale,
                                        tua_bytes* scratch, int64_t scratch_off, int64_t scratch_floats);

tua_err_t tua_llm_argmax_f32(tua_bytes* x, int64_t x_off, int32_t n, int64_t* out_index);

// Sampling helpers (for text generation).
// - `top_k <= 0` means "no top-k filter" (use full vocab or top-p only).
// - `top_p <= 0` or `top_p > 1` means "no top-p filter".
// - `temperature <= 0` means greedy argmax.
// RNG state is mutated in-place; pass a non-zero seed for determinism.
tua_err_t tua_llm_sample_topk_topp_f32(tua_bytes* logits, int64_t logits_off, int32_t n,
                                      int32_t top_k, float top_p, float temperature,
                                      int64_t* rng_state, int64_t* out_index);
tua_err_t tua_llm_sample_topk_topp_f32_arr(tua_array* logits_f32,
                                          int32_t top_k, float top_p, float temperature,
                                          int64_t* rng_state, int64_t* out_index);

// BPE merge for tokenizer: merges a byte-level id sequence using `pairRank` and `pairMergeId`
// maps generated from tokenizer.json merges. Returns a new `long[]` and writes outErr=0 on success.
tua_array* tua_llm_bpe_merge_ids(tua_array* ids, tua_map* pairRank, tua_map* pairMergeId, int32_t* outErr);
// Convert a byte slice to the initial byte-level BPE ids using `byteToId[0..255]`.
// Returns `long[]` of length `len` and writes outErr=0 on success.
tua_array* tua_llm_bpe_bytes_to_ids(tua_bytes* b, int64_t off, int64_t len, tua_array* byteToId, int32_t* outErr);
// Decode BPE token ids back to UTF-8 bytes using the GPT-2 byte-level inverse mapping.
// `idToToken` maps id -> token string (tokenizer.json vocab+added); returns a new `bytes` on success.
tua_bytes* tua_llm_bpe_decode_ids_to_bytes(tua_array* ids, tua_map* idToToken, int32_t* outErr);

#endif
