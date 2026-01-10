#include "tua_array.h"
#include "tua_bytes.h"
#include "tua_map.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Minimal UTF-8 decoder (strict enough for tokenizer vocab entries).
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

// GPT-2 byte<->unicode mapping inverse:
// In tokenizer JSON, vocab entries use the "bytes_to_unicode" mapping where each original byte
// is represented by a Unicode codepoint; decoding maps each codepoint back to one output byte.
static int32_t gpt2_bytes_to_unicode_inv(int32_t cp) {
    // Mapping range is small (<= 323); keep a small static table.
    static int inited = 0;
    static int8_t inv[1024];
    if (!inited) {
        for (int i = 0; i < (int)(sizeof(inv) / sizeof(inv[0])); i++) inv[i] = -1;
        int nextCp = 256;
        for (int b = 0; b < 256; b++) {
            int mapped = -1;
            if (b >= 33 && b <= 126) mapped = b;
            else if (b >= 161 && b <= 172) mapped = b;
            else if (b >= 174 && b <= 255) mapped = b;
            else mapped = nextCp++;
            if (mapped >= 0 && mapped < (int)(sizeof(inv) / sizeof(inv[0]))) {
                inv[mapped] = (int8_t)b;
            }
        }
        inited = 1;
    }
    if (cp < 0 || cp >= (int32_t)(sizeof(inv) / sizeof(inv[0]))) return -1;
    return (int32_t)inv[cp];
}

// External decode: ids + idToToken -> malloc'd C string.
// Signature in Tua:
//   extern fn tuaext_bpe_decode_ids_to_string(ids: long[], idToToken: map<long, string>, outErr: &int) string
char* tuaext_bpe_decode_ids_to_string(tua_array* ids, tua_map* idToToken, int32_t* outErr) {
    if (outErr) *outErr = 1;
    if (!outErr) return NULL;
    if (!ids || !idToToken) return NULL;
    if (ids->elem_size != (int64_t)sizeof(int64_t)) return NULL;
    if (ids->len < 0) return NULL;
    if (ids->len > 0 && !ids->data) return NULL;

    const int64_t* idp = (const int64_t*)ids->data;
    int64_t n = ids->len;

    // Pass 1: compute output length (1 output byte per codepoint in token string).
    int64_t outLen = 0;
    for (int64_t i = 0; i < n; i++) {
        int32_t ok = 0;
        tua_value v = tua_map_get_with_ok(idToToken, tua_value_long(idp[i]), &ok);
        if (!ok) return NULL;
        const char* tok = tua_value_to_string(v);
        if (!tok) return NULL;
        size_t j = 0;
        while (tok[j] != '\0') {
            int32_t cp = 0;
            if (!utf8_decode1_strict(tok, &j, &cp)) return NULL;
            if (gpt2_bytes_to_unicode_inv(cp) < 0) return NULL;
            outLen++;
        }
    }

    if (outLen < 0) return NULL;
    if (outLen == 0) {
        char* out = (char*)malloc(1);
        if (!out) tua_panic("out of memory");
        out[0] = '\0';
        *outErr = 0;
        return out;
    }
    if ((uint64_t)outLen > (uint64_t)SIZE_MAX - 1) return NULL;
    char* out = (char*)malloc((size_t)outLen + 1);
    if (!out) tua_panic("out of memory");

    // Pass 2: fill output.
    int64_t off = 0;
    for (int64_t i = 0; i < n; i++) {
        int32_t ok = 0;
        tua_value v = tua_map_get_with_ok(idToToken, tua_value_long(idp[i]), &ok);
        if (!ok) {
            free(out);
            return NULL;
        }
        const char* tok = tua_value_to_string(v);
        if (!tok) {
            free(out);
            return NULL;
        }
        size_t j = 0;
        while (tok[j] != '\0') {
            int32_t cp = 0;
            if (!utf8_decode1_strict(tok, &j, &cp)) {
                free(out);
                return NULL;
            }
            int32_t b = gpt2_bytes_to_unicode_inv(cp);
            if (b < 0) {
                free(out);
                return NULL;
            }
            if (off < outLen) out[off] = (char)(uint8_t)b;
            off++;
        }
    }
    if (off != outLen) {
        free(out);
        return NULL;
    }
    out[(size_t)outLen] = '\0';
    *outErr = 0;
    return out;
}

