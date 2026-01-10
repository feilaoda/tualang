#include "tua_str.h"

#include <errno.h>
#include <ctype.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "rt/rt_alloc.h"
#include "tua_bytes.h"
#include "tua_map.h"   // tua_panic

#if defined(__has_include)
#  if __has_include(<iconv.h>)
#    include <iconv.h>
#    define TUA_HAS_ICONV 1
#  endif
#endif
#ifndef TUA_HAS_ICONV
#  define TUA_HAS_ICONV 0
#endif

// Managed string layout: [hdr][utf8 bytes][NUL]
// `char*` points to the first byte of the utf8 bytes.
// The header is discovered by reading immediately before the data pointer.
// Unmanaged strings (no header) are treated as borrowed C strings.

typedef struct tua_str_hdr {
    uint32_t magic;
    uintptr_t data_ptr; // points to the first byte (h+1)
    int32_t rc;       // -1 => immortal/static, >=1 => refcounted
    uint32_t hash32;  // 0 => unknown
    int32_t cp_len;   // -1 => unknown
    int32_t _pad;
    int64_t byte_len; // number of bytes excluding trailing NUL
} tua_str_hdr;

enum { TUA_STR_MAGIC = 0x54554153u }; // 'TUAS'

// Registry of managed string pointers.
// This avoids unsafe header probing on unmanaged C strings / literals.
typedef struct {
    const char* key; // data pointer (h+1)
    uint8_t state;   // 0 empty, 1 full, 2 tombstone
} str_reg_entry;

static str_reg_entry* str_reg = NULL;
static size_t str_reg_cap = 0;
static size_t str_reg_count = 0;
static size_t str_reg_tombs = 0;

static uint64_t hash_ptr64(uintptr_t p) {
    uint64_t x = (uint64_t)p;
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x ? x : 1ULL;
}

static void str_reg_grow(size_t want) {
    size_t cap = str_reg_cap ? str_reg_cap : 256;
    while (cap < want) cap *= 2;
    str_reg_entry* n = (str_reg_entry*)tua_malloc(sizeof(str_reg_entry) * cap);
    if (!n) tua_panic("out of memory");
    memset(n, 0, sizeof(str_reg_entry) * cap);

    str_reg_entry* old = str_reg;
    size_t oldCap = str_reg_cap;
    str_reg = n;
    str_reg_cap = cap;
    str_reg_count = 0;
    str_reg_tombs = 0;

    if (old && oldCap) {
        for (size_t i = 0; i < oldCap; i++) {
            if (old[i].state != 1) continue;
            const char* k = old[i].key;
            uint64_t hv = hash_ptr64((uintptr_t)k);
            size_t mask = cap - 1;
            size_t j = (size_t)hv & mask;
            while (str_reg[j].state == 1) j = (j + 1) & mask;
            str_reg[j].key = k;
            str_reg[j].state = 1;
            str_reg_count++;
        }
        tua_free(old);
    }
}

static int str_reg_contains(const char* s) {
    if (!s || !str_reg || str_reg_cap == 0) return 0;
    uint64_t hv = hash_ptr64((uintptr_t)s);
    size_t mask = str_reg_cap - 1;
    size_t i = (size_t)hv & mask;
    for (;;) {
        str_reg_entry* e = &str_reg[i];
        if (e->state == 0) return 0;
        if (e->state == 1 && e->key == s) return 1;
        i = (i + 1) & mask;
    }
}

static void str_reg_insert(const char* s) {
    if (!s) return;
    if (!str_reg || str_reg_cap == 0) str_reg_grow(256);
    if ((str_reg_count + str_reg_tombs + 1) * 10 >= str_reg_cap * 7) {
        str_reg_grow(str_reg_cap * 2);
    }
    uint64_t hv = hash_ptr64((uintptr_t)s);
    size_t mask = str_reg_cap - 1;
    size_t i = (size_t)hv & mask;
    size_t firstTomb = (size_t)-1;
    for (;;) {
        str_reg_entry* e = &str_reg[i];
        if (e->state == 0) {
            size_t dst = (firstTomb != (size_t)-1) ? firstTomb : i;
            if (str_reg[dst].state == 2) str_reg_tombs--;
            str_reg[dst].key = s;
            str_reg[dst].state = 1;
            str_reg_count++;
            return;
        }
        if (e->state == 2) {
            if (firstTomb == (size_t)-1) firstTomb = i;
        } else if (e->key == s) {
            return;
        }
        i = (i + 1) & mask;
    }
}

static void str_reg_remove(const char* s) {
    if (!s || !str_reg || str_reg_cap == 0) return;
    uint64_t hv = hash_ptr64((uintptr_t)s);
    size_t mask = str_reg_cap - 1;
    size_t i = (size_t)hv & mask;
    for (;;) {
        str_reg_entry* e = &str_reg[i];
        if (e->state == 0) return;
        if (e->state == 1 && e->key == s) {
            e->key = NULL;
            e->state = 2;
            str_reg_count--;
            str_reg_tombs++;
            return;
        }
        i = (i + 1) & mask;
    }
}

static inline tua_str_hdr* hdr_from_data_unsafe(const char* s) {
    return (tua_str_hdr*)s - 1;
}

static inline tua_str_hdr* hdr_from_data(const char* s) {
    if (!s) return NULL;
    // Only probe headers for known-managed strings.
    if (!str_reg_contains(s)) return NULL;
    tua_str_hdr* h = hdr_from_data_unsafe(s);
    if (!h) return NULL;
    if (h->magic != (uint32_t)TUA_STR_MAGIC) return NULL;
    if ((const char*)(uintptr_t)h->data_ptr != s) return NULL;
    if (h->byte_len < 0) return NULL;
    return h;
}

static inline const uint8_t* bytes_ptr(const char* s) {
    return (const uint8_t*)s;
}

static uint32_t fnv1a32(const uint8_t* p, int64_t n) {
    uint32_t h = 2166136261u;
    if (n <= 0 || !p) return 1u;
    for (int64_t i = 0; i < n; i++) {
        h ^= (uint32_t)p[i];
        h *= 16777619u;
    }
    return h ? h : 1u;
}

static int32_t utf8_advance(const uint8_t* p, int64_t n) {
    if (!p || n <= 0) return 0;
    uint8_t b0 = p[0];
    if (b0 < 0x80) return 1;
    // 2-byte
    if ((b0 & 0xE0u) == 0xC0u) return (n >= 2) ? 2 : 1;
    // 3-byte
    if ((b0 & 0xF0u) == 0xE0u) return (n >= 3) ? 3 : 1;
    // 4-byte
    if ((b0 & 0xF8u) == 0xF0u) return (n >= 4) ? 4 : 1;
    return 1;
}

static int32_t utf8_validate_advance(const uint8_t* p, int64_t n) {
    if (!p || n <= 0) return 0;
    uint8_t b0 = p[0];
    if (b0 < 0x80) return 1;

    int32_t need = 0;
    if ((b0 & 0xE0u) == 0xC0u) need = 2;
    else if ((b0 & 0xF0u) == 0xE0u) need = 3;
    else if ((b0 & 0xF8u) == 0xF0u) need = 4;
    else return 0;

    if (n < need) return 0;
    for (int32_t i = 1; i < need; i++) {
        if ((p[i] & 0xC0u) != 0x80u) return 0;
    }
    return need;
}

static int32_t count_codepoints_best_effort(const uint8_t* p, int64_t n) {
    if (!p || n <= 0) return 0;
    int32_t cnt = 0;
    int64_t i = 0;
    while (i < n) {
        int32_t adv = utf8_advance(p + i, n - i);
        if (adv <= 0) adv = 1;
        cnt++;
        i += adv;
    }
    return cnt;
}

int64_t tua_str_byte_len(const char* s) {
    if (!s) return 0;
    tua_str_hdr* h = hdr_from_data(s);
    if (h) return h->byte_len;
    size_t n = strlen(s);
    if (n > (size_t)LLONG_MAX) return (int64_t)LLONG_MAX;
    return (int64_t)n;
}

int32_t tua_str_len(const char* s) {
    if (!s) return 0;
    tua_str_hdr* h = hdr_from_data(s);
    if (h && h->cp_len >= 0) return h->cp_len;
    int64_t bl = tua_str_byte_len(s);
    int32_t n = count_codepoints_best_effort(bytes_ptr(s), bl);
    if (h) h->cp_len = n;
    return n;
}

int32_t tua_str_eq(const char* a, const char* b) {
    if (!a && !b) return 1;
    if (!a || !b) return 0;
    int64_t la = tua_str_byte_len(a);
    int64_t lb = tua_str_byte_len(b);
    if (la != lb) return 0;
    if (la == 0) return 1;
    return memcmp(a, b, (size_t)la) == 0 ? 1 : 0;
}

uint32_t tua_str_hash32(const char* s) {
    if (!s) return 1u;
    tua_str_hdr* h = hdr_from_data(s);
    if (h && h->hash32) return h->hash32;
    int64_t n = tua_str_byte_len(s);
    uint32_t hv = fnv1a32(bytes_ptr(s), n);
    if (h) h->hash32 = hv;
    return hv;
}

void tua_str_retain(const char* s) {
    if (!s) return;
    tua_str_hdr* h = hdr_from_data(s);
    if (!h) return;
    if (h->rc < 0) return; // immortal
    h->rc++;
}

void tua_str_release(const char* s) {
    if (!s) return;
    tua_str_hdr* h = hdr_from_data(s);
    if (!h) return;
    if (h->rc < 0) return; // immortal
    h->rc--;
    if (h->rc <= 0) {
        str_reg_remove(s);
        tua_free(h);
    }
}

static char* tua_str_alloc_copy(const uint8_t* data, int64_t len) {
    if (len < 0) return NULL;
    uint64_t ulen = (uint64_t)len;
    if (ulen > (uint64_t)(SIZE_MAX - sizeof(tua_str_hdr) - 1)) return NULL;
    size_t total = sizeof(tua_str_hdr) + (size_t)ulen + 1;
    tua_str_hdr* h = (tua_str_hdr*)tua_malloc(total);
    if (!h) tua_panic("out of memory");
    h->magic = (uint32_t)TUA_STR_MAGIC;
    h->data_ptr = (uintptr_t)(h + 1);
    h->rc = 1;
    h->hash32 = 0;
    h->cp_len = -1;
    h->_pad = 0;
    h->byte_len = len;
    char* out = (char*)(h + 1);
    if (ulen > 0 && data) memcpy(out, data, (size_t)ulen);
    out[ulen] = '\0';
    str_reg_insert(out);
    return out;
}

char* tua_str_concat(const char* a, const char* b) {
    const char* na = a ? a : "null";
    const char* nb = b ? b : "null";
    int64_t la = tua_str_byte_len(na);
    int64_t lb = tua_str_byte_len(nb);
    if (la < 0 || lb < 0) return NULL;
    if (la > (int64_t)LLONG_MAX - lb) return NULL;
    int64_t len = la + lb;
    char* out = tua_str_alloc_copy(NULL, len);
    if (!out) return NULL;
    if (la > 0) memcpy(out, na, (size_t)la);
    if (lb > 0) memcpy(out + la, nb, (size_t)lb);
    return out;
}

char* tua_str_substring(const char* s, int32_t i) {
    if (!s) tua_panic("substring on null string");
    int32_t n = tua_str_len(s);
    if (n <= 0) tua_panic("substring index out of range");
    int32_t idx = i;
    if (idx < 0) idx = n + idx;
    if (idx < 0 || idx >= n) tua_panic("substring index out of range");

    int64_t bl = tua_str_byte_len(s);
    const uint8_t* p = bytes_ptr(s);
    int32_t cur = 0;
    int64_t off = 0;
    while (off < bl && cur < idx) {
        int32_t adv = utf8_advance(p + off, bl - off);
        if (adv <= 0) adv = 1;
        off += (int64_t)adv;
        cur++;
    }
    if (off < 0 || off > bl) tua_panic("substring index out of range");
    int64_t outLen = bl - off;
    return tua_str_alloc_copy(p + off, outLen);
}

char* tua_str_from_utf8_bytes_replace(const uint8_t* data, int64_t len) {
    if (len < 0) return NULL;
    if (len == 0) return tua_str_alloc_copy(NULL, 0);
    if (!data) return NULL;

    // Worst-case expansion: every byte becomes U+FFFD (3 bytes).
    uint64_t ulen = (uint64_t)len;
    if (ulen > (uint64_t)(SIZE_MAX / 3u)) return NULL;
    size_t cap = (size_t)ulen * 3u;
    uint8_t* out = (uint8_t*)tua_malloc(cap);
    if (!out) tua_panic("out of memory");
    size_t outLen = 0;

    int64_t i = 0;
    while (i < len) {
        int32_t adv = utf8_validate_advance(data + i, len - i);
        if (adv > 0) {
            memcpy(out + outLen, data + i, (size_t)adv);
            outLen += (size_t)adv;
            i += adv;
            continue;
        }
        // U+FFFD
        out[outLen + 0] = 0xEF;
        out[outLen + 1] = 0xBF;
        out[outLen + 2] = 0xBD;
        outLen += 3u;
        i += 1;
    }

    char* s = tua_str_alloc_copy(out, (int64_t)outLen);
    tua_free(out);
    return s;
}

typedef enum {
    ENC_UTF8 = 1,
    ENC_GBK = 2,
    ENC_UNSUPPORTED = 3,
} tua_encoding_kind;

static tua_encoding_kind encoding_kind(const char* enc) {
    if (!enc) return ENC_UTF8;
    while (*enc && isspace((unsigned char)*enc)) enc++;
    if (!*enc) return ENC_UTF8;

    char norm[16];
    size_t n = 0;
    for (const unsigned char* p = (const unsigned char*)enc; *p; p++) {
        unsigned char c = *p;
        if (c == '-' || c == '_' || c == ' ' || c == '\t') continue;
        if (!isalnum(c)) continue;
        if (n + 1 >= sizeof(norm)) break;
        norm[n++] = (char)tolower(c);
    }
    norm[n] = '\0';

    if (strcmp(norm, "utf8") == 0) return ENC_UTF8;
    if (strcmp(norm, "gbk") == 0) return ENC_GBK;
    return ENC_UNSUPPORTED;
}

#if TUA_HAS_ICONV
static iconv_t iconv_open_gbk_to_utf8(void) {
    iconv_t cd = iconv_open("UTF-8", "GBK");
    if (cd != (iconv_t)-1) return cd;
    // Some platforms use CP936 as the GBK alias.
    cd = iconv_open("UTF-8", "CP936");
    return cd;
}

static iconv_t iconv_open_utf8_to_gbk(void) {
    iconv_t cd = iconv_open("GBK", "UTF-8");
    if (cd != (iconv_t)-1) return cd;
    cd = iconv_open("CP936", "UTF-8");
    return cd;
}

static int grow_buf(uint8_t** buf, size_t* cap, char** outp, size_t* outLeft, size_t minExtra) {
    size_t used = (size_t)(*outp - (char*)(*buf));
    size_t want = *cap;
    size_t need = used + minExtra;
    if (want < need) want = need;
    if (want < *cap * 2) want = *cap * 2;
    if (want < 64) want = 64;
    if (want > (size_t)SIZE_MAX) return 0;
    uint8_t* nb = (uint8_t*)tua_malloc(want);
    if (!nb) return 0;
    if (used) memcpy(nb, *buf, used);
    tua_free(*buf);
    *buf = nb;
    *cap = want;
    *outp = (char*)(*buf) + used;
    *outLeft = *cap - used;
    return 1;
}
#endif

tua_err_t tua_str_to_bytes_encoding(const char* s, const char* encoding, tua_bytes** out_bytes) {
    if (!out_bytes) return TUA_E_INVALID;
    *out_bytes = NULL;

    tua_encoding_kind k = encoding_kind(encoding);
    if (k == ENC_UNSUPPORTED) return TUA_E_NOTSUP;

    if (!s) {
        tua_bytes* b0 = tua_bytes_new(0);
        if (!b0) return TUA_E_NOMEM;
        *out_bytes = b0;
        return TUA_OK;
    }

    int64_t bl = tua_str_byte_len(s);
    if (bl < 0) return TUA_E_INVALID;
    if (bl == 0) {
        tua_bytes* b0 = tua_bytes_new(0);
        if (!b0) return TUA_E_NOMEM;
        *out_bytes = b0;
        return TUA_OK;
    }
    if ((uint64_t)bl > (uint64_t)SIZE_MAX) return TUA_E_INVALID;

    if (k == ENC_UTF8) {
        tua_bytes* b = tua_bytes_from_copy(s, bl);
        if (!b) return TUA_E_NOMEM;
        *out_bytes = b;
        return TUA_OK;
    }

    // GBK
#if !TUA_HAS_ICONV
    (void)bl;
    return TUA_E_NOTSUP;
#else
    iconv_t cd = iconv_open_utf8_to_gbk();
    if (cd == (iconv_t)-1) return TUA_E_NOTSUP;

    size_t inLeft = (size_t)bl;
    char* in = (char*)s;

    size_t cap = (size_t)bl * 2u + 16u;
    if (cap < 64u) cap = 64u;
    uint8_t* outBuf = (uint8_t*)tua_malloc(cap);
    if (!outBuf) {
        iconv_close(cd);
        return TUA_E_NOMEM;
    }
    char* out = (char*)outBuf;
    size_t outLeft = cap;

    while (inLeft > 0) {
        size_t r = iconv(cd, &in, &inLeft, &out, &outLeft);
        if (r != (size_t)-1) continue;
        if (errno == E2BIG) {
            if (!grow_buf(&outBuf, &cap, &out, &outLeft, 16)) {
                tua_free(outBuf);
                iconv_close(cd);
                return TUA_E_NOMEM;
            }
            continue;
        }

        // Replace either invalid UTF-8 or unrepresentable codepoint with '?'.
        if (outLeft == 0) {
            if (!grow_buf(&outBuf, &cap, &out, &outLeft, 16)) {
                tua_free(outBuf);
                iconv_close(cd);
                return TUA_E_NOMEM;
            }
        }
        *out++ = (char)0x3F;
        outLeft--;

        int32_t adv = utf8_validate_advance((const uint8_t*)in, (int64_t)inLeft);
        if (adv <= 0) adv = 1;
        if ((size_t)adv > inLeft) adv = 1;
        in += adv;
        inLeft -= (size_t)adv;
        (void)iconv(cd, NULL, NULL, NULL, NULL);
    }

    iconv_close(cd);
    size_t outLen = cap - outLeft;
    tua_bytes* b = tua_bytes_from_copy(outBuf, (int64_t)outLen);
    tua_free(outBuf);
    if (!b) return TUA_E_NOMEM;
    *out_bytes = b;
    return TUA_OK;
#endif
}

tua_err_t tua_str_from_bytes_encoding(tua_bytes* b, const char* encoding, char** out_s) {
    if (!out_s) return TUA_E_INVALID;
    *out_s = NULL;

    tua_encoding_kind k = encoding_kind(encoding);
    if (k == ENC_UNSUPPORTED) return TUA_E_NOTSUP;

    int64_t n = tua_bytes_len(b);
    if (n < 0) return TUA_E_INVALID;
    if (n == 0) {
        *out_s = tua_str_alloc_copy(NULL, 0);
        return *out_s ? TUA_OK : TUA_E_NOMEM;
    }
    uint8_t* data = tua_bytes_data(b);
    if (!data) return TUA_E_INVALID;
    if ((uint64_t)n > (uint64_t)SIZE_MAX) return TUA_E_INVALID;

    if (k == ENC_UTF8) {
        char* s = tua_str_from_utf8_bytes_replace(data, n);
        if (!s) return TUA_E_NOMEM;
        *out_s = s;
        return TUA_OK;
    }

    // GBK
#if !TUA_HAS_ICONV
    return TUA_E_NOTSUP;
#else
    iconv_t cd = iconv_open_gbk_to_utf8();
    if (cd == (iconv_t)-1) return TUA_E_NOTSUP;

    size_t inLeft = (size_t)n;
    char* in = (char*)data;

    size_t cap = (size_t)n * 3u + 16u;
    if (cap < 64u) cap = 64u;
    uint8_t* outBuf = (uint8_t*)tua_malloc(cap);
    if (!outBuf) {
        iconv_close(cd);
        return TUA_E_NOMEM;
    }
    char* out = (char*)outBuf;
    size_t outLeft = cap;

    while (inLeft > 0) {
        size_t r = iconv(cd, &in, &inLeft, &out, &outLeft);
        if (r != (size_t)-1) continue;
        if (errno == E2BIG) {
            if (!grow_buf(&outBuf, &cap, &out, &outLeft, 16)) {
                tua_free(outBuf);
                iconv_close(cd);
                return TUA_E_NOMEM;
            }
            continue;
        }

        // Invalid GBK byte/sequence: consume 1 byte and emit U+FFFD.
        if (outLeft < 3) {
            if (!grow_buf(&outBuf, &cap, &out, &outLeft, 16)) {
                tua_free(outBuf);
                iconv_close(cd);
                return TUA_E_NOMEM;
            }
        }
        out[0] = (char)0xEF;
        out[1] = (char)0xBF;
        out[2] = (char)0xBD;
        out += 3;
        outLeft -= 3;
        in++;
        inLeft--;
        (void)iconv(cd, NULL, NULL, NULL, NULL);
    }

    iconv_close(cd);
    size_t outLen = cap - outLeft;
    char* s = tua_str_alloc_copy(outBuf, (int64_t)outLen);
    tua_free(outBuf);
    if (!s) return TUA_E_NOMEM;
    *out_s = s;
    return TUA_OK;
#endif
}
