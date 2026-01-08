#include "tua_llm.h"

#include "tua_array.h"
#include "tua_map.h"

#include <math.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__)
#include <Accelerate/Accelerate.h>
#endif

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

tua_err_t tua_llm_gemv_f32(tua_bytes* y, int64_t y_off,
                          tua_bytes* a, int64_t a_off,
                          tua_bytes* x, int64_t x_off,
                          int32_t m, int32_t n) {
    if (m <= 0 || n <= 0) return TUA_E_INVALID;
    int64_t mn = (int64_t)m * (int64_t)n;
    if (mn <= 0) return TUA_E_INVALID;
    if (!check_f32_span(a, a_off, mn)) return TUA_E_INVALID;
    if (!check_f32_span(x, x_off, n)) return TUA_E_INVALID;
    if (!check_f32_span(y, y_off, m)) return TUA_E_INVALID;

    const float* A = (const float*)tua_bytes_data_at(a, a_off);
    const float* X = (const float*)tua_bytes_data_at(x, x_off);
    float* Y = (float*)tua_bytes_data_at(y, y_off);
    if (!A || !X || !Y) return TUA_E_INVALID;

#if defined(__APPLE__)
    // Chunk into smaller row blocks; avoids pathological cases and keeps stack usage modest.
    const int32_t block = 4096;
    for (int32_t row = 0; row < m; row += block) {
        int32_t mb = m - row;
        if (mb > block) mb = block;
        const float* Ab = A + (int64_t)row * (int64_t)n;
        float* Yb = Y + row;
        // Treat `X` as an [n,1] matrix and `Yb` as [mb,1] (row-major).
        cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans, mb, 1, n, 1.0f, Ab, n, X, 1, 0.0f, Yb, 1);
    }
    return TUA_OK;
#else
    for (int32_t i = 0; i < m; i++) {
        const float* row = A + (int64_t)i * (int64_t)n;
        double sum = 0.0;
        for (int32_t j = 0; j < n; j++) sum += (double)row[j] * (double)X[j];
        Y[i] = (float)sum;
    }
    return TUA_OK;
#endif
}

tua_err_t tua_llm_repetition_penalty_f32(tua_bytes* logits, int64_t logits_off, int32_t n,
                                        tua_array* ids, int32_t last_n, float penalty) {
    if (penalty <= 1.0f) return TUA_OK;
    if (n <= 0) return TUA_E_INVALID;
    if (!check_f32_span(logits, logits_off, n)) return TUA_E_INVALID;
    if (!ids) return TUA_E_INVALID;
    if (ids->elem_size != (int64_t)sizeof(int64_t)) return TUA_E_INVALID;
    if (ids->len <= 0 || !ids->data) return TUA_OK;
    if (last_n <= 0) return TUA_OK;

    float* L = (float*)tua_bytes_data_at(logits, logits_off);
    if (!L) return TUA_E_INVALID;

    int64_t len = ids->len;
    int64_t start = len - (int64_t)last_n;
    if (start < 0) start = 0;
    int64_t* p = (int64_t*)ids->data;

    for (int64_t i = start; i < len; i++) {
        int64_t tok = p[i];
        if (tok < 0 || tok >= (int64_t)n) continue;
        // De-dup within the window to avoid over-penalizing repeated tokens.
        int dup = 0;
        for (int64_t j = start; j < i; j++) {
            if (p[j] == tok) { dup = 1; break; }
        }
        if (dup) continue;

        float v = L[tok];
        L[tok] = (v > 0.0f) ? (v / penalty) : (v * penalty);
    }
    return TUA_OK;
}

tua_err_t tua_llm_no_repeat_ngram_f32(tua_bytes* logits, int64_t logits_off, int32_t n,
                                     tua_array* ids, int32_t ngram) {
    if (ngram <= 1) return TUA_OK;
    if (ngram > 16) return TUA_E_INVALID; // guard against abuse
    if (n <= 0) return TUA_E_INVALID;
    if (!check_f32_span(logits, logits_off, n)) return TUA_E_INVALID;
    if (!ids) return TUA_E_INVALID;
    if (ids->elem_size != (int64_t)sizeof(int64_t)) return TUA_E_INVALID;
    if (ids->len <= 0 || !ids->data) return TUA_OK;
    if (ids->len < (int64_t)(ngram - 1)) return TUA_OK;

    float* L = (float*)tua_bytes_data_at(logits, logits_off);
    if (!L) return TUA_E_INVALID;

    int64_t* p = (int64_t*)ids->data;
    int64_t len = ids->len;
    int32_t prefix_len = ngram - 1;
    int64_t prefix_start = len - prefix_len;

    // For each prior n-gram that shares the same (ngram-1) prefix as the current suffix,
    // ban its next token.
    for (int64_t i = 0; i + (int64_t)ngram <= len; i++) {
        int match = 1;
        for (int32_t j = 0; j < prefix_len; j++) {
            if (p[i + j] != p[prefix_start + j]) { match = 0; break; }
        }
        if (!match) continue;
        int64_t tok = p[i + prefix_len];
        if (tok < 0 || tok >= (int64_t)n) continue;
        L[tok] = -INFINITY;
    }
    return TUA_OK;
}

tua_err_t tua_llm_logit_ban_id_f32(tua_bytes* logits, int64_t logits_off, int32_t n, int64_t id) {
    if (n <= 0) return TUA_E_INVALID;
    if (!check_f32_span(logits, logits_off, n)) return TUA_E_INVALID;
    if (id < 0 || id >= (int64_t)n) return TUA_OK;
    float* L = (float*)tua_bytes_data_at(logits, logits_off);
    if (!L) return TUA_E_INVALID;
    L[id] = -INFINITY;
    return TUA_OK;
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

static inline uint64_t xorshift64star(uint64_t* state) {
    uint64_t x = state ? *state : 0;
    if (x == 0) x = 0x9e3779b97f4a7c15ULL;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *state = x;
    return x * 2685821657736338717ULL;
}

static inline double u01_from_u64(uint64_t x) {
    // Convert to [0,1) with 53 bits of precision.
    return (double)(x >> 11) * (1.0 / 9007199254740992.0);
}

static tua_err_t sample_topk_topp_from_ptr(const float* logits, int32_t n,
                                           int32_t top_k, float top_p, float temperature,
                                           uint64_t* rng_state, int64_t* out_index) {
    if (!out_index) return TUA_E_INVALID;
    if (n <= 0) return TUA_E_INVALID;
    if (!logits) return TUA_E_INVALID;

    if (temperature <= 0.0f) {
        int32_t best_i = 0;
        float best_v = logits[0];
        for (int32_t i = 1; i < n; i++) {
            float a = logits[i];
            if (a > best_v) {
                best_v = a;
                best_i = i;
            }
        }
        *out_index = (int64_t)best_i;
        return TUA_OK;
    }

    int32_t k = top_k;
    if (k <= 0 || k > n) k = n;
    if (k <= 0) k = 1;

    int32_t* ids = (int32_t*)malloc(sizeof(int32_t) * (size_t)k);
    float* vals = (float*)malloc(sizeof(float) * (size_t)k);
    float* probs = (float*)malloc(sizeof(float) * (size_t)k);
    if (!ids || !vals || !probs) {
        free(ids);
        free(vals);
        free(probs);
        return TUA_E_NOMEM;
    }

    // Initialize with the first k entries.
    int32_t min_idx = 0;
    float min_v = logits[0];
    for (int32_t i = 0; i < k; i++) {
        ids[i] = i;
        vals[i] = logits[i];
        if (i == 0 || vals[i] < min_v) {
            min_v = vals[i];
            min_idx = i;
        }
    }

    // Keep top-k by logit.
    for (int32_t i = k; i < n; i++) {
        float v = logits[i];
        if (v <= min_v) continue;
        ids[min_idx] = i;
        vals[min_idx] = v;
        // Recompute min.
        min_idx = 0;
        min_v = vals[0];
        for (int32_t j = 1; j < k; j++) {
            if (vals[j] < min_v) {
                min_v = vals[j];
                min_idx = j;
            }
        }
    }

    // Sort descending by logit (insertion sort; k is small).
    for (int32_t i = 1; i < k; i++) {
        float v = vals[i];
        int32_t id = ids[i];
        int32_t j = i - 1;
        while (j >= 0 && vals[j] < v) {
            vals[j + 1] = vals[j];
            ids[j + 1] = ids[j];
            j--;
        }
        vals[j + 1] = v;
        ids[j + 1] = id;
    }

    float maxv = vals[0] / temperature;
    for (int32_t i = 1; i < k; i++) {
        float sv = vals[i] / temperature;
        if (sv > maxv) maxv = sv;
    }

    float sum = 0.0f;
    for (int32_t i = 0; i < k; i++) {
        float sv = vals[i] / temperature;
        float e = expf(sv - maxv);
        probs[i] = e;
        sum += e;
    }
    if (sum <= 0.0f) sum = 1.0f;

    // Optional nucleus cutoff.
    int32_t use_k = k;
    if (top_p > 0.0f && top_p < 1.0f) {
        float acc = 0.0f;
        use_k = 0;
        for (int32_t i = 0; i < k; i++) {
            float p = probs[i] / sum;
            acc += p;
            use_k++;
            if (acc >= top_p) break;
        }
        if (use_k < 1) use_k = 1;
    }

    // Renormalize over the kept set and sample.
    float sum2 = 0.0f;
    for (int32_t i = 0; i < use_k; i++) sum2 += probs[i];
    if (sum2 <= 0.0f) sum2 = 1.0f;

    uint64_t rnd = xorshift64star(rng_state);
    double u = u01_from_u64(rnd);
    double r = u * (double)sum2;
    float c = 0.0f;
    int32_t pick = use_k - 1;
    for (int32_t i = 0; i < use_k; i++) {
        c += probs[i];
        if ((double)c >= r) {
            pick = i;
            break;
        }
    }
    *out_index = (int64_t)ids[pick];

    free(ids);
    free(vals);
    free(probs);
    return TUA_OK;
}

tua_err_t tua_llm_sample_topk_topp_f32(tua_bytes* logits, int64_t logits_off, int32_t n,
                                      int32_t top_k, float top_p, float temperature,
                                      int64_t* rng_state, int64_t* out_index) {
    if (!rng_state) return TUA_E_INVALID;
    if (!check_f32_span(logits, logits_off, n)) return TUA_E_INVALID;
    const float* v = f32p(logits, logits_off);
    uint64_t st = (uint64_t)(*rng_state);
    tua_err_t e = sample_topk_topp_from_ptr(v, n, top_k, top_p, temperature, &st, out_index);
    *rng_state = (int64_t)st;
    return e;
}

tua_err_t tua_llm_sample_topk_topp_f32_arr(tua_array* logits_f32,
                                          int32_t top_k, float top_p, float temperature,
                                          int64_t* rng_state, int64_t* out_index) {
    if (!rng_state) return TUA_E_INVALID;
    if (!logits_f32 || logits_f32->elem_size != (int64_t)sizeof(float)) return TUA_E_INVALID;
    if (logits_f32->len <= 0) return TUA_E_INVALID;
    if (!logits_f32->data) return TUA_E_INVALID;
    int32_t n = (int32_t)logits_f32->len;
    const float* v = (const float*)logits_f32->data;
    uint64_t st = (uint64_t)(*rng_state);
    tua_err_t e = sample_topk_topp_from_ptr(v, n, top_k, top_p, temperature, &st, out_index);
    *rng_state = (int64_t)st;
    return e;
}

static inline tua_value make_long_value_llm(int64_t x) {
    tua_value v;
    v.tag = 2; // TUA_VAL_LONG (must match src/tua_map.c)
    v.payload = (uint64_t)x;
    return v;
}

static inline int64_t value_to_i64_strict_llm(tua_value v) {
    if (v.tag == 2) return (int64_t)v.payload;      // long
    if (v.tag == 1) return (int64_t)(int32_t)v.payload; // int
    return INT64_MIN;
}

static inline int map_get_i64(tua_map* m, int64_t key, int64_t* out) {
    if (!m || !out) return 0;
    int32_t ok = 0;
    tua_value v = tua_map_get_with_ok(m, make_long_value_llm(key), &ok);
    if (!ok) return 0;
    int64_t x = value_to_i64_strict_llm(v);
    if (x == INT64_MIN) return 0;
    *out = x;
    return 1;
}

static inline const char* value_to_cstr_strict_llm(tua_value v) {
    if (v.tag == 5) return (const char*)(uintptr_t)v.payload; // TUA_VAL_STRING
    return NULL;
}

static inline int map_get_cstr(tua_map* m, int64_t key, const char** out) {
    if (!m || !out) return 0;
    int32_t ok = 0;
    tua_value v = tua_map_get_with_ok(m, make_long_value_llm(key), &ok);
    if (!ok) return 0;
    const char* s = value_to_cstr_strict_llm(v);
    if (!s) return 0;
    *out = s;
    return 1;
}

static inline int64_t pair_key_i64(int64_t a, int64_t b) {
    // Match Tua side `_pairKey(a, b)` semantics without signed left-shift UB.
    uint64_t hi = ((uint64_t)(uint32_t)a) << 32;
    uint64_t lo = (uint64_t)(uint32_t)b;
    uint64_t k = hi | lo;
    return (int64_t)k;
}

static inline int32_t gpt2_bytes_to_unicode_inv(int32_t cp) {
    // Inverse mapping for GPT-2 ByteLevel `bytes_to_unicode()`.
    // Allowed bytes map to themselves; excluded bytes map to code points starting at 256.
    if (cp >= 33 && cp <= 126) return cp;
    if (cp >= 161 && cp <= 172) return cp;
    if (cp >= 174 && cp <= 255) return cp;
    if (cp >= 256 && cp <= 323) {
        int32_t idx = cp - 256;
        if (idx < 33) return idx; // 0..32
        idx -= 33;
        if (idx < 34) return 127 + idx; // 127..160
        idx -= 34;
        if (idx == 0) return 173;
    }
    return -1;
}

static int utf8_decode1_strict(const char* s, size_t* io, int32_t* outCp) {
    if (!s || !io || !outCp) return 0;
    size_t i = *io;
    unsigned char c0 = (unsigned char)s[i];
    if (c0 == 0) return 0;
    if (c0 < 0x80) {
        *outCp = (int32_t)c0;
        *io = i + 1;
        return 1;
    }
    unsigned char c1 = (unsigned char)s[i + 1];
    if ((c0 & 0xE0) == 0xC0) {
        if ((c1 & 0xC0) != 0x80) return 0;
        int32_t cp = (int32_t)(((c0 & 0x1F) << 6) | (c1 & 0x3F));
        if (cp < 0x80) return 0; // overlong
        *outCp = cp;
        *io = i + 2;
        return 1;
    }
    unsigned char c2 = (unsigned char)s[i + 2];
    if ((c0 & 0xF0) == 0xE0) {
        if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80) return 0;
        int32_t cp = (int32_t)(((c0 & 0x0F) << 12) | ((c1 & 0x3F) << 6) | (c2 & 0x3F));
        if (cp < 0x800) return 0; // overlong
        if (cp >= 0xD800 && cp <= 0xDFFF) return 0; // surrogate
        *outCp = cp;
        *io = i + 3;
        return 1;
    }
    unsigned char c3 = (unsigned char)s[i + 3];
    if ((c0 & 0xF8) == 0xF0) {
        if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80 || (c3 & 0xC0) != 0x80) return 0;
        int32_t cp = (int32_t)(((c0 & 0x07) << 18) | ((c1 & 0x3F) << 12) | ((c2 & 0x3F) << 6) | (c3 & 0x3F));
        if (cp < 0x10000) return 0; // overlong
        if (cp > 0x10FFFF) return 0;
        *outCp = cp;
        *io = i + 4;
        return 1;
    }
    return 0;
}

tua_bytes* tua_llm_bpe_decode_ids_to_bytes(tua_array* ids, tua_map* idToToken, int32_t* outErr) {
    if (outErr) *outErr = 1;
    if (!outErr) return NULL;
    if (!ids || !idToToken) return NULL;
    if (ids->elem_size != (int64_t)sizeof(int64_t)) return NULL;
    if (ids->len < 0) return NULL;
    if (ids->len > INT32_MAX) return NULL;
    if (ids->len > 0 && !ids->data) return NULL;

    const int64_t* idp = (const int64_t*)ids->data;
    int32_t n = (int32_t)ids->len;

    // Pass 1: compute output byte length (1 byte per code point in byte-level token string).
    int64_t outLen = 0;
    for (int32_t i = 0; i < n; i++) {
        const char* tok = NULL;
        if (!map_get_cstr(idToToken, idp[i], &tok)) return NULL;
        size_t j = 0;
        while (tok[j] != '\0') {
            int32_t cp = 0;
            if (!utf8_decode1_strict(tok, &j, &cp)) return NULL;
            if (gpt2_bytes_to_unicode_inv(cp) < 0) return NULL;
            outLen++;
        }
    }

    tua_bytes* out = tua_bytes_new_uninit(outLen);
    if (!out && outLen == 0) {
        *outErr = 0;
        return tua_bytes_new_uninit(0);
    }
    if (!out) return NULL;
    uint8_t* dst = tua_bytes_data(out);
    if (!dst && outLen != 0) {
        tua_bytes_free(out);
        return NULL;
    }

    // Pass 2: fill output bytes.
    int64_t off = 0;
    for (int32_t i = 0; i < n; i++) {
        const char* tok = NULL;
        if (!map_get_cstr(idToToken, idp[i], &tok)) {
            tua_bytes_free(out);
            return NULL;
        }
        size_t j = 0;
        while (tok[j] != '\0') {
            int32_t cp = 0;
            if (!utf8_decode1_strict(tok, &j, &cp)) {
                tua_bytes_free(out);
                return NULL;
            }
            int32_t b = gpt2_bytes_to_unicode_inv(cp);
            if (b < 0) {
                tua_bytes_free(out);
                return NULL;
            }
            if (off < outLen) dst[off] = (uint8_t)b;
            off++;
        }
    }
    if (off != outLen) {
        // Should not happen; keep safe.
        tua_bytes_free(out);
        return NULL;
    }

    *outErr = 0;
    return out;
}

tua_array* tua_llm_bpe_merge_ids(tua_array* ids, tua_map* pairRank, tua_map* pairMergeId, int32_t* outErr) {
    if (outErr) *outErr = 1;
    if (!outErr) return NULL;
    if (!ids || !pairRank || !pairMergeId) return NULL;
    if (ids->elem_size != (int64_t)sizeof(int64_t)) return NULL;
    if (ids->len < 0) return NULL;
    if (ids->len > INT32_MAX) return NULL;
    int32_t n = (int32_t)ids->len;
    if (n == 0) {
        *outErr = 0;
        return tua_array_new(0, 0, (int64_t)sizeof(int64_t), -1);
    }
    if (!ids->data) return NULL;

    int64_t* a = (int64_t*)malloc((size_t)n * sizeof(int64_t));
    int64_t* b = (int64_t*)malloc((size_t)n * sizeof(int64_t));
    if (!a || !b) tua_panic("out of memory");
    const int64_t* in = (const int64_t*)ids->data;
    memcpy(a, in, (size_t)n * sizeof(int64_t));
    int32_t alen = n;

    // Reference behavior: repeatedly pick the lowest-rank pair, then merge all its occurrences in one pass.
    for (;;) {
        if (alen < 2) break;
        int64_t bestRank = INT64_MAX;
        int64_t bestA = 0;
        int64_t bestB = 0;
        int found = 0;

        for (int32_t i = 0; i + 1 < alen; i++) {
            int64_t key = pair_key_i64(a[i], a[i + 1]);
            int64_t rank = 0;
            if (map_get_i64(pairRank, key, &rank)) {
                if (rank < bestRank) {
                    bestRank = rank;
                    bestA = a[i];
                    bestB = a[i + 1];
                    found = 1;
                }
            }
        }
        if (!found) break;

        int64_t key2 = pair_key_i64(bestA, bestB);
        int64_t mid = 0;
        if (!map_get_i64(pairMergeId, key2, &mid)) break;

        int32_t blen = 0;
        for (int32_t i = 0; i < alen; i++) {
            if (i + 1 < alen && a[i] == bestA && a[i + 1] == bestB) {
                b[blen++] = mid;
                i++;
            } else {
                b[blen++] = a[i];
            }
        }

        int64_t* tmp = a;
        a = b;
        b = tmp;
        alen = blen;
    }

    tua_array* out = tua_array_new(0, alen, (int64_t)sizeof(int64_t), -1);
    for (int32_t i = 0; i < alen; i++) {
        tua_array_push(out, &a[i]);
    }

    free(a);
    free(b);
    *outErr = 0;
    return out;
}

tua_array* tua_llm_bpe_bytes_to_ids(tua_bytes* b, int64_t off, int64_t len, tua_array* byteToId, int32_t* outErr) {
    if (outErr) *outErr = 1;
    if (!outErr) return NULL;
    if (!b || !byteToId) return NULL;
    if (off < 0 || len < 0) return NULL;
    int64_t end = off + len;
    if (end < off) return NULL;
    if (end > tua_bytes_len(b)) return NULL;
    const uint8_t* p = tua_bytes_data_at(b, off);
    if (len > 0 && !p) return NULL;

    if (byteToId->elem_size != (int64_t)sizeof(int64_t)) return NULL;
    if (byteToId->len < 256) return NULL;
    if (!byteToId->data) return NULL;
    const int64_t* table = (const int64_t*)byteToId->data;

    if (len > INT32_MAX) return NULL;
    tua_array* out = tua_array_new(len, len, (int64_t)sizeof(int64_t), -1);
    if (!out) tua_panic("out of memory");
    int64_t* outv = (int64_t*)out->data;
    if (len > 0 && !outv) {
        tua_array_free(out);
        return NULL;
    }

    for (int64_t i = 0; i < len; i++) {
        uint8_t bb = p[i];
        int64_t id = table[(int)bb];
        if (id < 0) {
            tua_array_free(out);
            return NULL;
        }
        outv[i] = id;
    }

    *outErr = 0;
    return out;
}
