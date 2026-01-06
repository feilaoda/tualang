#include "tua_llm.h"

#include "tua_map.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static int check_f32_span(tua_bytes* b, int64_t off, int64_t n_floats) {
    if (!b) return 0;
    if (off < 0 || n_floats < 0) return 0;
    int64_t need = off + n_floats * 4;
    if (need < off) return 0;
    if (need > tua_bytes_len(b)) return 0;
    if (!tua_bytes_data_at(b, off)) return 0;
    return 1;
}

static inline float* f32p(tua_bytes* b, int64_t off) {
    return (float*)tua_bytes_data_at(b, off);
}

tua_err_t tua_llm_add_inplace_f32(tua_bytes* dst, int64_t dst_off, tua_bytes* src, int64_t src_off, int64_t n) {
    if (!check_f32_span(dst, dst_off, n) || !check_f32_span(src, src_off, n)) return TUA_E_INVALID;
    float* d = f32p(dst, dst_off);
    const float* s = f32p(src, src_off);
    for (int64_t i = 0; i < n; i++) d[i] += s[i];
    return TUA_OK;
}

tua_err_t tua_llm_rmsnorm_f32(tua_bytes* out, int64_t out_off, tua_bytes* x, int64_t x_off,
                              tua_bytes* w, int64_t w_off, int32_t n, float eps) {
    if (n <= 0) return TUA_E_INVALID;
    if (!check_f32_span(out, out_off, n) || !check_f32_span(x, x_off, n) || !check_f32_span(w, w_off, n)) {
        return TUA_E_INVALID;
    }
    float* y = f32p(out, out_off);
    const float* xv = f32p(x, x_off);
    const float* wt = f32p(w, w_off);
    double ss = 0.0;
    for (int32_t i = 0; i < n; i++) {
        double v = (double)xv[i];
        ss += v * v;
    }
    float mean = (float)(ss / (double)n);
    float inv = 1.0f / sqrtf(mean + eps);
    for (int32_t i = 0; i < n; i++) y[i] = xv[i] * inv * wt[i];
    return TUA_OK;
}

tua_err_t tua_llm_qk_rmsnorm_inplace_f32(tua_bytes* x, int64_t x_off, int32_t n_heads, int32_t head_dim,
                                        tua_bytes* w, int64_t w_off, float eps) {
    if (n_heads <= 0 || head_dim <= 0) return TUA_E_INVALID;
    int64_t total = (int64_t)n_heads * (int64_t)head_dim;
    if (!check_f32_span(x, x_off, total) || !check_f32_span(w, w_off, head_dim)) return TUA_E_INVALID;
    float* xv = f32p(x, x_off);
    const float* wt = f32p(w, w_off);
    for (int32_t h = 0; h < n_heads; h++) {
        float* p = xv + (int64_t)h * head_dim;
        double ss = 0.0;
        for (int32_t i = 0; i < head_dim; i++) {
            double v = (double)p[i];
            ss += v * v;
        }
        float mean = (float)(ss / (double)head_dim);
        float inv = 1.0f / sqrtf(mean + eps);
        for (int32_t i = 0; i < head_dim; i++) p[i] = p[i] * inv * wt[i];
    }
    return TUA_OK;
}

tua_err_t tua_llm_silu_mul_f32(tua_bytes* out, int64_t out_off, tua_bytes* gate, int64_t gate_off,
                              tua_bytes* up, int64_t up_off, int64_t n) {
    if (n < 0) return TUA_E_INVALID;
    if (!check_f32_span(out, out_off, n) || !check_f32_span(gate, gate_off, n) || !check_f32_span(up, up_off, n)) {
        return TUA_E_INVALID;
    }
    float* y = f32p(out, out_off);
    const float* g = f32p(gate, gate_off);
    const float* u = f32p(up, up_off);
    for (int64_t i = 0; i < n; i++) {
        float x = g[i];
        float s = 1.0f / (1.0f + expf(-x));
        y[i] = (x * s) * u[i];
    }
    return TUA_OK;
}

tua_err_t tua_llm_rope_inplace_f32(tua_bytes* x, int64_t x_off, int32_t n_heads, int32_t head_dim,
                                  int32_t pos, float theta) {
    if (n_heads <= 0 || head_dim <= 0 || (head_dim % 2) != 0) return TUA_E_INVALID;
    if (pos < 0) return TUA_E_INVALID;
    if (theta <= 0.0f) return TUA_E_INVALID;
    int64_t total = (int64_t)n_heads * (int64_t)head_dim;
    if (!check_f32_span(x, x_off, total)) return TUA_E_INVALID;

    float* v = f32p(x, x_off);
    int32_t half = head_dim / 2;
    // Precompute inv_freq for this head_dim.
    float inv_freq[256];
    if (half > (int32_t)(sizeof(inv_freq) / sizeof(inv_freq[0]))) return TUA_E_INVALID;
    for (int32_t i = 0; i < half; i++) {
        float expn = -2.0f * (float)i / (float)head_dim;
        inv_freq[i] = powf(theta, expn);
    }
    for (int32_t h = 0; h < n_heads; h++) {
        float* p = v + (int64_t)h * head_dim;
        for (int32_t i = 0; i < half; i++) {
            float angle = (float)pos * inv_freq[i];
            float c = cosf(angle);
            float s = sinf(angle);
            int32_t j = 2 * i;
            float x0 = p[j];
            float x1 = p[j + 1];
            p[j] = x0 * c - x1 * s;
            p[j + 1] = x0 * s + x1 * c;
        }
    }
    return TUA_OK;
}

tua_err_t tua_llm_attn_decode_f32(tua_bytes* out, int64_t out_off,
                                 tua_bytes* q, int64_t q_off,
                                 tua_bytes* kv, int64_t k_base, int64_t v_base,
                                 int64_t layer_bytes, int64_t token_bytes,
                                 int32_t layer, int32_t pos,
                                 int32_t n_heads, int32_t n_kv_heads, int32_t head_dim,
                                 float scale,
                                 tua_bytes* scratch, int64_t scratch_off, int64_t scratch_floats) {
    if (layer < 0 || pos < 0) return TUA_E_INVALID;
    if (n_heads <= 0 || n_kv_heads <= 0 || head_dim <= 0) return TUA_E_INVALID;
    if ((n_heads % n_kv_heads) != 0) return TUA_E_INVALID;
    if (scale <= 0.0f) return TUA_E_INVALID;

    int64_t qtot = (int64_t)n_heads * head_dim;
    if (!check_f32_span(out, out_off, qtot) || !check_f32_span(q, q_off, qtot)) return TUA_E_INVALID;
    if (!kv) return TUA_E_INVALID;
    if (!scratch) return TUA_E_INVALID;
    int64_t need_scores = (int64_t)pos + 1;
    if (scratch_floats < need_scores) return TUA_E_INVALID;
    if (!check_f32_span(scratch, scratch_off, scratch_floats)) return TUA_E_INVALID;

    const float* qv = f32p(q, q_off);
    float* outv = f32p(out, out_off);
    float* scores = f32p(scratch, scratch_off);

    uint8_t* kv_base = tua_bytes_data(kv);
    if (!kv_base) return TUA_E_INVALID;

    int32_t group = n_heads / n_kv_heads;
    for (int32_t h = 0; h < n_heads; h++) {
        int32_t kvh = h / group;
        const float* qh = qv + (int64_t)h * head_dim;

        float maxv = -INFINITY;
        for (int32_t t = 0; t <= pos; t++) {
            int64_t koff = k_base + (int64_t)layer * layer_bytes + (int64_t)t * token_bytes + (int64_t)kvh * head_dim * 4;
            float s = 0.0f;
            const float* kh = (const float*)(kv_base + koff);
            for (int32_t i = 0; i < head_dim; i++) s += qh[i] * kh[i];
            s *= scale;
            scores[t] = s;
            if (s > maxv) maxv = s;
        }

        float sum = 0.0f;
        for (int32_t t = 0; t <= pos; t++) {
            float e = expf(scores[t] - maxv);
            scores[t] = e;
            sum += e;
        }
        if (sum <= 0.0f) sum = 1.0f;
        float inv_sum = 1.0f / sum;

        float* oh = outv + (int64_t)h * head_dim;
        for (int32_t i = 0; i < head_dim; i++) oh[i] = 0.0f;

        for (int32_t t = 0; t <= pos; t++) {
            float p = scores[t] * inv_sum;
            int64_t voff = v_base + (int64_t)layer * layer_bytes + (int64_t)t * token_bytes + (int64_t)kvh * head_dim * 4;
            const float* vh = (const float*)(kv_base + voff);
            for (int32_t i = 0; i < head_dim; i++) oh[i] += p * vh[i];
        }
    }

    return TUA_OK;
}

tua_err_t tua_llm_argmax_f32(tua_bytes* x, int64_t x_off, int32_t n, int64_t* out_index) {
    if (!out_index) return TUA_E_INVALID;
    if (n <= 0) return TUA_E_INVALID;
    if (!check_f32_span(x, x_off, n)) return TUA_E_INVALID;
    const float* v = f32p(x, x_off);
    int32_t best_i = 0;
    float best_v = v[0];
    for (int32_t i = 1; i < n; i++) {
        float a = v[i];
        if (a > best_v) {
            best_v = a;
            best_i = i;
        }
    }
    *out_index = (int64_t)best_i;
    return TUA_OK;
}

