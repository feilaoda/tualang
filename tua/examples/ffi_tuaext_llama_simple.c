#include "tua_array.h"
#include "tua_bytes.h"

#include <llama.h>

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

void tua_panic(const char* msg);

typedef struct tuaext_llama {
    struct llama_model * model;
    struct llama_context * ctx;
    const struct llama_vocab * vocab;
    int32_t n_vocab;
    int32_t pos;

    struct llama_batch batch;
    int32_t batch_cap;
} tuaext_llama;

static pthread_once_t g_llama_once = PTHREAD_ONCE_INIT;
static void tuaext_llama_log_silent(enum ggml_log_level level, const char * text, void * user_data) {
    (void) level;
    (void) text;
    (void) user_data;
}
static void tuaext_llama_init_once(void) {
    llama_backend_init();
    // Keep CLI output clean for Tua programs; override the global logger.
    llama_log_set(tuaext_llama_log_silent, NULL);
}

void tuaext_llama_backend_init(void) {
    pthread_once(&g_llama_once, tuaext_llama_init_once);
}

int64_t tuaext_llama_time_us(void) {
    pthread_once(&g_llama_once, tuaext_llama_init_once);
    return (int64_t) llama_time_us();
}

static void tuaext_llama_batch_free(tuaext_llama * h) {
    if (!h) return;
    if (h->batch_cap > 0) {
        llama_batch_free(h->batch);
        memset(&h->batch, 0, sizeof(h->batch));
        h->batch_cap = 0;
    }
}

static int tuaext_llama_ensure_batch(tuaext_llama * h, int32_t n_tokens) {
    if (!h) return 1;
    if (n_tokens <= 0) return 0;
    if (h->batch_cap >= n_tokens) return 0;
    tuaext_llama_batch_free(h);
    h->batch = llama_batch_init(n_tokens, 0, 1);
    if (!h->batch.token || !h->batch.pos || !h->batch.n_seq_id || !h->batch.seq_id || !h->batch.logits) {
        llama_batch_free(h->batch);
        memset(&h->batch, 0, sizeof(h->batch));
        h->batch_cap = 0;
        return 2;
    }
    h->batch_cap = n_tokens;
    return 0;
}

// Signature in Tua:
//   extern fn tuaext_llama_new(modelPath: string, nCtx: int, nThreads: int, nBatch: int, outErr: &int) ptr
void * tuaext_llama_new(const char * modelPath, int32_t nCtx, int32_t nThreads, int32_t nBatch, int32_t * outErr) {
    if (outErr) *outErr = 1;
    if (!outErr) return NULL;
    if (!modelPath || modelPath[0] == '\0') return NULL;
    pthread_once(&g_llama_once, tuaext_llama_init_once);

    struct llama_model_params mp = llama_model_default_params();
    struct llama_model * model = llama_model_load_from_file(modelPath, mp);
    if (!model) return NULL;

    struct llama_context_params cp = llama_context_default_params();
    if (nCtx > 0) cp.n_ctx = (uint32_t) nCtx;
    if (nBatch > 0) cp.n_batch = (uint32_t) nBatch;
    if (nThreads > 0) {
        cp.n_threads = (uint32_t) nThreads;
        cp.n_threads_batch = (uint32_t) nThreads;
    }
    struct llama_context * ctx = llama_init_from_model(model, cp);
    if (!ctx) {
        llama_model_free(model);
        return NULL;
    }

    const struct llama_vocab * vocab = llama_model_get_vocab(model);
    if (!vocab) {
        llama_free(ctx);
        llama_model_free(model);
        return NULL;
    }

    tuaext_llama * h = (tuaext_llama *) calloc(1, sizeof(tuaext_llama));
    if (!h) tua_panic("out of memory");
    h->model = model;
    h->ctx = ctx;
    h->vocab = vocab;
    h->n_vocab = llama_vocab_n_tokens(vocab);
    h->pos = 0;
    h->batch_cap = 0;
    memset(&h->batch, 0, sizeof(h->batch));

    *outErr = 0;
    return (void *) h;
}

// Signature in Tua:
//   extern fn tuaext_llama_new2(modelPath: string, nCtx: int, nThreads: int, nBatch: int, nGpuLayers: int, outErr: &int) ptr
void * tuaext_llama_new2(const char * modelPath, int32_t nCtx, int32_t nThreads, int32_t nBatch, int32_t nGpuLayers, int32_t * outErr) {
    if (outErr) *outErr = 1;
    if (!outErr) return NULL;
    if (!modelPath || modelPath[0] == '\0') return NULL;
    pthread_once(&g_llama_once, tuaext_llama_init_once);

    struct llama_model_params mp = llama_model_default_params();
    if (nGpuLayers != 0) mp.n_gpu_layers = nGpuLayers;
    struct llama_model * model = llama_model_load_from_file(modelPath, mp);
    if (!model) return NULL;

    struct llama_context_params cp = llama_context_default_params();
    if (nCtx > 0) cp.n_ctx = (uint32_t) nCtx;
    if (nBatch > 0) cp.n_batch = (uint32_t) nBatch;
    if (nThreads > 0) {
        cp.n_threads = (uint32_t) nThreads;
        cp.n_threads_batch = (uint32_t) nThreads;
    }
    struct llama_context * ctx = llama_init_from_model(model, cp);
    if (!ctx) {
        llama_model_free(model);
        return NULL;
    }

    const struct llama_vocab * vocab = llama_model_get_vocab(model);
    if (!vocab) {
        llama_free(ctx);
        llama_model_free(model);
        return NULL;
    }

    tuaext_llama * h = (tuaext_llama *) calloc(1, sizeof(tuaext_llama));
    if (!h) tua_panic("out of memory");
    h->model = model;
    h->ctx = ctx;
    h->vocab = vocab;
    h->n_vocab = llama_vocab_n_tokens(vocab);
    h->pos = 0;
    h->batch_cap = 0;
    memset(&h->batch, 0, sizeof(h->batch));

    *outErr = 0;
    return (void *) h;
}

// Signature in Tua:
//   extern fn tuaext_llama_free(h: ptr)
void tuaext_llama_free(void * hp) {
    tuaext_llama * h = (tuaext_llama *) hp;
    if (!h) return;
    tuaext_llama_batch_free(h);
    if (h->ctx) llama_free(h->ctx);
    if (h->model) llama_model_free(h->model);
    free(h);
}

// Signature in Tua:
//   extern fn tuaext_llama_vocab_size(h: ptr) int
int32_t tuaext_llama_vocab_size(void * hp) {
    tuaext_llama * h = (tuaext_llama *) hp;
    if (!h) return 0;
    return h->n_vocab;
}

// Signature in Tua:
//   extern fn tuaext_llama_vocab_is_eog(h: ptr, tok: long) int
int32_t tuaext_llama_vocab_is_eog(void * hp, int64_t tok) {
    tuaext_llama * h = (tuaext_llama *) hp;
    if (!h || !h->vocab) return 0;
    return llama_vocab_is_eog(h->vocab, (llama_token) tok) ? 1 : 0;
}

// Signature in Tua:
//   extern fn tuaext_llama_find_token_id(h: ptr, text: string, outTok: &long) int
int32_t tuaext_llama_find_token_id(void * hp, const char * text, int64_t * outTok) {
    if (!outTok) return 1;
    *outTok = -1;
    tuaext_llama * h = (tuaext_llama *) hp;
    if (!h || !h->vocab || h->n_vocab <= 0) return 2;
    if (!text) return 3;
    for (int32_t i = 0; i < h->n_vocab; i++) {
        const char * t = llama_vocab_get_text(h->vocab, (llama_token) i);
        if (!t) continue;
        if (strcmp(t, text) == 0) {
            *outTok = (int64_t) i;
            return 0;
        }
    }
    return 4;
}

static int64_t argmax_f32_ptr(const float * v, int32_t n) {
    if (!v || n <= 0) return -1;
    int32_t best = 0;
    float bestv = v[0];
    for (int32_t i = 1; i < n; i++) {
        float x = v[i];
        if (x > bestv) {
            bestv = x;
            best = i;
        }
    }
    return (int64_t) best;
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
    return (double)(x >> 11) * (1.0 / 9007199254740992.0);
}

static int32_t sample_topk_topp_from_ptr(const float* logits, int32_t n,
                                         int32_t top_k, float top_p, float temperature,
                                         uint64_t* rng_state, int64_t* out_index) {
    if (!out_index || !rng_state) return 1;
    if (n <= 0) return 1;
    if (!logits) return 1;

    if (temperature <= 0.0f) {
        *out_index = argmax_f32_ptr(logits, n);
        return (*out_index >= 0) ? 0 : 1;
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
        return 2;
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
    return 0;
}

// Signature in Tua:
//   extern fn tuaext_llama_sample_next(h: ptr, greedy: int, topK: int, topP: float, temperature: float, rngState: &long, outTok: &long) int
int32_t tuaext_llama_sample_next(void * hp,
                                int32_t greedy,
                                int32_t topK,
                                float topP,
                                float temperature,
                                int64_t * rngState,
                                int64_t * outTok) {
    if (!rngState || !outTok) return 1;
    *outTok = -1;
    tuaext_llama * h = (tuaext_llama *) hp;
    if (!h || !h->ctx || h->n_vocab <= 0) return 2;

    float * logits = llama_get_logits_ith(h->ctx, -1);
    if (!logits) return 3;

    if (greedy || temperature <= 0.0f || topK <= 1) {
        *outTok = argmax_f32_ptr(logits, h->n_vocab);
        return (*outTok >= 0) ? 0 : 4;
    }

    uint64_t st = (uint64_t)(*rngState);
    int32_t e = sample_topk_topp_from_ptr(logits, h->n_vocab, topK, topP, temperature, &st, outTok);
    *rngState = (int64_t)st;
    return e == 0 ? 0 : 5;
}

// Signature in Tua:
//   extern fn tuaext_llama_detokenize(h: ptr, toks: long[], removeSpecial: int, unparseSpecial: int, outErr: &int) string
char * tuaext_llama_detokenize(void * hp, tua_array * toks, int32_t removeSpecial, int32_t unparseSpecial, int32_t * outErr) {
    if (outErr) *outErr = 1;
    if (!outErr) return NULL;
    tuaext_llama * h = (tuaext_llama *) hp;
    if (!h || !h->vocab) return NULL;
    if (!toks) return NULL;
    if (toks->elem_size != (int64_t) sizeof(int64_t)) return NULL;
    if (toks->len < 0) return NULL;
    if (toks->len == 0) {
        char * out = (char *)malloc(1);
        if (!out) tua_panic("out of memory");
        out[0] = '\0';
        *outErr = 0;
        return out;
    }
    if (!toks->data) return NULL;
    if (toks->len > INT32_MAX) return NULL;
    int32_t n = (int32_t) toks->len;

    llama_token * tt = (llama_token *) malloc((size_t) n * sizeof(llama_token));
    if (!tt) tua_panic("out of memory");
    const int64_t * src = (const int64_t *) toks->data;
    for (int32_t i = 0; i < n; i++) tt[i] = (llama_token) src[i];

    int32_t cap = n * 8 + 8;
    if (cap < 64) cap = 64;
    char * buf = (char *) malloc((size_t) cap);
    if (!buf) tua_panic("out of memory");

    int32_t m = llama_detokenize(h->vocab, tt, n, buf, cap, removeSpecial != 0, unparseSpecial != 0);
    if (m < 0) {
        int32_t need = -m;
        free(buf);
        buf = (char *) malloc((size_t) need + 1);
        if (!buf) tua_panic("out of memory");
        m = llama_detokenize(h->vocab, tt, n, buf, need, removeSpecial != 0, unparseSpecial != 0);
        if (m < 0) {
            free(buf);
            free(tt);
            return NULL;
        }
    }
    char * out = (char *) malloc((size_t) m + 1);
    if (!out) tua_panic("out of memory");
    memcpy(out, buf, (size_t) m);
    out[m] = '\0';
    free(buf);
    free(tt);
    *outErr = 0;
    return out;
}

// Signature in Tua:
//   extern fn tuaext_llama_reset(h: ptr, outErr: &int) int
int32_t tuaext_llama_reset(void * hp, int32_t * outErr) {
    if (outErr) *outErr = 1;
    if (!outErr) return 1;
    tuaext_llama * h = (tuaext_llama *) hp;
    if (!h || !h->ctx) return 1;
    llama_memory_t mem = llama_get_memory(h->ctx);
    if (mem) llama_memory_clear(mem, true);
    h->pos = 0;
    *outErr = 0;
    return 0;
}

// Signature in Tua:
//   extern fn tuaext_llama_tokenize(h: ptr, text: string, addSpecial: int, parseSpecial: int, outErr: &int) long[]
tua_array * tuaext_llama_tokenize(void * hp, const char * text, int32_t addSpecial, int32_t parseSpecial, int32_t * outErr) {
    if (outErr) *outErr = 1;
    if (!outErr) return NULL;
    tuaext_llama * h = (tuaext_llama *) hp;
    if (!h || !h->vocab || !text) return NULL;

    int32_t textLen = (int32_t) strlen(text);
    int32_t cap = textLen + 8;
    if (cap < 16) cap = 16;
    llama_token * tmp = (llama_token *) malloc((size_t) cap * sizeof(llama_token));
    if (!tmp) tua_panic("out of memory");

    int32_t n = llama_tokenize(h->vocab, text, textLen, tmp, cap, addSpecial != 0, parseSpecial != 0);
    if (n < 0) {
        int32_t need = -n;
        free(tmp);
        tmp = (llama_token *) malloc((size_t) need * sizeof(llama_token));
        if (!tmp) tua_panic("out of memory");
        n = llama_tokenize(h->vocab, text, textLen, tmp, need, addSpecial != 0, parseSpecial != 0);
        if (n < 0) {
            free(tmp);
            return NULL;
        }
    }

    tua_array * out = tua_array_new((int64_t) n, (int64_t) n, (int64_t) sizeof(int64_t), -1);
    if (!out) tua_panic("out of memory");
    int64_t * dst = (int64_t *) out->data;
    for (int32_t i = 0; i < n; i++) dst[i] = (int64_t) tmp[i];
    free(tmp);
    *outErr = 0;
    return out;
}

// Signature in Tua:
//   extern fn tuaext_llama_token_to_piece(h: ptr, tok: long, special: int, outErr: &int) string
char * tuaext_llama_token_to_piece(void * hp, int64_t tok, int32_t special, int32_t * outErr) {
    if (outErr) *outErr = 1;
    if (!outErr) return NULL;
    tuaext_llama * h = (tuaext_llama *) hp;
    if (!h || !h->vocab) return NULL;

    int32_t cap = 128;
    char * buf = (char *) malloc((size_t) cap);
    if (!buf) tua_panic("out of memory");

    int32_t n = llama_token_to_piece(h->vocab, (llama_token) tok, buf, cap, 0, special != 0);
    if (n < 0) {
        int32_t need = -n;
        free(buf);
        buf = (char *) malloc((size_t) need + 1);
        if (!buf) tua_panic("out of memory");
        n = llama_token_to_piece(h->vocab, (llama_token) tok, buf, need, 0, special != 0);
        if (n < 0) {
            free(buf);
            return NULL;
        }
    }

    char * out = (char *) malloc((size_t) n + 1);
    if (!out) tua_panic("out of memory");
    memcpy(out, buf, (size_t) n);
    out[n] = '\0';
    free(buf);
    *outErr = 0;
    return out;
}

// Signature in Tua:
//   extern fn tuaext_llama_decode(h: ptr, toks: long[], outErr: &int) int
int32_t tuaext_llama_decode(void * hp, tua_array * toks, int32_t * outErr) {
    if (outErr) *outErr = 1;
    if (!outErr) return 1;
    tuaext_llama * h = (tuaext_llama *) hp;
    if (!h || !h->ctx || !toks) return 1;
    if (toks->elem_size != (int64_t) sizeof(int64_t)) return 2;
    if (toks->len < 0) return 3;
    if (toks->len == 0) {
        *outErr = 0;
        return 0;
    }
    if (!toks->data) return 4;
    if (toks->len > INT32_MAX) return 5;

    int32_t n = (int32_t) toks->len;
    int e = tuaext_llama_ensure_batch(h, n);
    if (e != 0) return 6;

    const int64_t * src = (const int64_t *) toks->data;
    h->batch.n_tokens = n;
    for (int32_t i = 0; i < n; i++) {
        h->batch.token[i] = (llama_token) src[i];
        h->batch.pos[i] = (llama_pos) (h->pos + i);
        h->batch.n_seq_id[i] = 1;
        h->batch.seq_id[i][0] = 0;
        h->batch.logits[i] = 0;
    }
    h->batch.logits[n - 1] = 1;

    int32_t rc = llama_decode(h->ctx, h->batch);
    if (rc < 0) {
        *outErr = rc;
        return rc;
    }
    h->pos += n;
    *outErr = 0;
    return rc;
}

// Signature in Tua:
//   extern fn tuaext_llama_get_logits_f32(h: ptr, outErr: &int) bytes
tua_bytes * tuaext_llama_get_logits_f32(void * hp, int32_t * outErr) {
    if (outErr) *outErr = 1;
    if (!outErr) return NULL;
    tuaext_llama * h = (tuaext_llama *) hp;
    if (!h || !h->ctx || h->n_vocab <= 0) return NULL;

    float * logits = llama_get_logits_ith(h->ctx, -1);
    if (!logits) return NULL;

    int64_t nbytes = (int64_t) h->n_vocab * 4;
    tua_bytes * out = tua_bytes_new_uninit(nbytes);
    if (!out) tua_panic("out of memory");
    memcpy(tua_bytes_data_at(out, 0), logits, (size_t) nbytes);
    *outErr = 0;
    return out;
}
