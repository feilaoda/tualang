#include "tua_llm.h"

#include "tua_array.h"
#include "tua_map.h"

#include <math.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#if defined(__APPLE__)
#include <Accelerate/Accelerate.h>
#include <vecLib/BNNS/bnns.h>
#endif

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

static int32_t g_tua_llm_threads = 0;

static int32_t clamp_threads(int32_t n) {
    if (n <= 0) {
        long c = sysconf(_SC_NPROCESSORS_ONLN);
        if (c > 0 && c < INT32_MAX) return (int32_t)c;
        return 1;
    }
    if (n > 256) n = 256;
    return n;
}

tua_err_t tua_llm_set_threads(int32_t n) {
    if (n < 0) n = 0;
    g_tua_llm_threads = n;
#if defined(__APPLE__)
    // Accelerate reads these env vars to size its internal thread pool.
    // Setting them here allows `--threads N` to take effect without having to wrap every kernel ourselves.
    if (n > 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", (int)n);
        setenv("VECLIB_MAXIMUM_THREADS", buf, 1);
        setenv("VECLIB_MINIMUM_THREADS", buf, 1);
        setenv("BLAS_NUM_THREADS", buf, 1);
        setenv("OMP_NUM_THREADS", buf, 1);
    }
#endif
    return TUA_OK;
}

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

static int check_bf16_span(tua_bytes* b, int64_t off, int64_t n_elts) {
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
            exp = 1;
            while ((mant & 0x0400u) == 0) {
                mant <<= 1;
                exp--;
            }
            mant &= 0x03ffu;
            uint32_t exp32 = (exp + (127 - 15)) & 0xffu;
            out = sign | (exp32 << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        out = sign | 0x7f800000u | (mant << 13);
    } else {
        uint32_t exp32 = (exp + (127 - 15)) & 0xffu;
        out = sign | (exp32 << 23) | (mant << 13);
    }
    float f;
    memcpy(&f, &out, sizeof(f));
    return f;
}

static inline void bf16_to_f32_buf(const uint16_t* src, float* dst, int32_t n) {
    if (!src || !dst || n <= 0) return;
#if defined(__ARM_NEON)
    int32_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint16x8_t a16 = vld1q_u16(src + i);
        uint32x4_t lo = vshll_n_u16(vget_low_u16(a16), 16);
        uint32x4_t hi = vshll_n_u16(vget_high_u16(a16), 16);
        vst1q_f32(dst + i, vreinterpretq_f32_u32(lo));
        vst1q_f32(dst + i + 4, vreinterpretq_f32_u32(hi));
    }
    for (; i < n; i++) dst[i] = bf16_to_f32(src[i]);
#else
    for (int32_t i = 0; i < n; i++) dst[i] = bf16_to_f32(src[i]);
#endif
}

static inline void f16_to_f32_buf(const uint16_t* src, float* dst, int32_t n) {
    if (!src || !dst || n <= 0) return;
#if defined(__ARM_NEON) && defined(__aarch64__) && defined(__ARM_FEATURE_FP16_VECTOR_ARITHMETIC)
    int32_t i = 0;
    for (; i + 8 <= n; i += 8) {
        float16x8_t h = vreinterpretq_f16_u16(vld1q_u16(src + i));
        float32x4_t lo = vcvt_f32_f16(vget_low_f16(h));
        float32x4_t hi = vcvt_f32_f16(vget_high_f16(h));
        vst1q_f32(dst + i, lo);
        vst1q_f32(dst + i + 4, hi);
    }
    for (; i < n; i++) dst[i] = f16_to_f32(src[i]);
#else
    for (int32_t i = 0; i < n; i++) dst[i] = f16_to_f32(src[i]);
#endif
}

static inline float dot_bf16_f32(const uint16_t* a, const float* x, int32_t n) {
    if (!a || !x || n <= 0) return 0.0f;
#if defined(__ARM_NEON)
    float32x4_t acc0 = vdupq_n_f32(0.0f);
    float32x4_t acc1 = vdupq_n_f32(0.0f);
    int32_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint16x8_t a16 = vld1q_u16(a + i);
        uint32x4_t lo = vshll_n_u16(vget_low_u16(a16), 16);
        uint32x4_t hi = vshll_n_u16(vget_high_u16(a16), 16);
        float32x4_t af0 = vreinterpretq_f32_u32(lo);
        float32x4_t af1 = vreinterpretq_f32_u32(hi);
        float32x4_t xf0 = vld1q_f32(x + i);
        float32x4_t xf1 = vld1q_f32(x + i + 4);
#if defined(__aarch64__)
        acc0 = vfmaq_f32(acc0, af0, xf0);
        acc1 = vfmaq_f32(acc1, af1, xf1);
#else
        acc0 = vmlaq_f32(acc0, af0, xf0);
        acc1 = vmlaq_f32(acc1, af1, xf1);
#endif
    }
    float32x4_t acc = vaddq_f32(acc0, acc1);
    float32x2_t sum2 = vadd_f32(vget_low_f32(acc), vget_high_f32(acc));
    float sum = vget_lane_f32(sum2, 0) + vget_lane_f32(sum2, 1);
    for (; i < n; i++) sum += bf16_to_f32(a[i]) * x[i];
    return sum;
#else
    double sum = 0.0;
    for (int32_t i = 0; i < n; i++) sum += (double)bf16_to_f32(a[i]) * (double)x[i];
    return (float)sum;
#endif
}

static inline float dot_f16_f32(const uint16_t* a, const float* x, int32_t n) {
    if (!a || !x || n <= 0) return 0.0f;
#if defined(__ARM_NEON) && defined(__aarch64__) && defined(__ARM_FEATURE_FP16_VECTOR_ARITHMETIC)
    float32x4_t acc0 = vdupq_n_f32(0.0f);
    float32x4_t acc1 = vdupq_n_f32(0.0f);
    int32_t i = 0;
    for (; i + 8 <= n; i += 8) {
        float16x8_t h = vreinterpretq_f16_u16(vld1q_u16(a + i));
        float32x4_t af0 = vcvt_f32_f16(vget_low_f16(h));
        float32x4_t af1 = vcvt_f32_f16(vget_high_f16(h));
        float32x4_t xf0 = vld1q_f32(x + i);
        float32x4_t xf1 = vld1q_f32(x + i + 4);
        acc0 = vfmaq_f32(acc0, af0, xf0);
        acc1 = vfmaq_f32(acc1, af1, xf1);
    }
    float32x4_t acc = vaddq_f32(acc0, acc1);
    float32x2_t sum2 = vadd_f32(vget_low_f32(acc), vget_high_f32(acc));
    float sum = vget_lane_f32(sum2, 0) + vget_lane_f32(sum2, 1);
    for (; i < n; i++) sum += f16_to_f32(a[i]) * x[i];
    return sum;
#else
    double sum = 0.0;
    for (int32_t i = 0; i < n; i++) sum += (double)f16_to_f32(a[i]) * (double)x[i];
    return (float)sum;
#endif
}

static void bf16_gemv_range(const uint16_t* A, const float* X, float* Y, int32_t n, int32_t row0, int32_t row1) {
    for (int32_t r = row0; r < row1; r++) {
        const uint16_t* row = A + (int64_t)r * (int64_t)n;
        Y[r] = dot_bf16_f32(row, X, n);
    }
}

static void f16_gemv_range(const uint16_t* A, const float* X, float* Y, int32_t n, int32_t row0, int32_t row1) {
    for (int32_t r = row0; r < row1; r++) {
        const uint16_t* row = A + (int64_t)r * (int64_t)n;
        Y[r] = dot_f16_f32(row, X, n);
    }
}

#if defined(__APPLE__)
typedef struct {
    const void* w;
    int32_t m;
    int32_t n;
    uint32_t wtype; // BNNSDataType
    BNNSFilter filter;
} bnns_fc_entry_t;

static bnns_fc_entry_t* g_bnns_fc = NULL;
static size_t g_bnns_fc_cap = 0;
static size_t g_bnns_fc_len = 0;
static pthread_mutex_t g_bnns_fc_mu = PTHREAD_MUTEX_INITIALIZER;

static uint64_t bnns_fc_hash(const void* w, int32_t m, int32_t n, uint32_t wtype) {
    uint64_t x = (uint64_t)(uintptr_t)w;
    x ^= (uint64_t)(uint32_t)m * 0x9e3779b97f4a7c15ull;
    x ^= (uint64_t)(uint32_t)n * 0xbf58476d1ce4e5b9ull;
    x ^= (uint64_t)wtype * 0x94d049bb133111ebull;
    // mix
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ull;
    x ^= x >> 33;
    return x;
}

static BNNSFilter bnns_fc_create(const void* w, int32_t m, int32_t n, BNNSDataType wtype) {
    BNNSLayerParametersFullyConnected lp;
    memset(&lp, 0, sizeof(lp));

    lp.i_desc.layout = BNNSDataLayoutVector;
    lp.i_desc.size[0] = (size_t)n;
    lp.i_desc.stride[0] = 1;
    lp.i_desc.data = NULL;
    lp.i_desc.data_type = BNNSDataTypeFloat32;

    lp.w_desc.layout = BNNSDataLayoutRowMajorMatrix;
    lp.w_desc.size[0] = (size_t)n; // columns
    lp.w_desc.size[1] = (size_t)m; // rows
    lp.w_desc.stride[0] = 1;
    lp.w_desc.stride[1] = (size_t)n;
    lp.w_desc.data = (void*)w;
    lp.w_desc.data_type = wtype;

    lp.o_desc.layout = BNNSDataLayoutVector;
    lp.o_desc.size[0] = (size_t)m;
    lp.o_desc.stride[0] = 1;
    lp.o_desc.data = NULL;
    lp.o_desc.data_type = BNNSDataTypeFloat32;

    // Bias is optional; leave data NULL.
    lp.bias.layout = BNNSDataLayoutVector;
    lp.bias.size[0] = (size_t)m;
    lp.bias.stride[0] = 1;
    lp.bias.data = NULL;
    lp.bias.data_type = BNNSDataTypeFloat32;

    lp.activation.function = BNNSActivationFunctionIdentity;
    return BNNSFilterCreateLayerFullyConnected(&lp, NULL);
}

static BNNSFilter bnns_fc_get(const void* w, int32_t m, int32_t n, BNNSDataType wtype) {
    if (!w || m <= 0 || n <= 0) return NULL;
    pthread_mutex_lock(&g_bnns_fc_mu);
    if (g_bnns_fc_cap == 0) {
        g_bnns_fc_cap = 1024;
        g_bnns_fc = (bnns_fc_entry_t*)calloc(g_bnns_fc_cap, sizeof(*g_bnns_fc));
        if (!g_bnns_fc) {
            g_bnns_fc_cap = 0;
            pthread_mutex_unlock(&g_bnns_fc_mu);
            return NULL;
        }
    }
    uint64_t h = bnns_fc_hash(w, m, n, (uint32_t)wtype);
    size_t mask = g_bnns_fc_cap - 1;
    size_t idx = (size_t)h & mask;
    for (;;) {
        bnns_fc_entry_t* e = &g_bnns_fc[idx];
        if (!e->w) break;
        if (e->w == w && e->m == m && e->n == n && e->wtype == (uint32_t)wtype) {
            BNNSFilter f = e->filter;
            pthread_mutex_unlock(&g_bnns_fc_mu);
            return f;
        }
        idx = (idx + 1) & mask;
    }

    // Insert new.
    BNNSFilter f = bnns_fc_create(w, m, n, wtype);
    if (f) {
        g_bnns_fc[idx].w = w;
        g_bnns_fc[idx].m = m;
        g_bnns_fc[idx].n = n;
        g_bnns_fc[idx].wtype = (uint32_t)wtype;
        g_bnns_fc[idx].filter = f;
        g_bnns_fc_len++;
    }
    pthread_mutex_unlock(&g_bnns_fc_mu);
    return f;
}
#endif

#if !defined(__APPLE__)
typedef struct {
    pthread_t* threads;
    int32_t nthreads;
    pthread_mutex_t mu;
    pthread_cond_t cv_job;
    pthread_cond_t cv_done;
    int32_t stop;
    int32_t job_id;
    int32_t working;

    const uint16_t* A;
    const float* X;
    float* Y;
    int32_t m;
    int32_t n;
} bf16_gemv_pool_t;

static bf16_gemv_pool_t g_bf16_pool = {0};

typedef struct {
    int32_t tid;
} bf16_worker_arg_t;

static void* bf16_worker_main(void* p) {
    bf16_worker_arg_t* arg = (bf16_worker_arg_t*)p;
    int32_t tid = arg ? arg->tid : 0;
    free(arg);

    int32_t last_job = 0;
    for (;;) {
        pthread_mutex_lock(&g_bf16_pool.mu);
        while (!g_bf16_pool.stop && g_bf16_pool.job_id == last_job) {
            pthread_cond_wait(&g_bf16_pool.cv_job, &g_bf16_pool.mu);
        }
        if (g_bf16_pool.stop) {
            pthread_mutex_unlock(&g_bf16_pool.mu);
            return NULL;
        }
        last_job = g_bf16_pool.job_id;
        const uint16_t* A = g_bf16_pool.A;
        const float* X = g_bf16_pool.X;
        float* Y = g_bf16_pool.Y;
        int32_t m = g_bf16_pool.m;
        int32_t n = g_bf16_pool.n;
        int32_t nt = g_bf16_pool.nthreads;
        pthread_mutex_unlock(&g_bf16_pool.mu);

        int32_t row0 = (int32_t)((int64_t)m * (int64_t)tid / (int64_t)nt);
        int32_t row1 = (int32_t)((int64_t)m * (int64_t)(tid + 1) / (int64_t)nt);
        bf16_gemv_range(A, X, Y, n, row0, row1);

        pthread_mutex_lock(&g_bf16_pool.mu);
        g_bf16_pool.working--;
        if (g_bf16_pool.working <= 0) {
            pthread_cond_signal(&g_bf16_pool.cv_done);
        }
        pthread_mutex_unlock(&g_bf16_pool.mu);
    }
}

static void bf16_pool_destroy(void) {
    if (g_bf16_pool.nthreads <= 0) return;
    pthread_mutex_lock(&g_bf16_pool.mu);
    g_bf16_pool.stop = 1;
    pthread_cond_broadcast(&g_bf16_pool.cv_job);
    pthread_mutex_unlock(&g_bf16_pool.mu);
    for (int32_t i = 0; i < g_bf16_pool.nthreads; i++) {
        pthread_join(g_bf16_pool.threads[i], NULL);
    }
    free(g_bf16_pool.threads);
    g_bf16_pool.threads = NULL;
    g_bf16_pool.nthreads = 0;
    pthread_mutex_destroy(&g_bf16_pool.mu);
    pthread_cond_destroy(&g_bf16_pool.cv_job);
    pthread_cond_destroy(&g_bf16_pool.cv_done);
    g_bf16_pool.stop = 0;
    g_bf16_pool.job_id = 0;
    g_bf16_pool.working = 0;
}

static void bf16_pool_ensure(int32_t want_threads) {
    int32_t want = clamp_threads(want_threads);
    if (want <= 1) {
        bf16_pool_destroy();
        return;
    }
    if (g_bf16_pool.nthreads == want) return;
    bf16_pool_destroy();

    g_bf16_pool.threads = (pthread_t*)calloc((size_t)want, sizeof(pthread_t));
    if (!g_bf16_pool.threads) return;
    g_bf16_pool.nthreads = want;
    pthread_mutex_init(&g_bf16_pool.mu, NULL);
    pthread_cond_init(&g_bf16_pool.cv_job, NULL);
    pthread_cond_init(&g_bf16_pool.cv_done, NULL);
    g_bf16_pool.stop = 0;
    g_bf16_pool.job_id = 0;
    g_bf16_pool.working = 0;

    for (int32_t i = 0; i < want; i++) {
        bf16_worker_arg_t* arg = (bf16_worker_arg_t*)malloc(sizeof(*arg));
        if (!arg) {
            bf16_pool_destroy();
            return;
        }
        arg->tid = i;
        if (pthread_create(&g_bf16_pool.threads[i], NULL, bf16_worker_main, arg) != 0) {
            free(arg);
            bf16_pool_destroy();
            return;
        }
    }
}
#endif

#if defined(__APPLE__)
typedef struct {
    float* buf;
    int64_t cap_floats;
} bf16_sgemm_scratch_t;

static __thread bf16_sgemm_scratch_t g_bf16_scratch = {0};

static float* bf16_scratch_ensure(int64_t need_floats) {
    if (need_floats <= 0) return NULL;
    if (g_bf16_scratch.cap_floats >= need_floats && g_bf16_scratch.buf) return g_bf16_scratch.buf;
    size_t bytes = (size_t)need_floats * sizeof(float);
    float* p = (float*)realloc(g_bf16_scratch.buf, bytes);
    if (!p) return NULL;
    g_bf16_scratch.buf = p;
    g_bf16_scratch.cap_floats = need_floats;
    return p;
}
#endif

tua_err_t tua_llm_gemv_bf16_f32(tua_bytes* y, int64_t y_off,
                               tua_bytes* a, int64_t a_off,
                               tua_bytes* x, int64_t x_off,
                               int32_t m, int32_t n) {
    if (m <= 0 || n <= 0) return TUA_E_INVALID;
    int64_t mn = (int64_t)m * (int64_t)n;
    if (mn <= 0) return TUA_E_INVALID;
    if (!check_bf16_span(a, a_off, mn)) return TUA_E_INVALID;
    if (!check_f32_span(x, x_off, n)) return TUA_E_INVALID;
    if (!check_f32_span(y, y_off, m)) return TUA_E_INVALID;

    const uint16_t* A = (const uint16_t*)tua_bytes_data_at(a, a_off);
    const float* X = (const float*)tua_bytes_data_at(x, x_off);
    float* Y = (float*)tua_bytes_data_at(y, y_off);
    if (!A || !X || !Y) return TUA_E_INVALID;

#if defined(__APPLE__)
    BNNSFilter f = bnns_fc_get(A, m, n, BNNSDataTypeBFloat16);
    if (f && BNNSFilterApply(f, X, Y) == 0) return TUA_OK;

    // Use Accelerate for the math and pay only the BF16->F32 conversion cost.
    // This is significantly faster than scalar dot-products on non-ARM builds.
    int32_t block = 256;
    if (n <= 1024) block = 1024;
    if (block > m) block = m;
    for (int32_t row = 0; row < m; row += block) {
        int32_t mb = m - row;
        if (mb > block) mb = block;
        int64_t need = (int64_t)mb * (int64_t)n;
        float* tmp = bf16_scratch_ensure(need);
        if (!tmp) return TUA_E_NOMEM;
        const uint16_t* Ab = A + (int64_t)row * (int64_t)n;
        for (int32_t r = 0; r < mb; r++) {
            bf16_to_f32_buf(Ab + (int64_t)r * (int64_t)n, tmp + (int64_t)r * (int64_t)n, n);
        }
        float* Yb = Y + row;
        cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans, mb, 1, n, 1.0f, tmp, n, X, 1, 0.0f, Yb, 1);
    }
    return TUA_OK;
#else
    int32_t threads = clamp_threads(g_tua_llm_threads);
    if (threads <= 1 || m < 1024) {
        bf16_gemv_range(A, X, Y, n, 0, m);
        return TUA_OK;
    }

    bf16_pool_ensure(threads);
    if (g_bf16_pool.nthreads <= 1) {
        bf16_gemv_range(A, X, Y, n, 0, m);
        return TUA_OK;
    }

    pthread_mutex_lock(&g_bf16_pool.mu);
    g_bf16_pool.A = A;
    g_bf16_pool.X = X;
    g_bf16_pool.Y = Y;
    g_bf16_pool.m = m;
    g_bf16_pool.n = n;
    g_bf16_pool.working = g_bf16_pool.nthreads;
    g_bf16_pool.job_id++;
    pthread_cond_broadcast(&g_bf16_pool.cv_job);
    while (g_bf16_pool.working > 0) {
        pthread_cond_wait(&g_bf16_pool.cv_done, &g_bf16_pool.mu);
    }
    pthread_mutex_unlock(&g_bf16_pool.mu);
    return TUA_OK;
#endif
}

tua_err_t tua_llm_gemv_f16_f32(tua_bytes* y, int64_t y_off,
                              tua_bytes* a, int64_t a_off,
                              tua_bytes* x, int64_t x_off,
                              int32_t m, int32_t n) {
    if (m <= 0 || n <= 0) return TUA_E_INVALID;
    int64_t mn = (int64_t)m * (int64_t)n;
    if (mn <= 0) return TUA_E_INVALID;
    if (!check_bf16_span(a, a_off, mn)) return TUA_E_INVALID; // same bounds as u16 buffer
    if (!check_f32_span(x, x_off, n)) return TUA_E_INVALID;
    if (!check_f32_span(y, y_off, m)) return TUA_E_INVALID;

    const uint16_t* A = (const uint16_t*)tua_bytes_data_at(a, a_off);
    const float* X = (const float*)tua_bytes_data_at(x, x_off);
    float* Y = (float*)tua_bytes_data_at(y, y_off);
    if (!A || !X || !Y) return TUA_E_INVALID;

#if defined(__APPLE__)
    BNNSFilter f = bnns_fc_get(A, m, n, BNNSDataTypeFloat16);
    if (f && BNNSFilterApply(f, X, Y) == 0) return TUA_OK;

    int32_t block = 256;
    if (n <= 1024) block = 1024;
    if (block > m) block = m;
    for (int32_t row = 0; row < m; row += block) {
        int32_t mb = m - row;
        if (mb > block) mb = block;
        int64_t need = (int64_t)mb * (int64_t)n;
        float* tmp = bf16_scratch_ensure(need);
        if (!tmp) return TUA_E_NOMEM;
        const uint16_t* Ab = A + (int64_t)row * (int64_t)n;
        for (int32_t r = 0; r < mb; r++) {
            f16_to_f32_buf(Ab + (int64_t)r * (int64_t)n, tmp + (int64_t)r * (int64_t)n, n);
        }
        float* Yb = Y + row;
        cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans, mb, 1, n, 1.0f, tmp, n, X, 1, 0.0f, Yb, 1);
    }
    return TUA_OK;
#else
    int32_t threads = clamp_threads(g_tua_llm_threads);
    if (threads <= 1 || m < 1024) {
        f16_gemv_range(A, X, Y, n, 0, m);
        return TUA_OK;
    }
    // No FP16 worker pool yet; fall back to single-threaded range.
    f16_gemv_range(A, X, Y, n, 0, m);
    return TUA_OK;
#endif
}

tua_err_t tua_llm_rmsnorm_f32_bf16w(tua_bytes* out, int64_t out_off, tua_bytes* x, int64_t x_off,
                                   tua_bytes* w_bf16, int64_t w_off, int32_t n, float eps) {
    if (n <= 0) return TUA_E_INVALID;
    if (!check_f32_span(out, out_off, n) || !check_f32_span(x, x_off, n) || !check_bf16_span(w_bf16, w_off, n)) {
        return TUA_E_INVALID;
    }
    float* y = f32p(out, out_off);
    const float* xv = f32p(x, x_off);
    const uint16_t* wt = (const uint16_t*)tua_bytes_data_at(w_bf16, w_off);
    double ss = 0.0;
    for (int32_t i = 0; i < n; i++) {
        double v = (double)xv[i];
        ss += v * v;
    }
    float mean = (float)(ss / (double)n);
    float inv = 1.0f / sqrtf(mean + eps);
    for (int32_t i = 0; i < n; i++) y[i] = xv[i] * inv * bf16_to_f32(wt[i]);
    return TUA_OK;
}

tua_err_t tua_llm_rmsnorm_f32_f16w(tua_bytes* out, int64_t out_off, tua_bytes* x, int64_t x_off,
                                  tua_bytes* w_f16, int64_t w_off, int32_t n, float eps) {
    if (n <= 0) return TUA_E_INVALID;
    if (!check_f32_span(out, out_off, n) || !check_f32_span(x, x_off, n) || !check_bf16_span(w_f16, w_off, n)) {
        return TUA_E_INVALID;
    }
    float* y = f32p(out, out_off);
    const float* xv = f32p(x, x_off);
    const uint16_t* wt = (const uint16_t*)tua_bytes_data_at(w_f16, w_off);
    double ss = 0.0;
    for (int32_t i = 0; i < n; i++) {
        double v = (double)xv[i];
        ss += v * v;
    }
    float mean = (float)(ss / (double)n);
    float inv = 1.0f / sqrtf(mean + eps);
    for (int32_t i = 0; i < n; i++) y[i] = xv[i] * inv * f16_to_f32(wt[i]);
    return TUA_OK;
}

tua_err_t tua_llm_qk_rmsnorm_inplace_f32_bf16w(tua_bytes* x, int64_t x_off, int32_t n_heads, int32_t head_dim,
                                              tua_bytes* w_bf16, int64_t w_off, float eps) {
    if (n_heads <= 0 || head_dim <= 0) return TUA_E_INVALID;
    int64_t total = (int64_t)n_heads * (int64_t)head_dim;
    if (!check_f32_span(x, x_off, total) || !check_bf16_span(w_bf16, w_off, head_dim)) return TUA_E_INVALID;
    float* xv = f32p(x, x_off);
    const uint16_t* wt = (const uint16_t*)tua_bytes_data_at(w_bf16, w_off);
    for (int32_t h = 0; h < n_heads; h++) {
        float* p = xv + (int64_t)h * head_dim;
        double ss = 0.0;
        for (int32_t i = 0; i < head_dim; i++) {
            double v = (double)p[i];
            ss += v * v;
        }
        float mean = (float)(ss / (double)head_dim);
        float inv = 1.0f / sqrtf(mean + eps);
        for (int32_t i = 0; i < head_dim; i++) p[i] = p[i] * inv * bf16_to_f32(wt[i]);
    }
    return TUA_OK;
}

tua_err_t tua_llm_qk_rmsnorm_inplace_f32_f16w(tua_bytes* x, int64_t x_off, int32_t n_heads, int32_t head_dim,
                                             tua_bytes* w_f16, int64_t w_off, float eps) {
    if (n_heads <= 0 || head_dim <= 0) return TUA_E_INVALID;
    int64_t total = (int64_t)n_heads * (int64_t)head_dim;
    if (!check_f32_span(x, x_off, total) || !check_bf16_span(w_f16, w_off, head_dim)) return TUA_E_INVALID;
    float* xv = f32p(x, x_off);
    const uint16_t* wt = (const uint16_t*)tua_bytes_data_at(w_f16, w_off);
    for (int32_t h = 0; h < n_heads; h++) {
        float* p = xv + (int64_t)h * head_dim;
        double ss = 0.0;
        for (int32_t i = 0; i < head_dim; i++) {
            double v = (double)p[i];
            ss += v * v;
        }
        float mean = (float)(ss / (double)head_dim);
        float inv = 1.0f / sqrtf(mean + eps);
        for (int32_t i = 0; i < head_dim; i++) p[i] = p[i] * inv * f16_to_f32(wt[i]);
    }
    return TUA_OK;
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
    // Chunk into row blocks to avoid extreme edge cases, but keep the block large to reduce per-call overhead
    // (important for the huge vocab projection GEMV).
    const int32_t block = (m >= 65536) ? 65536 : m;
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

static inline float dot_bf16_f32_ptr(const float* qh, const uint16_t* kh, int32_t head_dim) {
    float s = 0.0f;
    for (int32_t i = 0; i < head_dim; i++) s += qh[i] * bf16_to_f32(kh[i]);
    return s;
}

static inline float dot_f16_f32_ptr(const float* qh, const uint16_t* kh, int32_t head_dim) {
    float s = 0.0f;
    for (int32_t i = 0; i < head_dim; i++) s += qh[i] * f16_to_f32(kh[i]);
    return s;
}

tua_err_t tua_llm_attn_decode_kv_bf16_f32(tua_bytes* out, int64_t out_off,
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

    if (token_bytes != (int64_t)n_kv_heads * (int64_t)head_dim * 2) return TUA_E_INVALID;

    const float* qv = f32p(q, q_off);
    float* outv = f32p(out, out_off);
    float* scores = f32p(scratch, scratch_off);

    uint8_t* kv_base_ptr = tua_bytes_data(kv);
    if (!kv_base_ptr) return TUA_E_INVALID;

    int32_t group = n_heads / n_kv_heads;
    for (int32_t h = 0; h < n_heads; h++) {
        int32_t kvh = h / group;
        const float* qh = qv + (int64_t)h * head_dim;

        float maxv = -INFINITY;
        for (int32_t t = 0; t <= pos; t++) {
            int64_t koff = k_base + (int64_t)layer * layer_bytes + (int64_t)t * token_bytes + (int64_t)kvh * head_dim * 2;
            const uint16_t* kh = (const uint16_t*)(kv_base_ptr + koff);
            float s = dot_bf16_f32_ptr(qh, kh, head_dim) * scale;
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
            int64_t voff = v_base + (int64_t)layer * layer_bytes + (int64_t)t * token_bytes + (int64_t)kvh * head_dim * 2;
            const uint16_t* vh = (const uint16_t*)(kv_base_ptr + voff);
            for (int32_t i = 0; i < head_dim; i++) oh[i] += p * bf16_to_f32(vh[i]);
        }
    }
    return TUA_OK;
}

tua_err_t tua_llm_attn_decode_kv_f16_f32(tua_bytes* out, int64_t out_off,
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

    if (token_bytes != (int64_t)n_kv_heads * (int64_t)head_dim * 2) return TUA_E_INVALID;

    const float* qv = f32p(q, q_off);
    float* outv = f32p(out, out_off);
    float* scores = f32p(scratch, scratch_off);

    uint8_t* kv_base_ptr = tua_bytes_data(kv);
    if (!kv_base_ptr) return TUA_E_INVALID;

    int32_t group = n_heads / n_kv_heads;
    for (int32_t h = 0; h < n_heads; h++) {
        int32_t kvh = h / group;
        const float* qh = qv + (int64_t)h * head_dim;

        float maxv = -INFINITY;
        for (int32_t t = 0; t <= pos; t++) {
            int64_t koff = k_base + (int64_t)layer * layer_bytes + (int64_t)t * token_bytes + (int64_t)kvh * head_dim * 2;
            const uint16_t* kh = (const uint16_t*)(kv_base_ptr + koff);
            float s = dot_f16_f32_ptr(qh, kh, head_dim) * scale;
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
            int64_t voff = v_base + (int64_t)layer * layer_bytes + (int64_t)t * token_bytes + (int64_t)kvh * head_dim * 2;
            const uint16_t* vh = (const uint16_t*)(kv_base_ptr + voff);
            for (int32_t i = 0; i < head_dim; i++) oh[i] += p * f16_to_f32(vh[i]);
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
