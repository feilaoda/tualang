#ifndef TUA_LLM_H
#define TUA_LLM_H

#include "rt/rt_err.h"
#include "tua_bytes.h"

#include <stdint.h>

// Elementwise kernels on contiguous float32 buffers stored in `bytes`.
// All offsets are in bytes; counts are in elements (float32).

tua_err_t tua_llm_add_inplace_f32(tua_bytes* dst, int64_t dst_off, tua_bytes* src, int64_t src_off, int64_t n);
tua_err_t tua_llm_rmsnorm_f32(tua_bytes* out, int64_t out_off, tua_bytes* x, int64_t x_off,
                              tua_bytes* w, int64_t w_off, int32_t n, float eps);
tua_err_t tua_llm_qk_rmsnorm_inplace_f32(tua_bytes* x, int64_t x_off, int32_t n_heads, int32_t head_dim,
                                        tua_bytes* w, int64_t w_off, float eps);
tua_err_t tua_llm_silu_mul_f32(tua_bytes* out, int64_t out_off, tua_bytes* gate, int64_t gate_off,
                              tua_bytes* up, int64_t up_off, int64_t n);
tua_err_t tua_llm_rope_inplace_f32(tua_bytes* x, int64_t x_off, int32_t n_heads, int32_t head_dim,
                                  int32_t pos, float theta);

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

tua_err_t tua_llm_argmax_f32(tua_bytes* x, int64_t x_off, int32_t n, int64_t* out_index);

#endif

