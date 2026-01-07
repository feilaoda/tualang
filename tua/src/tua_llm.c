#include "tua_llm.h"

#include "tua_array.h"
#include "tua_map.h"

#include <math.h>
#include <limits.h>
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

static inline int64_t pair_key_i64(int64_t a, int64_t b) {
    // Match Tua side `_pairKey(a, b)` semantics without signed left-shift UB.
    uint64_t hi = ((uint64_t)(uint32_t)a) << 32;
    uint64_t lo = (uint64_t)(uint32_t)b;
    uint64_t k = hi | lo;
    return (int64_t)k;
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
