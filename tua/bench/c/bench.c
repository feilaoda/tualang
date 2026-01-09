#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int64_t now_ns(void) {
#if defined(CLOCK_MONOTONIC)
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
#else
    return 0;
#endif
}

static void report(const char* name, int64_t iters, int64_t dt_ns, int64_t sink) {
    double ns_per_iter = (double)dt_ns / (double)iters;
    printf("%s iters=%lld dt_ns=%lld ns/iter=%g sink=%lld\n",
           name, (long long)iters, (long long)dt_ns, ns_per_iter, (long long)sink);
}

static int64_t bench_arith_int(int64_t iters) {
    int64_t x = 1;
    int64_t acc = 0;
    for (int64_t i = 0; i < iters; i++) {
        x = (x * 1664525LL) + 1013904223LL;
        acc += (x & 65535LL);
    }
    return acc;
}

static float* make_float_array(int n, float seed) {
    float* a = (float*)malloc((size_t)n * sizeof(float));
    if (!a) return NULL;
    for (int i = 0; i < n; i++) {
        a[i] = ((float)i * 0.001f) + seed;
    }
    return a;
}

static double bench_dot_f32(int iters, const float* a, const float* b, int n) {
    double sum = 0.0;
    for (int k = 0; k < iters; k++) {
        double s = 0.0;
        for (int i = 0; i < n; i++) {
            s += (double)a[i] * (double)b[i];
        }
        sum += s;
    }
    return sum;
}

static uint8_t* make_bytes(size_t n) {
    uint8_t* b = (uint8_t*)malloc(n);
    if (!b) return NULL;
    for (size_t i = 0; i < n; i++) {
        b[i] = (uint8_t)((((int64_t)i) * 131LL + 7LL) & 255LL);
    }
    return b;
}

static int64_t bench_bytes_scan(int iters, const uint8_t* b, size_t n, uint8_t needle) {
    int64_t total = 0;
    for (int k = 0; k < iters; k++) {
        int64_t cnt = 0;
        for (size_t i = 0; i < n; i++) {
            cnt += (b[i] == needle) ? 1 : 0;
        }
        total += cnt;
    }
    return total;
}

typedef struct {
    int key;
    int value;
    uint8_t used;
} int_int_entry;

typedef struct {
    int_int_entry* entries;
    int cap;
} int_int_map;

static uint32_t hash_u32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

static int_int_map map_new(int cap_pow2) {
    int_int_map m;
    m.cap = cap_pow2;
    m.entries = (int_int_entry*)calloc((size_t)cap_pow2, sizeof(int_int_entry));
    return m;
}

static void map_free(int_int_map* m) {
    free(m->entries);
    m->entries = NULL;
    m->cap = 0;
}

static void map_set(int_int_map* m, int key, int value) {
    uint32_t h = hash_u32((uint32_t)key);
    uint32_t mask = (uint32_t)m->cap - 1U;
    uint32_t i = h & mask;
    while (1) {
        int_int_entry* e = &m->entries[i];
        if (!e->used || e->key == key) {
            e->used = 1;
            e->key = key;
            e->value = value;
            return;
        }
        i = (i + 1U) & mask;
    }
}

static int map_get(const int_int_map* m, int key, int* out) {
    uint32_t h = hash_u32((uint32_t)key);
    uint32_t mask = (uint32_t)m->cap - 1U;
    uint32_t i = h & mask;
    while (1) {
        const int_int_entry* e = &m->entries[i];
        if (!e->used) return 0;
        if (e->key == key) {
            *out = e->value;
            return 1;
        }
        i = (i + 1U) & mask;
    }
}

static int_int_map make_map_10k(void) {
    // capacity must be power-of-2; leave headroom for linear probing
    int_int_map m = map_new(1 << 14); // 16384
    for (int i = 0; i < 8192; i++) {
        map_set(&m, i, (i * 2) + 1);
    }
    return m;
}

static int64_t bench_map_lookup(int64_t iters, const int_int_map* m, int mod) {
    int64_t acc = 0;
    int64_t x = 1;
    for (int64_t i = 0; i < iters; i++) {
        x = (x * 1103515245LL + 12345LL) & 2147483647LL;
        int k = (int)(x % (int64_t)mod);
        int v = 0;
        if (!map_get(m, k, &v)) return -1;
        acc += (int64_t)v;
    }
    return acc;
}

static int read_file(const char* path, char** out, size_t* out_len) {
    FILE* f = fopen(path, "rb");
    if (!f) return 1;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return 1;
    }
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return 1;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return 1;
    }
    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return 1;
    }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (n != (size_t)sz) {
        free(buf);
        return 1;
    }
    buf[n] = '\0';
    *out = buf;
    *out_len = n;
    return 0;
}

static int scan_top_level_long(const char* s, size_t n, const char* key, int64_t* out_val) {
    // Minimal JSON top-level object scanner (sufficient for bench input):
    // - handles strings + escaping
    // - only extracts a top-level numeric field
    size_t key_len = strlen(key);
    int depth = 0;
    int in_str = 0;
    int esc = 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (in_str) {
            if (esc) {
                esc = 0;
                continue;
            }
            if (c == '\\') {
                esc = 1;
                continue;
            }
            if (c == '"') in_str = 0;
            continue;
        }
        if (c == '"') {
            in_str = 1;
            if (depth == 1) {
                // parse key string
                size_t start = i + 1;
                size_t j = start;
                int esc2 = 0;
                while (j < n) {
                    char cc = s[j];
                    if (esc2) {
                        esc2 = 0;
                        j++;
                        continue;
                    }
                    if (cc == '\\') {
                        esc2 = 1;
                        j++;
                        continue;
                    }
                    if (cc == '"') break;
                    j++;
                }
                if (j >= n) return 1;
                size_t str_len = j - start;
                int match = (str_len == key_len) && memcmp(s + start, key, key_len) == 0;
                i = j; // position at closing quote
                in_str = 0; // we handled the full string token
                if (!match) continue;

                // skip ws + colon
                size_t k = i + 1;
                while (k < n && (s[k] == ' ' || s[k] == '\n' || s[k] == '\r' || s[k] == '\t')) k++;
                if (k >= n || s[k] != ':') return 1;
                k++;
                while (k < n && (s[k] == ' ' || s[k] == '\n' || s[k] == '\r' || s[k] == '\t')) k++;
                if (k >= n) return 1;

                // parse integer
                int neg = 0;
                if (s[k] == '-') {
                    neg = 1;
                    k++;
                }
                if (k >= n || s[k] < '0' || s[k] > '9') return 1;
                int64_t v = 0;
                while (k < n && s[k] >= '0' && s[k] <= '9') {
                    v = v * 10 + (int64_t)(s[k] - '0');
                    k++;
                }
                *out_val = neg ? -v : v;
                return 0;
            }
            continue;
        }
        if (c == '{') depth++;
        else if (c == '}') depth--;
    }
    return 1;
}

static int64_t bench_json_scan_top_long(int iters, const char* s, size_t n) {
    int64_t acc = 0;
    for (int i = 0; i < iters; i++) {
        int64_t v = 0;
        if (scan_top_level_long(s, n, "id", &v) != 0) return -1;
        acc += v;
    }
    return acc;
}

static int run_all(void) {
    int64_t iters_arith = 50000000LL;
    int64_t iters_lookup = 20000000LL;
    int iters_bytes = 50;
    int iters_dot = 3000;
    int iters_json = 2000;

    (void)bench_arith_int(10000);

    int64_t t0 = now_ns();
    int64_t s0 = bench_arith_int(iters_arith);
    int64_t t1 = now_ns();
    report("c/arith_int", iters_arith, t1 - t0, s0);

    float* a = make_float_array(1024, 0.25f);
    float* b = make_float_array(1024, 0.75f);
    int64_t td0 = now_ns();
    double sd = bench_dot_f32(iters_dot, a, b, 1024);
    int64_t td1 = now_ns();
    report("c/dot_f32_1024", (int64_t)iters_dot, td1 - td0, (int64_t)sd);
    free(a);
    free(b);

    uint8_t* bytes = make_bytes(1024 * 1024);
    int64_t tb0 = now_ns();
    int64_t sb = bench_bytes_scan(iters_bytes, bytes, 1024 * 1024, 127);
    int64_t tb1 = now_ns();
    report("c/bytes_scan_1m", (int64_t)iters_bytes, tb1 - tb0, sb);

    int64_t tb2 = now_ns();
    int64_t sb2 = bench_bytes_scan(iters_bytes, bytes, 1024 * 1024, 127);
    int64_t tb3 = now_ns();
    report("c/bytes_scan_1m_slice", (int64_t)iters_bytes, tb3 - tb2, sb2);
    free(bytes);

    int_int_map m = make_map_10k();
    int64_t tm0 = now_ns();
    int64_t sm = bench_map_lookup(iters_lookup, &m, 8192);
    int64_t tm1 = now_ns();
    report("c/map_lookup_8k", iters_lookup, tm1 - tm0, sm);
    map_free(&m);

    char* js = NULL;
    size_t jn = 0;
    if (read_file("bench/data/scan_top_level.json", &js, &jn) == 0) {
        int64_t tj0 = now_ns();
        int64_t sj = bench_json_scan_top_long(iters_json, js, jn);
        int64_t tj1 = now_ns();
        report("c/json_scan_top_long", (int64_t)iters_json, tj1 - tj0, sj);
        free(js);
    } else {
        printf("c/json_scan_top_long skipped (missing input)\n");
    }

    return 0;
}

int main(int argc, char** argv) {
    if (argc <= 1) return run_all();
    if (strcmp(argv[1], "--all") == 0) return run_all();
    if (strcmp(argv[1], "--bench") != 0 || argc < 3) {
        fprintf(stderr, "usage: bench --all | --bench <name> [--iters N]\n");
        return 2;
    }
    const char* name = argv[2];
    int64_t iters = 10000000LL;
    for (int i = 3; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--iters") == 0) {
            iters = atoll(argv[i + 1]);
        }
    }
    if (strcmp(name, "arith_int") == 0) {
        int64_t t0 = now_ns();
        int64_t s = bench_arith_int(iters);
        int64_t t1 = now_ns();
        report("c/arith_int", iters, t1 - t0, s);
        return 0;
    }
    if (strcmp(name, "map_lookup") == 0) {
        int_int_map m = make_map_10k();
        int64_t t0 = now_ns();
        int64_t s = bench_map_lookup(iters, &m, 8192);
        int64_t t1 = now_ns();
        report("c/map_lookup_8k", iters, t1 - t0, s);
        map_free(&m);
        return 0;
    }
    if (strcmp(name, "dot_f32") == 0) {
        float* a = make_float_array(1024, 0.25f);
        float* b = make_float_array(1024, 0.75f);
        int64_t t0 = now_ns();
        double s = bench_dot_f32((int)iters, a, b, 1024);
        int64_t t1 = now_ns();
        report("c/dot_f32_1024", iters, t1 - t0, (int64_t)s);
        free(a);
        free(b);
        return 0;
    }
    if (strcmp(name, "bytes_scan") == 0) {
        uint8_t* bytes = make_bytes(1024 * 1024);
        int64_t t0 = now_ns();
        int64_t s = bench_bytes_scan((int)iters, bytes, 1024 * 1024, 127);
        int64_t t1 = now_ns();
        report("c/bytes_scan_1m", iters, t1 - t0, s);
        free(bytes);
        return 0;
    }
    if (strcmp(name, "bytes_scan_slice") == 0) {
        uint8_t* bytes = make_bytes(1024 * 1024);
        int64_t t0 = now_ns();
        int64_t s = bench_bytes_scan((int)iters, bytes, 1024 * 1024, 127);
        int64_t t1 = now_ns();
        report("c/bytes_scan_1m_slice", iters, t1 - t0, s);
        free(bytes);
        return 0;
    }
    if (strcmp(name, "json_scan_top_long") == 0) {
        char* js = NULL;
        size_t jn = 0;
        if (read_file("bench/data/scan_top_level.json", &js, &jn) != 0) return 2;
        int64_t t0 = now_ns();
        int64_t s = bench_json_scan_top_long((int)iters, js, jn);
        int64_t t1 = now_ns();
        report("c/json_scan_top_long", iters, t1 - t0, s);
        free(js);
        return 0;
    }
    fprintf(stderr, "unknown bench: %s\n", name);
    return 2;
}
