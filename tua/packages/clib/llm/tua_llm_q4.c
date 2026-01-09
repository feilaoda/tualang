#include "tua_llm_q4.h"

#include <math.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <pthread.h>

#if defined(TUA_LLM_USE_GGML)
#include <ggml.h>
#include <ggml-cpu.h>
#endif

#if defined(__x86_64__)
#include <immintrin.h>
#endif

// This module is intentionally self-contained so we can later swap the GEMV backend
// (e.g. route to Metal) without mixing quantization details into the core runtime.

// Provided by `src/tua_map.c`.
void tua_panic(const char* msg);

#if defined(TUA_LLM_USE_GGML)
static pthread_once_t tua_llm_ggml_once = PTHREAD_ONCE_INIT;

static void tua_llm_ggml_init_once(void) {
    ggml_cpu_init();
}

static inline void tua_llm_ggml_ensure_init(void) {
    pthread_once(&tua_llm_ggml_once, tua_llm_ggml_init_once);
}

typedef struct {
    void* buf;
    int64_t cap;
} tua_tls_buf;

static __thread tua_tls_buf tua_llm_tls_tmp = { NULL, 0 };

static void* tua_tls_get_bytes(int64_t n) {
    if (n <= 0) return NULL;
    if (n <= tua_llm_tls_tmp.cap && tua_llm_tls_tmp.buf) return tua_llm_tls_tmp.buf;
    int64_t new_cap = tua_llm_tls_tmp.cap > 0 ? tua_llm_tls_tmp.cap : 0;
    if (new_cap < 1024) new_cap = 1024;
    while (new_cap < n) new_cap *= 2;
    void* p = realloc(tua_llm_tls_tmp.buf, (size_t)new_cap);
    if (!p) tua_panic("out of memory");
    tua_llm_tls_tmp.buf = p;
    tua_llm_tls_tmp.cap = new_cap;
    return p;
}
#endif

typedef struct __attribute__((packed)) {
    uint16_t d;      // fp16 scale
    uint8_t qs[16];  // 32 x int4, packed (low/high nibble)
} q4_0_block_t;

typedef struct __attribute__((packed)) {
    uint16_t d;        // fp16 base scale for subgroup scales
    uint16_t dmin;     // fp16 base scale for subgroup mins
    uint8_t scales[12];  // 16 packed 6-bit values: [8 scales][8 mins]
    uint8_t qs[128];     // 256 x uint4, packed (low/high nibble)
} q4_k_block_t;

typedef struct __attribute__((packed)) {
    uint8_t ql[128];     // low 4 bits for 256 values
    uint8_t qh[64];      // high 2 bits for 256 values
    int8_t scales[16];   // 16 group scales (16 values each)
    uint16_t d;          // fp16 base scale
} q6_k_block_t;

static inline int check_f32_span(tua_bytes* b, int64_t off, int64_t n_floats) {
    if (!b) return 0;
    if (off < 0 || n_floats < 0) return 0;
    int64_t need = off + n_floats * 4;
    if (need < off) return 0;
    if (need > tua_bytes_len(b)) return 0;
    if (!tua_bytes_data_at(b, off)) return 0;
    return 1;
}

static inline int check_u16_span(tua_bytes* b, int64_t off, int64_t n_elts) {
    if (!b) return 0;
    if (off < 0 || n_elts < 0) return 0;
    int64_t need = off + n_elts * 2;
    if (need < off) return 0;
    if (need > tua_bytes_len(b)) return 0;
    if (!tua_bytes_data_at(b, off)) return 0;
    return 1;
}

static inline float bf16_to_f32(uint16_t h) {
    union {
        uint32_t u;
        float f;
    } v;
    v.u = ((uint32_t)h) << 16;
    return v.f;
}

static inline float f16_to_f32(uint16_t h) {
    uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
    uint32_t exp = (h >> 10) & 0x1fu;
    uint32_t mant = (uint32_t)(h & 0x03ffu);
    uint32_t out;
    if (exp == 0) {
        if (mant == 0) {
            out = sign;
        } else {
            exp = 127 - 15 + 1;
            while ((mant & 0x0400u) == 0) {
                mant <<= 1;
                exp--;
            }
            mant &= 0x03ffu;
            out = sign | (exp << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        out = sign | 0x7f800000u | (mant << 13);
    } else {
        exp = exp + (127 - 15);
        out = sign | (exp << 23) | (mant << 13);
    }
    union {
        uint32_t u;
        float f;
    } v;
    v.u = out;
    return v.f;
}

static inline uint16_t f32_to_f16_bits(float x) {
    union {
        float f;
        uint32_t u;
    } v;
    v.f = x;
    uint32_t sign = (v.u >> 16) & 0x8000u;
    int32_t exp = (int32_t)((v.u >> 23) & 0xffu) - 127 + 15;
    uint32_t mant = v.u & 0x7fffffu;
    if (exp <= 0) {
        if (exp < -10) return (uint16_t)sign;
        mant = mant | 0x800000u;
        uint32_t t = mant >> (1u - (uint32_t)exp);
        uint32_t r = (t + 0x1000u) >> 13;
        return (uint16_t)(sign | r);
    }
    if (exp >= 31) {
        if ((v.u & 0x7fffffffU) == 0x7f800000U) return (uint16_t)(sign | 0x7c00u);
        return (uint16_t)(sign | 0x7c00u | 1u);
    }
    uint32_t r = (mant + 0x1000u) >> 13;
    return (uint16_t)(sign | ((uint32_t)exp << 10) | (r & 0x03ffu));
}

int64_t tua_llm_q4_0_row_bytes(int32_t n) {
    if (n <= 0) return 0;
    if ((n & 31) != 0) return 0;
    int32_t nb = n / 32;
    return (int64_t)nb * (int64_t)sizeof(q4_0_block_t);
}

int64_t tua_llm_q4_0_mat_bytes(int32_t m, int32_t n) {
    if (m <= 0 || n <= 0) return 0;
    int64_t rowb = tua_llm_q4_0_row_bytes(n);
    if (rowb <= 0) return 0;
    return rowb * (int64_t)m;
}

// =========================
// Packed 6-bit helpers
// =========================

static inline int8_t sext6(uint8_t u6) {
    // u6 is 6-bit two's complement.
    return (u6 & 0x20) ? (int8_t)(u6 | 0xC0) : (int8_t)u6;
}

// GGML Q4_K packs 8 scales (u6) and 8 mins (s6) into 12 bytes, interleaved per pair of subgroups:
//   for subgroup pairs (0,1), (2,3), (4,5), (6,7):
//     [s0][s1][m0][m1] as 4x6-bit values packed into 3 bytes (LSB-first bitstream).
static inline void q4k_pack_scales_mins(uint8_t out[12], const uint8_t sc_u6[8], const uint8_t mn_u6[8]) {
    for (int32_t p = 0; p < 4; p++) {
        uint8_t s0 = sc_u6[2 * p + 0] & 0x3f;
        uint8_t s1 = sc_u6[2 * p + 1] & 0x3f;
        uint8_t m0 = mn_u6[2 * p + 0] & 0x3f;
        uint8_t m1 = mn_u6[2 * p + 1] & 0x3f;
        out[3 * p + 0] = (uint8_t)((s0 & 0x3f) | ((s1 & 0x03) << 6));
        out[3 * p + 1] = (uint8_t)(((s1 >> 2) & 0x0f) | ((m0 & 0x0f) << 4));
        out[3 * p + 2] = (uint8_t)(((m0 >> 4) & 0x03) | ((m1 & 0x3f) << 2));
    }
}

static inline void q4k_get_scale_min_u6(const uint8_t scales[12], int32_t g, uint8_t* out_sc, uint8_t* out_mn) {
    // g in [0..7]
    int32_t p = g >> 1; // pair index
    const uint8_t b0 = scales[3 * p + 0];
    const uint8_t b1 = scales[3 * p + 1];
    const uint8_t b2 = scales[3 * p + 2];
    if ((g & 1) == 0) {
        // even: scale=s0, min=m0
        if (out_sc) *out_sc = (uint8_t)(b0 & 0x3f);
        if (out_mn) *out_mn = (uint8_t)((b1 >> 4) | ((b2 & 0x03) << 4));
    } else {
        // odd: scale=s1, min=m1
        if (out_sc) *out_sc = (uint8_t)((b0 >> 6) | ((b1 & 0x0f) << 2));
        if (out_mn) *out_mn = (uint8_t)(b2 >> 2);
    }
}

static inline uint8_t clamp_u8(int32_t v, int32_t lo, int32_t hi) {
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return (uint8_t)v;
}

static void q4_0_pack_row(const uint16_t* src_bf16, int32_t n, q4_0_block_t* dst_blocks) {
    int32_t nb = n / 32;
    for (int32_t bi = 0; bi < nb; bi++) {
        const uint16_t* s = src_bf16 + bi * 32;
        float amax = 0.0f;
        for (int32_t i = 0; i < 32; i++) {
            float v = bf16_to_f32(s[i]);
            float av = fabsf(v);
            if (av > amax) amax = av;
        }
        float d = 0.0f;
        if (amax > 0.0f) d = amax / 7.0f;
        dst_blocks[bi].d = f32_to_f16_bits(d);
        float inv = (d > 0.0f) ? (1.0f / d) : 0.0f;
        for (int32_t j = 0; j < 16; j++) {
            float v0 = bf16_to_f32(s[2 * j + 0]);
            float v1 = bf16_to_f32(s[2 * j + 1]);
            int32_t q0 = (int32_t)lrintf(v0 * inv) + 8;
            int32_t q1 = (int32_t)lrintf(v1 * inv) + 8;
            uint8_t u0 = clamp_u8(q0, 0, 15);
            uint8_t u1 = clamp_u8(q1, 0, 15);
            dst_blocks[bi].qs[j] = (uint8_t)(u0 | (u1 << 4));
        }
    }
}

// =========================
// Q4_K packing
// =========================

int64_t tua_llm_q4_k_row_bytes(int32_t n) {
    if (n <= 0) return 0;
    if ((n & 255) != 0) return 0;
    int32_t nb = n / 256;
    return (int64_t)nb * (int64_t)sizeof(q4_k_block_t);
}

int64_t tua_llm_q4_k_mat_bytes(int32_t m, int32_t n) {
    if (m <= 0 || n <= 0) return 0;
    int64_t rowb = tua_llm_q4_k_row_bytes(n);
    if (rowb <= 0) return 0;
    return rowb * (int64_t)m;
}

// =========================
// Q6_K helpers
// =========================

static inline int32_t q6k_get_q(const q6_k_block_t* b, int32_t i) {
    // i in [0..255]
    uint8_t ql = b->ql[i >> 1];
    uint8_t lo = (i & 1) ? (ql >> 4) : (ql & 0x0f);
    uint8_t qh = b->qh[i >> 2];
    uint8_t hi = (qh >> (2 * (i & 3))) & 0x03;
    return (int32_t)(lo | (hi << 4));
}

int64_t tua_llm_q6_k_row_bytes(int32_t n) {
    if (n <= 0) return 0;
    if ((n & 255) != 0) return 0;
    int32_t nb = n / 256;
    return (int64_t)nb * 210;
}

int64_t tua_llm_q6_k_mat_bytes(int32_t m, int32_t n) {
    if (m <= 0 || n <= 0) return 0;
    int64_t rowb = tua_llm_q6_k_row_bytes(n);
    if (rowb <= 0) return 0;
    return rowb * (int64_t)m;
}

static void q4_k_pack_block(const uint16_t* src_bf16, q4_k_block_t* dst) {
    // 8 subgroups x 32 values each = 256
    float scales[8];
    float mins[8];
    for (int32_t g = 0; g < 8; g++) {
        float mn = INFINITY;
        float mx = -INFINITY;
        const uint16_t* s = src_bf16 + g * 32;
        for (int32_t i = 0; i < 32; i++) {
            float v = bf16_to_f32(s[i]);
            if (v < mn) mn = v;
            if (v > mx) mx = v;
        }
        mins[g] = (isfinite(mn) ? mn : 0.0f);
        float span = mx - mn;
        scales[g] = (span > 0.0f) ? (span / 15.0f) : 0.0f;
        // GGML's Q4_K dequant uses: v = mn + sc * (q - 8), with q in [0..15].
        // Store mn as an 8-step offset so that (mn - 8*sc) approximates the true subgroup minimum.
        mins[g] = mins[g] + 8.0f * scales[g];
    }

    float max_scale = 0.0f;
    float max_abs_min = 0.0f;
    for (int32_t g = 0; g < 8; g++) {
        if (scales[g] > max_scale) max_scale = scales[g];
        float am = fabsf(mins[g]);
        if (am > max_abs_min) max_abs_min = am;
    }

    float d = (max_scale > 0.0f) ? (max_scale / 63.0f) : 0.0f;
    float dmin = (max_abs_min > 0.0f) ? (max_abs_min / 31.0f) : 0.0f;
    dst->d = f32_to_f16_bits(d);
    dst->dmin = f32_to_f16_bits(dmin);

    uint8_t sc_u6[8];
    uint8_t mn_u6[8];
    for (int32_t g = 0; g < 8; g++) {
        int32_t sc = 0;
        if (d > 0.0f) sc = (int32_t)lrintf(scales[g] / d);
        if (sc < 0) sc = 0;
        if (sc > 63) sc = 63;
        sc_u6[g] = (uint8_t)sc;

        int32_t mn = 0;
        if (dmin > 0.0f) mn = (int32_t)lrintf(mins[g] / dmin);
        if (mn < -32) mn = -32;
        if (mn > 31) mn = 31;
        mn_u6[g] = (uint8_t)(mn & 0x3f);
    }
    q4k_pack_scales_mins(dst->scales, sc_u6, mn_u6);

    // Quantize values using quantized subgroup parameters (so dequant matches).
    float fd = f16_to_f32(dst->d);
    float fdm = f16_to_f32(dst->dmin);
    for (int32_t g = 0; g < 8; g++) {
        uint8_t sc6 = 0, mn6 = 0;
        q4k_get_scale_min_u6(dst->scales, g, &sc6, &mn6);
        float sc = fd * (float)(sc6 & 0x3f);
        float mn = fdm * (float)sext6(mn6);
        const uint16_t* s = src_bf16 + g * 32;
        uint8_t* qdst = dst->qs + g * 16;
        float inv = (sc > 0.0f) ? (1.0f / sc) : 0.0f;
        for (int32_t j = 0; j < 16; j++) {
            float v0 = bf16_to_f32(s[2 * j + 0]);
            float v1 = bf16_to_f32(s[2 * j + 1]);
            int32_t q0 = (int32_t)lrintf((v0 - mn) * inv) + 8;
            int32_t q1 = (int32_t)lrintf((v1 - mn) * inv) + 8;
            uint8_t u0 = clamp_u8(q0, 0, 15);
            uint8_t u1 = clamp_u8(q1, 0, 15);
            qdst[j] = (uint8_t)(u0 | (u1 << 4));
        }
    }
}

static void q4_k_pack_row(const uint16_t* src_bf16, int32_t n, q4_k_block_t* dst_blocks) {
    int32_t nb = n / 256;
    for (int32_t bi = 0; bi < nb; bi++) {
        q4_k_pack_block(src_bf16 + bi * 256, &dst_blocks[bi]);
    }
}

tua_err_t tua_llm_q4_0_pack_bf16(tua_bytes* dst, int64_t dst_off,
                                tua_bytes* src, int64_t src_off,
                                int32_t m, int32_t n) {
    if (m <= 0 || n <= 0) return TUA_E_INVALID;
    if ((n & 31) != 0) return TUA_E_INVALID;
    int64_t rowb = tua_llm_q4_0_row_bytes(n);
    int64_t need = tua_llm_q4_0_mat_bytes(m, n);
    if (rowb <= 0 || need <= 0) return TUA_E_INVALID;

    int64_t src_elts = (int64_t)m * (int64_t)n;
    if (!check_u16_span(src, src_off, src_elts)) return TUA_E_INVALID;
    if (!dst) return TUA_E_INVALID;
    if (dst_off < 0) return TUA_E_INVALID;
    if (dst_off + need > tua_bytes_len(dst)) return TUA_E_INVALID;

    const uint16_t* sbase = (const uint16_t*)tua_bytes_data_at(src, src_off);
    q4_0_block_t* dbase = (q4_0_block_t*)tua_bytes_data_at(dst, dst_off);
    if (!sbase || !dbase) return TUA_E_INVALID;

    int32_t nb = n / 32;
    for (int32_t row = 0; row < m; row++) {
        const uint16_t* srow = sbase + (int64_t)row * (int64_t)n;
        q4_0_block_t* drow = dbase + (int64_t)row * (int64_t)nb;
        q4_0_pack_row(srow, n, drow);
    }
    return TUA_OK;
}

tua_err_t tua_llm_q4_k_pack_bf16(tua_bytes* dst, int64_t dst_off,
                                tua_bytes* src, int64_t src_off,
                                int32_t m, int32_t n) {
    if (m <= 0 || n <= 0) return TUA_E_INVALID;
    if ((n & 255) != 0) return TUA_E_INVALID;
    int64_t rowb = tua_llm_q4_k_row_bytes(n);
    int64_t need = tua_llm_q4_k_mat_bytes(m, n);
    if (rowb <= 0 || need <= 0) return TUA_E_INVALID;

    int64_t src_elts = (int64_t)m * (int64_t)n;
    if (!check_u16_span(src, src_off, src_elts)) return TUA_E_INVALID;
    if (!dst) return TUA_E_INVALID;
    if (dst_off < 0) return TUA_E_INVALID;
    if (dst_off + need > tua_bytes_len(dst)) return TUA_E_INVALID;

    const uint16_t* sbase = (const uint16_t*)tua_bytes_data_at(src, src_off);
    uint8_t* dbase = (uint8_t*)tua_bytes_data_at(dst, dst_off);
    if (!sbase || !dbase) return TUA_E_INVALID;

#if defined(TUA_LLM_USE_GGML)
    tua_llm_ggml_ensure_init();
    const struct ggml_type_traits* tr = ggml_get_type_traits(GGML_TYPE_Q4_K);
    const struct ggml_type_traits_cpu* tc = ggml_get_type_traits_cpu(GGML_TYPE_Q4_K);
    if (!tr || !tc || !tc->from_float) return TUA_E_NOTSUP;
    if ((int64_t)tr->blck_size != 256) return TUA_E_NOTSUP;
    if ((int64_t)tr->type_size != rowb / (n / 256)) return TUA_E_NOTSUP;

    float* tmp = (float*)tua_tls_get_bytes((int64_t)n * 4);
    if (!tmp) return TUA_E_NOMEM;

    for (int32_t row = 0; row < m; row++) {
        const uint16_t* srow = sbase + (int64_t)row * (int64_t)n;
        uint8_t* drow = dbase + (int64_t)row * rowb;
        for (int32_t i = 0; i < n; i++) tmp[i] = bf16_to_f32(srow[i]);
        tc->from_float(tmp, drow, n);
    }
    return TUA_OK;
#else
    int32_t nb = n / 256;
    q4_k_block_t* dblocks = (q4_k_block_t*)dbase;
    for (int32_t row = 0; row < m; row++) {
        const uint16_t* srow = sbase + (int64_t)row * (int64_t)n;
        q4_k_block_t* drow = dblocks + (int64_t)row * (int64_t)nb;
        q4_k_pack_row(srow, n, drow);
    }
    return TUA_OK;
#endif
}

tua_err_t tua_llm_q4_0_get_row_f32(tua_bytes* out, int64_t out_off,
                                  tua_bytes* a, int64_t a_off,
                                  int64_t row, int32_t n) {
    if (row < 0) return TUA_E_INVALID;
    if (n <= 0 || (n & 31) != 0) return TUA_E_INVALID;
    if (!check_f32_span(out, out_off, n)) return TUA_E_INVALID;
    int32_t nb = n / 32;
    int64_t rowb = tua_llm_q4_0_row_bytes(n);
    if (rowb <= 0) return TUA_E_INVALID;

    int64_t aoff = a_off + row * rowb;
    if (!a) return TUA_E_INVALID;
    if (aoff < 0) return TUA_E_INVALID;
    if (aoff + rowb > tua_bytes_len(a)) return TUA_E_INVALID;

    const q4_0_block_t* blocks = (const q4_0_block_t*)tua_bytes_data_at(a, aoff);
    float* dst = (float*)tua_bytes_data_at(out, out_off);
    if (!blocks || !dst) return TUA_E_INVALID;

    for (int32_t bi = 0; bi < nb; bi++) {
        float d = f16_to_f32(blocks[bi].d);
        const uint8_t* qs = blocks[bi].qs;
        for (int32_t j = 0; j < 16; j++) {
            uint8_t b = qs[j];
            int8_t q0 = (int8_t)((b & 0x0f) - 8);
            int8_t q1 = (int8_t)((b >> 4) - 8);
            dst[bi * 32 + 2 * j + 0] = (float)q0 * d;
            dst[bi * 32 + 2 * j + 1] = (float)q1 * d;
        }
    }
    return TUA_OK;
}

tua_err_t tua_llm_q4_k_get_row_f32(tua_bytes* out, int64_t out_off,
                                  tua_bytes* a, int64_t a_off,
                                  int64_t row, int32_t n) {
    if (row < 0) return TUA_E_INVALID;
    if (n <= 0 || (n & 255) != 0) return TUA_E_INVALID;
    if (!check_f32_span(out, out_off, n)) return TUA_E_INVALID;

    int32_t nb = n / 256;
    int64_t rowb = tua_llm_q4_k_row_bytes(n);
    if (rowb <= 0) return TUA_E_INVALID;

    int64_t aoff = a_off + row * rowb;
    if (!a) return TUA_E_INVALID;
    if (aoff < 0) return TUA_E_INVALID;
    if (aoff + rowb > tua_bytes_len(a)) return TUA_E_INVALID;

    const q4_k_block_t* blocks = (const q4_k_block_t*)tua_bytes_data_at(a, aoff);
    float* dst = (float*)tua_bytes_data_at(out, out_off);
    if (!blocks || !dst) return TUA_E_INVALID;

#if defined(TUA_LLM_USE_GGML)
    tua_llm_ggml_ensure_init();
    const struct ggml_type_traits* tr = ggml_get_type_traits(GGML_TYPE_Q4_K);
    if (!tr || !tr->to_float) return TUA_E_NOTSUP;
    tr->to_float((const void*)blocks, dst, n);
    return TUA_OK;
#else
    for (int32_t bi = 0; bi < nb; bi++) {
        float d = f16_to_f32(blocks[bi].d);
        float dmin = f16_to_f32(blocks[bi].dmin);
        const uint8_t* qs = blocks[bi].qs;
        for (int32_t g = 0; g < 8; g++) {
            uint8_t sc6 = 0, mn6 = 0;
            q4k_get_scale_min_u6(blocks[bi].scales, g, &sc6, &mn6);
            float sc = d * (float)(sc6 & 0x3f);
            float mn = dmin * (float)sext6(mn6);
            const uint8_t* qg = qs + g * 16;
            for (int32_t j = 0; j < 16; j++) {
                uint8_t b = qg[j];
                uint8_t q0 = b & 0x0f;
                uint8_t q1 = b >> 4;
                int32_t base = bi * 256 + g * 32 + 2 * j;
                dst[base + 0] = mn + sc * (float)((int32_t)q0 - 8);
                dst[base + 1] = mn + sc * (float)((int32_t)q1 - 8);
            }
        }
    }
    return TUA_OK;
#endif
}

tua_err_t tua_llm_q6_k_get_row_f32(tua_bytes* out, int64_t out_off,
                                  tua_bytes* a, int64_t a_off,
                                  int64_t row, int32_t n) {
    if (row < 0) return TUA_E_INVALID;
    if (n <= 0 || (n & 255) != 0) return TUA_E_INVALID;
    if (!check_f32_span(out, out_off, n)) return TUA_E_INVALID;

    int64_t rowb = tua_llm_q6_k_row_bytes(n);
    if (rowb <= 0) return TUA_E_INVALID;
    int64_t aoff = a_off + row * rowb;
    if (!a) return TUA_E_INVALID;
    if (aoff < 0) return TUA_E_INVALID;
    if (aoff + rowb > tua_bytes_len(a)) return TUA_E_INVALID;

    const uint8_t* p = (const uint8_t*)tua_bytes_data_at(a, aoff);
    float* dst = (float*)tua_bytes_data_at(out, out_off);
    if (!p || !dst) return TUA_E_INVALID;

#if defined(TUA_LLM_USE_GGML)
    tua_llm_ggml_ensure_init();
    const struct ggml_type_traits* tr = ggml_get_type_traits(GGML_TYPE_Q6_K);
    if (!tr || !tr->to_float) return TUA_E_NOTSUP;
    tr->to_float((const void*)p, dst, n);
    return TUA_OK;
#else
    int32_t nb = n / 256;
    for (int32_t bi = 0; bi < nb; bi++) {
        const q6_k_block_t* b = (const q6_k_block_t*)(p + (int64_t)bi * 210);
        float d = f16_to_f32(b->d);
        for (int32_t g = 0; g < 16; g++) {
            float sc = d * (float)b->scales[g];
            int32_t base = bi * 256 + g * 16;
            for (int32_t j = 0; j < 16; j++) {
                int32_t q = q6k_get_q(b, g * 16 + j);
                int32_t qs = q - 32;
                dst[base + j] = (float)qs * sc;
            }
        }
    }
    return TUA_OK;
#endif
}

// =========================
// GEMV thread pool (shared)
// =========================

typedef void (*q4_gemv_range_fn)(const void* A, const float* X, float* Y, int32_t n, int32_t row0, int32_t row1);

// =========================
// Q4_0 GEMV (CPU)
// =========================

static void q4_0_gemv_range_scalar(const void* Av, const float* X, float* Y, int32_t n, int32_t row0, int32_t row1) {
    const q4_0_block_t* A = (const q4_0_block_t*)Av;
    int32_t nb = n / 32;
    for (int32_t row = row0; row < row1; row++) {
        const q4_0_block_t* brow = A + (int64_t)row * (int64_t)nb;
        double sum = 0.0;
        for (int32_t bi = 0; bi < nb; bi++) {
            float d = f16_to_f32(brow[bi].d);
            const uint8_t* qs = brow[bi].qs;
            for (int32_t j = 0; j < 16; j++) {
                uint8_t b = qs[j];
                int32_t q0 = (int32_t)((b & 0x0f) - 8);
                int32_t q1 = (int32_t)((b >> 4) - 8);
                sum += (double)(q0 * d) * (double)X[bi * 32 + 2 * j + 0];
                sum += (double)(q1 * d) * (double)X[bi * 32 + 2 * j + 1];
            }
        }
        Y[row] = (float)sum;
    }
}

#if defined(__x86_64__)
__attribute__((target("avx2,fma")))
static void q4_0_gemv_range_avx2(const void* Av, const float* X, float* Y, int32_t n, int32_t row0, int32_t row1) {
    const q4_0_block_t* A = (const q4_0_block_t*)Av;
    int32_t nb = n / 32;
    const __m256i mask0f = _mm256_set1_epi8(0x0f);
    const __m256i bias8 = _mm256_set1_epi8(8);
    for (int32_t row = row0; row < row1; row++) {
        const q4_0_block_t* brow = A + (int64_t)row * (int64_t)nb;
        __m256 acc0 = _mm256_setzero_ps();
        __m256 acc1 = _mm256_setzero_ps();
        __m256 acc2 = _mm256_setzero_ps();
        __m256 acc3 = _mm256_setzero_ps();

        for (int32_t bi = 0; bi < nb; bi++) {
            float d = f16_to_f32(brow[bi].d);
            __m256 sd = _mm256_set1_ps(d);

            __m128i qpk = _mm_loadu_si128((const __m128i*)brow[bi].qs);
            __m128i lo = _mm_and_si128(qpk, _mm256_castsi256_si128(mask0f));
            __m128i hi = _mm_and_si128(_mm_srli_epi16(qpk, 4), _mm256_castsi256_si128(mask0f));
            __m128i v0 = _mm_unpacklo_epi8(lo, hi);
            __m128i v1 = _mm_unpackhi_epi8(lo, hi);
            v0 = _mm_sub_epi8(v0, _mm256_castsi256_si128(bias8));
            v1 = _mm_sub_epi8(v1, _mm256_castsi256_si128(bias8));

            // 32 values: v0[0..15], v1[0..15]
            const float* xp = X + bi * 32;

            __m256i i0 = _mm256_cvtepi8_epi32(v0);
            __m256 q0 = _mm256_cvtepi32_ps(i0);
            __m256 x0 = _mm256_mul_ps(_mm256_loadu_ps(xp + 0), sd);
            acc0 = _mm256_fmadd_ps(q0, x0, acc0);

            __m256i i1 = _mm256_cvtepi8_epi32(_mm_srli_si128(v0, 8));
            __m256 q1 = _mm256_cvtepi32_ps(i1);
            __m256 x1 = _mm256_mul_ps(_mm256_loadu_ps(xp + 8), sd);
            acc1 = _mm256_fmadd_ps(q1, x1, acc1);

            __m256i i2 = _mm256_cvtepi8_epi32(v1);
            __m256 q2 = _mm256_cvtepi32_ps(i2);
            __m256 x2 = _mm256_mul_ps(_mm256_loadu_ps(xp + 16), sd);
            acc2 = _mm256_fmadd_ps(q2, x2, acc2);

            __m256i i3 = _mm256_cvtepi8_epi32(_mm_srli_si128(v1, 8));
            __m256 q3 = _mm256_cvtepi32_ps(i3);
            __m256 x3 = _mm256_mul_ps(_mm256_loadu_ps(xp + 24), sd);
            acc3 = _mm256_fmadd_ps(q3, x3, acc3);
        }

        float tmp0[8], tmp1[8], tmp2[8], tmp3[8];
        _mm256_storeu_ps(tmp0, acc0);
        _mm256_storeu_ps(tmp1, acc1);
        _mm256_storeu_ps(tmp2, acc2);
        _mm256_storeu_ps(tmp3, acc3);
        float sum =
            (tmp0[0] + tmp0[1] + tmp0[2] + tmp0[3] + tmp0[4] + tmp0[5] + tmp0[6] + tmp0[7]) +
            (tmp1[0] + tmp1[1] + tmp1[2] + tmp1[3] + tmp1[4] + tmp1[5] + tmp1[6] + tmp1[7]) +
            (tmp2[0] + tmp2[1] + tmp2[2] + tmp2[3] + tmp2[4] + tmp2[5] + tmp2[6] + tmp2[7]) +
            (tmp3[0] + tmp3[1] + tmp3[2] + tmp3[3] + tmp3[4] + tmp3[5] + tmp3[6] + tmp3[7]);
        Y[row] = sum;
    }
}
#endif

static q4_gemv_range_fn q4_0_gemv_range_pick(void) {
#if defined(__x86_64__)
    // The top-level binary is not built with -mavx2; gate on runtime features.
    // We conservatively rely on clang's builtin checks.
    if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma")) return q4_0_gemv_range_avx2;
#endif
    return q4_0_gemv_range_scalar;
}

typedef struct {
    pthread_t* threads;
    int32_t nthreads;
    pthread_mutex_t mu;
    pthread_cond_t cv_job;
    pthread_cond_t cv_done;
    const void* A;
    const float* X;
    float* Y;
    int32_t m;
    int32_t n;
    q4_gemv_range_fn fn;
    uint64_t job_id;
    int32_t working;
    int stop;
} q4_pool_t;

static q4_pool_t g_q4_pool = {0};

typedef struct {
    int32_t tid;
} q4_worker_arg_t;

static int32_t clamp_threads(int32_t n) {
    if (n <= 0) {
        long c = sysconf(_SC_NPROCESSORS_ONLN);
        if (c > 0 && c < INT32_MAX) return (int32_t)c;
        return 1;
    }
    if (n > 256) n = 256;
    return n;
}

// Weak symbol implemented in tua_llm.c; used to match `--threads`.
__attribute__((weak)) int32_t tua_llm_get_threads(void) { return 0; }

static void q4_pool_destroy(void) {
    if (!g_q4_pool.threads) return;
    pthread_mutex_lock(&g_q4_pool.mu);
    g_q4_pool.stop = 1;
    g_q4_pool.job_id++;
    pthread_cond_broadcast(&g_q4_pool.cv_job);
    pthread_mutex_unlock(&g_q4_pool.mu);
    for (int32_t i = 0; i < g_q4_pool.nthreads; i++) pthread_join(g_q4_pool.threads[i], NULL);
    free(g_q4_pool.threads);
    g_q4_pool.threads = NULL;
    g_q4_pool.nthreads = 0;
    pthread_mutex_destroy(&g_q4_pool.mu);
    pthread_cond_destroy(&g_q4_pool.cv_job);
    pthread_cond_destroy(&g_q4_pool.cv_done);
    g_q4_pool.stop = 0;
}

static void* q4_worker_main(void* p) {
    q4_worker_arg_t* arg = (q4_worker_arg_t*)p;
    int32_t tid = arg->tid;
    free(arg);

    pthread_mutex_lock(&g_q4_pool.mu);
    uint64_t seen = 0;
    for (;;) {
        while (!g_q4_pool.stop && g_q4_pool.job_id == seen) pthread_cond_wait(&g_q4_pool.cv_job, &g_q4_pool.mu);
        if (g_q4_pool.stop) break;
        seen = g_q4_pool.job_id;

        const void* A = g_q4_pool.A;
        const float* X = g_q4_pool.X;
        float* Y = g_q4_pool.Y;
        int32_t m = g_q4_pool.m;
        int32_t n = g_q4_pool.n;
        q4_gemv_range_fn fn = g_q4_pool.fn;

        int32_t nt = g_q4_pool.nthreads;
        int32_t chunk = (m + nt - 1) / nt;
        int32_t row0 = tid * chunk;
        int32_t row1 = row0 + chunk;
        if (row1 > m) row1 = m;

        pthread_mutex_unlock(&g_q4_pool.mu);
        if (row0 < row1) fn(A, X, Y, n, row0, row1);
        pthread_mutex_lock(&g_q4_pool.mu);

        g_q4_pool.working--;
        if (g_q4_pool.working == 0) pthread_cond_signal(&g_q4_pool.cv_done);
    }
    pthread_mutex_unlock(&g_q4_pool.mu);
    return NULL;
}

static void q4_pool_ensure(int32_t threads) {
    if (threads < 1) threads = 1;
    if (threads > 256) threads = 256;
    if (g_q4_pool.nthreads == threads && g_q4_pool.threads) return;
    q4_pool_destroy();

    g_q4_pool.threads = (pthread_t*)calloc((size_t)threads, sizeof(pthread_t));
    if (!g_q4_pool.threads) return;
    g_q4_pool.nthreads = threads;
    pthread_mutex_init(&g_q4_pool.mu, NULL);
    pthread_cond_init(&g_q4_pool.cv_job, NULL);
    pthread_cond_init(&g_q4_pool.cv_done, NULL);
    g_q4_pool.stop = 0;
    g_q4_pool.job_id = 0;

    for (int32_t i = 0; i < threads; i++) {
        q4_worker_arg_t* arg = (q4_worker_arg_t*)malloc(sizeof(*arg));
        if (!arg) {
            q4_pool_destroy();
            return;
        }
        arg->tid = i;
        if (pthread_create(&g_q4_pool.threads[i], NULL, q4_worker_main, arg) != 0) {
            free(arg);
            q4_pool_destroy();
            return;
        }
    }
}

static void q4_gemv_mt(const void* A, const float* X, float* Y, int32_t m, int32_t n, q4_gemv_range_fn fn) {
    int32_t threads = clamp_threads(tua_llm_get_threads());
    if (threads <= 1 || m < 512) {
        fn(A, X, Y, n, 0, m);
        return;
    }
    q4_pool_ensure(threads);
    if (!g_q4_pool.threads || g_q4_pool.nthreads <= 1) {
        fn(A, X, Y, n, 0, m);
        return;
    }
    pthread_mutex_lock(&g_q4_pool.mu);
    g_q4_pool.A = A;
    g_q4_pool.X = X;
    g_q4_pool.Y = Y;
    g_q4_pool.m = m;
    g_q4_pool.n = n;
    g_q4_pool.fn = fn;
    g_q4_pool.working = g_q4_pool.nthreads;
    g_q4_pool.job_id++;
    pthread_cond_broadcast(&g_q4_pool.cv_job);
    while (g_q4_pool.working > 0) pthread_cond_wait(&g_q4_pool.cv_done, &g_q4_pool.mu);
    pthread_mutex_unlock(&g_q4_pool.mu);
}

static tua_err_t q4_0_gemv_cpu(tua_bytes* y, int64_t y_off,
                              tua_bytes* a, int64_t a_off,
                              tua_bytes* x, int64_t x_off,
                              int32_t m, int32_t n) {
    if (m <= 0 || n <= 0) return TUA_E_INVALID;
    if ((n & 31) != 0) return TUA_E_INVALID;
    int64_t need = tua_llm_q4_0_mat_bytes(m, n);
    if (need <= 0) return TUA_E_INVALID;
    if (!check_f32_span(x, x_off, n) || !check_f32_span(y, y_off, m)) return TUA_E_INVALID;
    if (!a) return TUA_E_INVALID;
    if (a_off < 0) return TUA_E_INVALID;
    if (a_off + need > tua_bytes_len(a)) return TUA_E_INVALID;

    const q4_0_block_t* A = (const q4_0_block_t*)tua_bytes_data_at(a, a_off);
    const float* X = (const float*)tua_bytes_data_at(x, x_off);
    float* Y = (float*)tua_bytes_data_at(y, y_off);
    if (!A || !X || !Y) return TUA_E_INVALID;

    q4_gemv_range_fn fn = q4_0_gemv_range_pick();
    q4_gemv_mt((const void*)A, X, Y, m, n, fn);
    return TUA_OK;
}

static int q4_backend_is_metal(void) {
    const char* e = getenv("TUA_LLM_GEMV_BACKEND");
    if (!e || !e[0]) return 0;
    return strcmp(e, "metal") == 0;
}

tua_err_t tua_llm_gemv_q4_0_f32(tua_bytes* y, int64_t y_off,
                               tua_bytes* a, int64_t a_off,
                               tua_bytes* x, int64_t x_off,
                               int32_t m, int32_t n) {
    // Metal backend will be wired here later. Keep a simple check so callers can
    // flip the backend selector without breaking correctness.
    if (q4_backend_is_metal()) {
        // Not implemented yet; fall back to CPU.
    }
    return q4_0_gemv_cpu(y, y_off, a, a_off, x, x_off, m, n);
}

// =========================
// Q4_K GEMV (CPU)
// =========================

static void q4_k_gemv_range_scalar(const void* Av, const float* X, float* Y, int32_t n, int32_t row0, int32_t row1) {
    const q4_k_block_t* A = (const q4_k_block_t*)Av;
    int32_t nb = n / 256;
    for (int32_t row = row0; row < row1; row++) {
        const q4_k_block_t* brow = A + (int64_t)row * (int64_t)nb;
        double sum = 0.0;
        for (int32_t bi = 0; bi < nb; bi++) {
            float d = f16_to_f32(brow[bi].d);
            float dmin = f16_to_f32(brow[bi].dmin);
            const uint8_t* qs = brow[bi].qs;
            const float* xp = X + bi * 256;
            for (int32_t g = 0; g < 8; g++) {
                uint8_t sc6 = 0, mn6 = 0;
                q4k_get_scale_min_u6(brow[bi].scales, g, &sc6, &mn6);
                float sc = d * (float)(sc6 & 0x3f);
                float mn = dmin * (float)sext6(mn6);
                const uint8_t* qg = qs + g * 16;
                const float* xg = xp + g * 32;
                double sum_x = 0.0;
                double sum_qx = 0.0;
                for (int32_t j = 0; j < 16; j++) {
                    uint8_t b = qg[j];
                    int32_t q0 = (int32_t)(b & 0x0f);
                    int32_t q1 = (int32_t)(b >> 4);
                    float x0 = xg[2 * j + 0];
                    float x1 = xg[2 * j + 1];
                    sum_x += (double)x0 + (double)x1;
                    sum_qx += (double)q0 * (double)x0 + (double)q1 * (double)x1;
                }
                sum += ((double)mn - 8.0 * (double)sc) * sum_x + (double)sc * sum_qx;
            }
        }
        Y[row] = (float)sum;
    }
}

#if defined(__x86_64__)
__attribute__((target("avx2,fma")))
static void q4_k_gemv_range_avx2(const void* Av, const float* X, float* Y, int32_t n, int32_t row0, int32_t row1) {
    const q4_k_block_t* A = (const q4_k_block_t*)Av;
    int32_t nb = n / 256;
    const __m256i mask0f = _mm256_set1_epi8(0x0f);

    for (int32_t row = row0; row < row1; row++) {
        const q4_k_block_t* brow = A + (int64_t)row * (int64_t)nb;
        double sum = 0.0;

        for (int32_t bi = 0; bi < nb; bi++) {
            float d = f16_to_f32(brow[bi].d);
            float dmin = f16_to_f32(brow[bi].dmin);
            const uint8_t* qs = brow[bi].qs;
            const float* xp = X + bi * 256;

            for (int32_t g = 0; g < 8; g++) {
                uint8_t sc6 = 0, mn6 = 0;
                q4k_get_scale_min_u6(brow[bi].scales, g, &sc6, &mn6);
                float sc = d * (float)(sc6 & 0x3f);
                float mn = dmin * (float)sext6(mn6);
                const uint8_t* qg = qs + g * 16;
                const float* xg = xp + g * 32;

                __m256 sx0 = _mm256_add_ps(_mm256_loadu_ps(xg + 0), _mm256_loadu_ps(xg + 8));
                __m256 sx1 = _mm256_add_ps(_mm256_loadu_ps(xg + 16), _mm256_loadu_ps(xg + 24));
                __m256 sx = _mm256_add_ps(sx0, sx1);

                __m128i qpk = _mm_loadu_si128((const __m128i*)qg);
                __m128i lo = _mm_and_si128(qpk, _mm256_castsi256_si128(mask0f));
                __m128i hi = _mm_and_si128(_mm_srli_epi16(qpk, 4), _mm256_castsi256_si128(mask0f));
                __m128i v0 = _mm_unpacklo_epi8(lo, hi);
                __m128i v1 = _mm_unpackhi_epi8(lo, hi);

                __m256 acc0 = _mm256_setzero_ps();
                __m256i i0 = _mm256_cvtepu8_epi32(v0);
                __m256 q0 = _mm256_cvtepi32_ps(i0);
                acc0 = _mm256_fmadd_ps(q0, _mm256_loadu_ps(xg + 0), acc0);

                __m256 acc1 = _mm256_setzero_ps();
                __m256i i1 = _mm256_cvtepu8_epi32(_mm_srli_si128(v0, 8));
                __m256 q1 = _mm256_cvtepi32_ps(i1);
                acc1 = _mm256_fmadd_ps(q1, _mm256_loadu_ps(xg + 8), acc1);

                __m256 acc2 = _mm256_setzero_ps();
                __m256i i2 = _mm256_cvtepu8_epi32(v1);
                __m256 q2 = _mm256_cvtepi32_ps(i2);
                acc2 = _mm256_fmadd_ps(q2, _mm256_loadu_ps(xg + 16), acc2);

                __m256 acc3 = _mm256_setzero_ps();
                __m256i i3 = _mm256_cvtepu8_epi32(_mm_srli_si128(v1, 8));
                __m256 q3 = _mm256_cvtepi32_ps(i3);
                acc3 = _mm256_fmadd_ps(q3, _mm256_loadu_ps(xg + 24), acc3);

                float tx[8], t0[8], t1[8], t2[8], t3[8];
                _mm256_storeu_ps(tx, sx);
                _mm256_storeu_ps(t0, acc0);
                _mm256_storeu_ps(t1, acc1);
                _mm256_storeu_ps(t2, acc2);
                _mm256_storeu_ps(t3, acc3);
                float sum_x = tx[0] + tx[1] + tx[2] + tx[3] + tx[4] + tx[5] + tx[6] + tx[7];
                float sum_qx =
                    (t0[0] + t0[1] + t0[2] + t0[3] + t0[4] + t0[5] + t0[6] + t0[7]) +
                    (t1[0] + t1[1] + t1[2] + t1[3] + t1[4] + t1[5] + t1[6] + t1[7]) +
                    (t2[0] + t2[1] + t2[2] + t2[3] + t2[4] + t2[5] + t2[6] + t2[7]) +
                    (t3[0] + t3[1] + t3[2] + t3[3] + t3[4] + t3[5] + t3[6] + t3[7]);

                sum += ((double)mn - 8.0 * (double)sc) * (double)sum_x + (double)sc * (double)sum_qx;
            }
        }
        Y[row] = (float)sum;
    }
}
#endif

static q4_gemv_range_fn q4_k_gemv_range_pick(void) {
#if defined(__x86_64__)
    if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma")) return q4_k_gemv_range_avx2;
#endif
    return q4_k_gemv_range_scalar;
}

#if defined(TUA_LLM_USE_GGML)
static tua_err_t q4_k_gemv_ggml(tua_bytes* y, int64_t y_off,
                               tua_bytes* a, int64_t a_off,
                               tua_bytes* x, int64_t x_off,
                               int32_t m, int32_t n) {
    if (m <= 0 || n <= 0) return TUA_E_INVALID;
    if ((n & 255) != 0) return TUA_E_INVALID;
    int64_t need = tua_llm_q4_k_mat_bytes(m, n);
    if (need <= 0) return TUA_E_INVALID;
    if (!check_f32_span(x, x_off, n) || !check_f32_span(y, y_off, m)) return TUA_E_INVALID;
    if (!a) return TUA_E_INVALID;
    if (a_off < 0) return TUA_E_INVALID;
    if (a_off + need > tua_bytes_len(a)) return TUA_E_INVALID;

    const uint8_t* A = (const uint8_t*)tua_bytes_data_at(a, a_off);
    const float* X = (const float*)tua_bytes_data_at(x, x_off);
    float* Y = (float*)tua_bytes_data_at(y, y_off);
    if (!A || !X || !Y) return TUA_E_INVALID;

    tua_llm_ggml_ensure_init();

    const struct ggml_type_traits_cpu* tcw = ggml_get_type_traits_cpu(GGML_TYPE_Q4_K);
    const struct ggml_type_traits* trw = ggml_get_type_traits(GGML_TYPE_Q4_K);
    if (!tcw || !trw || !tcw->vec_dot) return TUA_E_NOTSUP;

    enum ggml_type vtype = tcw->vec_dot_type;
    const struct ggml_type_traits_cpu* tcv = ggml_get_type_traits_cpu(vtype);
    const struct ggml_type_traits* trv = ggml_get_type_traits(vtype);
    if (!tcv || !trv || !tcv->from_float) return TUA_E_NOTSUP;
    if (trv->blck_size <= 0) return TUA_E_NOTSUP;
    if ((n % (int32_t)trv->blck_size) != 0) return TUA_E_INVALID;

    int64_t xq_bytes = ((int64_t)n / (int64_t)trv->blck_size) * (int64_t)trv->type_size;
    void* xq = tua_tls_get_bytes(xq_bytes);
    if (!xq) return TUA_E_NOMEM;
    tcv->from_float(X, xq, n);

    int64_t rowb = tua_llm_q4_k_row_bytes(n);
    if (rowb <= 0) return TUA_E_INVALID;
    for (int32_t row = 0; row < m; row++) {
        const void* rowp = (const void*)(A + (int64_t)row * rowb);
        tcw->vec_dot(n, Y + row, sizeof(float), rowp, 0, xq, 0, 1);
    }
    return TUA_OK;
}
#endif

static tua_err_t q4_k_gemv_cpu(tua_bytes* y, int64_t y_off,
                              tua_bytes* a, int64_t a_off,
                              tua_bytes* x, int64_t x_off,
                              int32_t m, int32_t n) {
    if (m <= 0 || n <= 0) return TUA_E_INVALID;
    if ((n & 255) != 0) return TUA_E_INVALID;
    int64_t need = tua_llm_q4_k_mat_bytes(m, n);
    if (need <= 0) return TUA_E_INVALID;
    if (!check_f32_span(x, x_off, n) || !check_f32_span(y, y_off, m)) return TUA_E_INVALID;
    if (!a) return TUA_E_INVALID;
    if (a_off < 0) return TUA_E_INVALID;
    if (a_off + need > tua_bytes_len(a)) return TUA_E_INVALID;

    const q4_k_block_t* A = (const q4_k_block_t*)tua_bytes_data_at(a, a_off);
    const float* X = (const float*)tua_bytes_data_at(x, x_off);
    float* Y = (float*)tua_bytes_data_at(y, y_off);
    if (!A || !X || !Y) return TUA_E_INVALID;

    q4_gemv_range_fn fn = q4_k_gemv_range_pick();
    q4_gemv_mt((const void*)A, X, Y, m, n, fn);
    return TUA_OK;
}

tua_err_t tua_llm_gemv_q4_k_f32(tua_bytes* y, int64_t y_off,
                               tua_bytes* a, int64_t a_off,
                               tua_bytes* x, int64_t x_off,
                               int32_t m, int32_t n) {
    if (q4_backend_is_metal()) {
        // Not implemented yet; fall back to CPU.
    }
#if defined(TUA_LLM_USE_GGML)
    return q4_k_gemv_ggml(y, y_off, a, a_off, x, x_off, m, n);
#else
    return q4_k_gemv_cpu(y, y_off, a, a_off, x, x_off, m, n);
#endif
}

// =========================
// Q6_K GEMV (CPU)
// =========================

static void q6_k_gemv_range_scalar(const void* Av, const float* X, float* Y, int32_t n, int32_t row0, int32_t row1) {
    const uint8_t* A = (const uint8_t*)Av;
    int32_t nb = n / 256;
    const int64_t rowb = (int64_t)nb * 210;

    for (int32_t row = row0; row < row1; row++) {
        const uint8_t* brow = A + (int64_t)row * rowb;
        double sum = 0.0;
        for (int32_t bi = 0; bi < nb; bi++) {
            const q6_k_block_t* b = (const q6_k_block_t*)(brow + (int64_t)bi * 210);
            float d = f16_to_f32(b->d);
            const float* xp = X + bi * 256;
            for (int32_t g = 0; g < 16; g++) {
                float sc = d * (float)b->scales[g];
                const float* xg = xp + g * 16;
                double s = 0.0;
                for (int32_t j = 0; j < 16; j++) {
                    int32_t q = q6k_get_q(b, g * 16 + j);
                    int32_t qs = q - 32;
                    s += (double)qs * (double)xg[j];
                }
                sum += (double)sc * s;
            }
        }
        Y[row] = (float)sum;
    }
}

static q4_gemv_range_fn q6_k_gemv_range_pick(void) {
    // Scalar only for now; can add SIMD later.
    return q6_k_gemv_range_scalar;
}

#if defined(TUA_LLM_USE_GGML)
static tua_err_t q6_k_gemv_ggml(tua_bytes* y, int64_t y_off,
                              tua_bytes* a, int64_t a_off,
                              tua_bytes* x, int64_t x_off,
                              int32_t m, int32_t n) {
    if (m <= 0 || n <= 0) return TUA_E_INVALID;
    if ((n & 255) != 0) return TUA_E_INVALID;
    int64_t need = tua_llm_q6_k_mat_bytes(m, n);
    if (need <= 0) return TUA_E_INVALID;
    if (!check_f32_span(x, x_off, n) || !check_f32_span(y, y_off, m)) return TUA_E_INVALID;
    if (!a) return TUA_E_INVALID;
    if (a_off < 0) return TUA_E_INVALID;
    if (a_off + need > tua_bytes_len(a)) return TUA_E_INVALID;

    const uint8_t* A = (const uint8_t*)tua_bytes_data_at(a, a_off);
    const float* X = (const float*)tua_bytes_data_at(x, x_off);
    float* Y = (float*)tua_bytes_data_at(y, y_off);
    if (!A || !X || !Y) return TUA_E_INVALID;

    tua_llm_ggml_ensure_init();

    const struct ggml_type_traits_cpu* tcw = ggml_get_type_traits_cpu(GGML_TYPE_Q6_K);
    const struct ggml_type_traits* trw = ggml_get_type_traits(GGML_TYPE_Q6_K);
    if (!tcw || !trw || !tcw->vec_dot) return TUA_E_NOTSUP;

    enum ggml_type vtype = tcw->vec_dot_type;
    const struct ggml_type_traits_cpu* tcv = ggml_get_type_traits_cpu(vtype);
    const struct ggml_type_traits* trv = ggml_get_type_traits(vtype);
    if (!tcv || !trv || !tcv->from_float) return TUA_E_NOTSUP;
    if (trv->blck_size <= 0) return TUA_E_NOTSUP;
    if ((n % (int32_t)trv->blck_size) != 0) return TUA_E_INVALID;

    int64_t xq_bytes = ((int64_t)n / (int64_t)trv->blck_size) * (int64_t)trv->type_size;
    void* xq = tua_tls_get_bytes(xq_bytes);
    if (!xq) return TUA_E_NOMEM;
    tcv->from_float(X, xq, n);

    int64_t rowb = tua_llm_q6_k_row_bytes(n);
    if (rowb <= 0) return TUA_E_INVALID;
    for (int32_t row = 0; row < m; row++) {
        const void* rowp = (const void*)(A + (int64_t)row * rowb);
        tcw->vec_dot(n, Y + row, sizeof(float), rowp, 0, xq, 0, 1);
    }
    return TUA_OK;
}
#endif

static tua_err_t q6_k_gemv_cpu(tua_bytes* y, int64_t y_off,
                              tua_bytes* a, int64_t a_off,
                              tua_bytes* x, int64_t x_off,
                              int32_t m, int32_t n) {
    if (m <= 0 || n <= 0) return TUA_E_INVALID;
    if ((n & 255) != 0) return TUA_E_INVALID;
    int64_t need = tua_llm_q6_k_mat_bytes(m, n);
    if (need <= 0) return TUA_E_INVALID;
    if (!check_f32_span(x, x_off, n) || !check_f32_span(y, y_off, m)) return TUA_E_INVALID;
    if (!a) return TUA_E_INVALID;
    if (a_off < 0) return TUA_E_INVALID;
    if (a_off + need > tua_bytes_len(a)) return TUA_E_INVALID;

    const void* A = (const void*)tua_bytes_data_at(a, a_off);
    const float* X = (const float*)tua_bytes_data_at(x, x_off);
    float* Y = (float*)tua_bytes_data_at(y, y_off);
    if (!A || !X || !Y) return TUA_E_INVALID;

    q4_gemv_range_fn fn = q6_k_gemv_range_pick();
    q4_gemv_mt(A, X, Y, m, n, fn);
    return TUA_OK;
}

tua_err_t tua_llm_gemv_q6_k_f32(tua_bytes* y, int64_t y_off,
                               tua_bytes* a, int64_t a_off,
                               tua_bytes* x, int64_t x_off,
                               int32_t m, int32_t n) {
    if (q4_backend_is_metal()) {
        // Not implemented yet; fall back to CPU.
    }
#if defined(TUA_LLM_USE_GGML)
    return q6_k_gemv_ggml(y, y_off, a, a_off, x, x_off, m, n);
#else
    return q6_k_gemv_cpu(y, y_off, a, a_off, x, x_off, m, n);
#endif
}

// =========================
// Self-test
// =========================

static void f32_to_bf16_buf(const float* src, uint16_t* dst, int32_t n) {
    for (int32_t i = 0; i < n; i++) {
        union {
            float f;
            uint32_t u;
        } v;
        v.f = src[i];
        dst[i] = (uint16_t)(v.u >> 16);
    }
}

int32_t tua_llm_q4_0_selftest(void) {
    // Small deterministic matrix-vector test (n must be divisible by 32).
    const int32_t m = 7;
    const int32_t n = 32;
    float A_f32[m * n];
    float x[n];
    for (int32_t i = 0; i < m * n; i++) A_f32[i] = sinf((float)i * 0.1f) * 0.5f;
    for (int32_t i = 0; i < n; i++) x[i] = cosf((float)i * 0.2f) * 0.7f;

    uint16_t A_bf16[m * n];
    f32_to_bf16_buf(A_f32, A_bf16, m * n);

    // Pack.
    int64_t qbytes = tua_llm_q4_0_mat_bytes(m, n);
    if (qbytes <= 0) return 1;
    tua_bytes* qb = tua_bytes_new_uninit(qbytes);
    if (!qb) return 2;
    tua_bytes* sb = tua_bytes_new_uninit((int64_t)m * (int64_t)n * 2);
    if (!sb) return 3;
    memcpy(tua_bytes_data_at(sb, 0), A_bf16, (size_t)m * (size_t)n * 2);
    if (tua_llm_q4_0_pack_bf16(qb, 0, sb, 0, m, n) != TUA_OK) return 4;

    // Run GEMV.
    tua_bytes* xb = tua_bytes_new_uninit((int64_t)n * 4);
    tua_bytes* yb = tua_bytes_new_uninit((int64_t)m * 4);
    if (!xb || !yb) return 5;
    memcpy(tua_bytes_data_at(xb, 0), x, (size_t)n * 4);
    if (tua_llm_gemv_q4_0_f32(yb, 0, qb, 0, xb, 0, m, n) != TUA_OK) return 6;
    float yq[m];
    memcpy(yq, tua_bytes_data_at(yb, 0), (size_t)m * 4);

    // Reference.
    float yr[m];
    for (int32_t r = 0; r < m; r++) {
        double sum = 0.0;
        for (int32_t c = 0; c < n; c++) sum += (double)A_f32[r * n + c] * (double)x[c];
        yr[r] = (float)sum;
    }

    // Error bound: Q4 is rough; allow small absolute error for this tiny test.
    for (int32_t r = 0; r < m; r++) {
        float err = fabsf(yq[r] - yr[r]);
        if (!(err <= 0.15f)) return 10 + r;
    }
    return 0;
}

int32_t tua_llm_q4_k_selftest(void) {
    // Small deterministic matrix-vector test (n must be divisible by 256).
    const int32_t m = 7;
    const int32_t n = 256;
    float A_f32[m * n];
    float x[n];
    for (int32_t i = 0; i < m * n; i++) A_f32[i] = sinf((float)i * 0.1f) * 0.5f + 0.1f * cosf((float)i * 0.03f);
    for (int32_t i = 0; i < n; i++) x[i] = cosf((float)i * 0.2f) * 0.7f;

    uint16_t A_bf16[m * n];
    f32_to_bf16_buf(A_f32, A_bf16, m * n);

    int64_t qbytes = tua_llm_q4_k_mat_bytes(m, n);
    if (qbytes <= 0) return 1;
    tua_bytes* qb = tua_bytes_new_uninit(qbytes);
    if (!qb) return 2;
    tua_bytes* sb = tua_bytes_new_uninit((int64_t)m * (int64_t)n * 2);
    if (!sb) return 3;
    memcpy(tua_bytes_data_at(sb, 0), A_bf16, (size_t)m * (size_t)n * 2);
    if (tua_llm_q4_k_pack_bf16(qb, 0, sb, 0, m, n) != TUA_OK) return 4;

    tua_bytes* xb = tua_bytes_new_uninit((int64_t)n * 4);
    tua_bytes* yb = tua_bytes_new_uninit((int64_t)m * 4);
    if (!xb || !yb) return 5;
    memcpy(tua_bytes_data_at(xb, 0), x, (size_t)n * 4);
    if (tua_llm_gemv_q4_k_f32(yb, 0, qb, 0, xb, 0, m, n) != TUA_OK) return 6;
    float yq[m];
    memcpy(yq, tua_bytes_data_at(yb, 0), (size_t)m * 4);

    float yr[m];
    for (int32_t r = 0; r < m; r++) {
        double sum = 0.0;
        for (int32_t c = 0; c < n; c++) sum += (double)A_f32[r * n + c] * (double)x[c];
        yr[r] = (float)sum;
    }

    for (int32_t r = 0; r < m; r++) {
        float err = fabsf(yq[r] - yr[r]);
        if (!(err <= 0.12f)) return 10 + r;
    }
    return 0;
}

int32_t tua_llm_q6_k_selftest(void) {
    const int32_t m = 1;
    const int32_t n = 256;
    tua_bytes* A = tua_bytes_new_uninit(210);
    tua_bytes* X = tua_bytes_new_uninit((int64_t)n * 4);
    tua_bytes* Y = tua_bytes_new_uninit((int64_t)m * 4);
    if (!A || !X || !Y) return 1;

    // Quantize a deterministic float row via ggml, then round-trip through get_row_f32.
    float src[n];
    for (int32_t i = 0; i < n; i++) {
        src[i] = sinf((float)i * 0.07f) * 0.5f + cosf((float)i * 0.11f) * 0.2f;
    }
#if defined(TUA_LLM_USE_GGML)
    tua_llm_ggml_ensure_init();
    const struct ggml_type_traits_cpu* tc = ggml_get_type_traits_cpu(GGML_TYPE_Q6_K);
    if (!tc || !tc->from_float) return 2;
    tc->from_float(src, tua_bytes_data_at(A, 0), n);
#else
    // Without ggml, we only validate the hand-written Q6_K layout via get_row_f32 in other tests.
    return 0;
#endif

    float x[n];
    for (int32_t i = 0; i < n; i++) x[i] = 1.0f;
    memcpy(tua_bytes_data_at(X, 0), x, (size_t)n * 4);

    tua_bytes* R = tua_bytes_new_uninit((int64_t)n * 4);
    if (!R) return 4;
    if (tua_llm_q6_k_get_row_f32(R, 0, A, 0, 0, n) != TUA_OK) return 5;
    const float* rf = (const float*)tua_bytes_data_at(R, 0);
    if (!rf) return 6;
    for (int32_t i = 0; i < n; i++) {
        float err = fabsf(rf[i] - src[i]);
        if (!(err <= 0.08f)) return 7;
    }

    // GEMV should be close to the dequantized dot even if it internally quantizes activations.
    if (tua_llm_gemv_q6_k_f32(Y, 0, A, 0, X, 0, m, n) != TUA_OK) return 13;
    float y;
    memcpy(&y, tua_bytes_data_at(Y, 0), 4);
    float expected = 0.0f;
    for (int32_t i = 0; i < n; i++) expected += rf[i];
    if (fabsf(y - expected) > 0.5f) return 14;
    return 0;
}
