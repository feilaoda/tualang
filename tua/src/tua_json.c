#include "tua_json.h"

#include "rt/rt_alloc.h"
#include "tua_bytes.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

static int is_ws(uint8_t c) {
    return c == 32 || c == 10 || c == 13 || c == 9;
}

static int is_digit(uint8_t c) {
    return c >= '0' && c <= '9';
}

static int is_hex(uint8_t c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}

static int hex_val(uint8_t c) {
    if (c >= '0' && c <= '9') return (int)(c - '0');
    if (c >= 'A' && c <= 'F') return 10 + (int)(c - 'A');
    return 10 + (int)(c - 'a');
}

static int skip_ws(const uint8_t* p, int64_t n, int64_t* io) {
    int64_t i = *io;
    while (i < n && is_ws(p[i])) i++;
    *io = i;
    return 0;
}

static int parse_string_end(const uint8_t* p, int64_t n, int64_t pos_quote, int64_t* out_end_quote, int* out_has_esc) {
    // pos_quote points at '"'
    if (pos_quote < 0 || pos_quote >= n) return 1;
    if (p[pos_quote] != '"') return 1;
    int64_t i = pos_quote + 1;
    int hasEsc = 0;
    while (i < n) {
        uint8_t c = p[i];
        if (c == '\\') {
            hasEsc = 1;
            if (i + 1 >= n) return 1;
            i += 2;
            continue;
        }
        if (c == '"') {
            *out_end_quote = i;
            *out_has_esc = hasEsc;
            return 0;
        }
        if (c <= 31) return 1;
        i++;
    }
    return 1;
}

static int utf8_encode(int32_t cp, char out[4], int* out_n) {
    if (cp < 0 || cp > 0x10FFFF) return 1;
    if (cp >= 0xD800 && cp <= 0xDFFF) return 1;
    if (cp <= 0x7F) {
        out[0] = (char)cp;
        *out_n = 1;
        return 0;
    }
    if (cp <= 0x7FF) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        *out_n = 2;
        return 0;
    }
    if (cp <= 0xFFFF) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        *out_n = 3;
        return 0;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    *out_n = 4;
    return 0;
}

static int decode_string_alloc(tua_bytes* b, const uint8_t* p, int64_t n, int64_t start, int64_t end_quote, char** out) {
    // start..end_quote is the raw content (no quotes). Output length <= raw length.
    int64_t rawLen = end_quote - start;
    if (rawLen < 0) return 1;
    char* buf = (char*)tua_malloc((size_t)rawLen + 1);
    if (!buf) return 1;

    int64_t i = start;
    int64_t o = 0;
    while (i < end_quote) {
        uint8_t c = p[i];
        if (c != '\\') {
            buf[o++] = (char)c;
            i++;
            continue;
        }
        if (i + 1 >= end_quote) {
            tua_free(buf);
            return 1;
        }
        uint8_t esc = p[i + 1];
        if (esc == '"' || esc == '\\' || esc == '/') {
            buf[o++] = (char)esc;
            i += 2;
            continue;
        }
        if (esc == 'b') { buf[o++] = 8; i += 2; continue; }
        if (esc == 'f') { buf[o++] = 12; i += 2; continue; }
        if (esc == 'n') { buf[o++] = 10; i += 2; continue; }
        if (esc == 'r') { buf[o++] = 13; i += 2; continue; }
        if (esc == 't') { buf[o++] = 9; i += 2; continue; }
        if (esc == 'u') {
            if (i + 6 > end_quote) { tua_free(buf); return 1; }
            uint8_t h0 = p[i + 2], h1 = p[i + 3], h2 = p[i + 4], h3 = p[i + 5];
            if (!is_hex(h0) || !is_hex(h1) || !is_hex(h2) || !is_hex(h3)) { tua_free(buf); return 1; }
            int32_t cp = (int32_t)((hex_val(h0) << 12) | (hex_val(h1) << 8) | (hex_val(h2) << 4) | hex_val(h3));
            i += 6;
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                // High surrogate, must be followed by \uXXXX low surrogate.
                if (i + 6 > end_quote) { tua_free(buf); return 1; }
                if (p[i] != '\\' || p[i + 1] != 'u') { tua_free(buf); return 1; }
                uint8_t l0 = p[i + 2], l1 = p[i + 3], l2 = p[i + 4], l3 = p[i + 5];
                if (!is_hex(l0) || !is_hex(l1) || !is_hex(l2) || !is_hex(l3)) { tua_free(buf); return 1; }
                int32_t low = (int32_t)((hex_val(l0) << 12) | (hex_val(l1) << 8) | (hex_val(l2) << 4) | hex_val(l3));
                if (low < 0xDC00 || low > 0xDFFF) { tua_free(buf); return 1; }
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                i += 6;
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                tua_free(buf);
                return 1;
            }
            char tmp[4];
            int wrote = 0;
            if (utf8_encode(cp, tmp, &wrote) != 0) { tua_free(buf); return 1; }
            for (int k = 0; k < wrote; k++) buf[o++] = tmp[k];
            continue;
        }
        tua_free(buf);
        return 1;
    }
    buf[o] = '\0';
    *out = buf;
    (void)b;
    (void)n;
    return 0;
}

static int string_equals_key_span(tua_bytes* b, const uint8_t* p, int64_t n, int64_t pos_quote, int64_t end_quote, int has_esc, const char* key, size_t keyLen) {
    (void)b;
    if (!key) return 0;
    int64_t rawOff = pos_quote + 1;
    int64_t rawLen = end_quote - rawOff;
    if (rawLen < 0) return 0;

    if (!has_esc) {
        if ((size_t)rawLen != keyLen) return 0;
        return memcmp(p + rawOff, key, keyLen) == 0;
    }

    // Decode on the fly and compare to UTF-8 key bytes.
    size_t ki = 0;
    int64_t i = rawOff;
    while (i < end_quote) {
        uint8_t c = p[i];
        if (c != '\\') {
            if (ki >= keyLen || (uint8_t)key[ki] != c) return 0;
            ki++;
            i++;
            continue;
        }
        if (i + 1 >= end_quote) return 0;
        uint8_t esc = p[i + 1];
        if (esc == '"' || esc == '\\' || esc == '/') {
            if (ki >= keyLen || (uint8_t)key[ki] != esc) return 0;
            ki++;
            i += 2;
            continue;
        }
        if (esc == 'b' || esc == 'f' || esc == 'n' || esc == 'r' || esc == 't') {
            uint8_t outc = 0;
            if (esc == 'b') outc = 8;
            else if (esc == 'f') outc = 12;
            else if (esc == 'n') outc = 10;
            else if (esc == 'r') outc = 13;
            else outc = 9;
            if (ki >= keyLen || (uint8_t)key[ki] != outc) return 0;
            ki++;
            i += 2;
            continue;
        }
        if (esc == 'u') {
            if (i + 6 > end_quote) return 0;
            uint8_t h0 = p[i + 2], h1 = p[i + 3], h2 = p[i + 4], h3 = p[i + 5];
            if (!is_hex(h0) || !is_hex(h1) || !is_hex(h2) || !is_hex(h3)) return 0;
            int32_t cp = (int32_t)((hex_val(h0) << 12) | (hex_val(h1) << 8) | (hex_val(h2) << 4) | hex_val(h3));
            i += 6;
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                if (i + 6 > end_quote) return 0;
                if (p[i] != '\\' || p[i + 1] != 'u') return 0;
                uint8_t l0 = p[i + 2], l1 = p[i + 3], l2 = p[i + 4], l3 = p[i + 5];
                if (!is_hex(l0) || !is_hex(l1) || !is_hex(l2) || !is_hex(l3)) return 0;
                int32_t low = (int32_t)((hex_val(l0) << 12) | (hex_val(l1) << 8) | (hex_val(l2) << 4) | hex_val(l3));
                if (low < 0xDC00 || low > 0xDFFF) return 0;
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                i += 6;
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                return 0;
            }
            char tmp[4];
            int wrote = 0;
            if (utf8_encode(cp, tmp, &wrote) != 0) return 0;
            for (int k = 0; k < wrote; k++) {
                if (ki >= keyLen || key[ki] != tmp[k]) return 0;
                ki++;
            }
            continue;
        }
        return 0;
    }
    return ki == keyLen;
}

static int string_equals_key(tua_bytes* b, const uint8_t* p, int64_t n, int64_t pos_quote, int64_t end_quote, int has_esc, const char* key) {
    if (!key) return 0;
    return string_equals_key_span(b, p, n, pos_quote, end_quote, has_esc, key, strlen(key));
}

static int skip_string(const uint8_t* p, int64_t n, int64_t* io) {
    int64_t endq = 0;
    int hasEsc = 0;
    int64_t pos = *io;
    if (parse_string_end(p, n, pos, &endq, &hasEsc) != 0) return 1;
    *io = endq + 1;
    (void)hasEsc;
    return 0;
}

static int skip_number(const uint8_t* p, int64_t n, int64_t* io) {
    int64_t i = *io;
    if (i >= n) return 1;
    if (p[i] == '-') {
        i++;
        if (i >= n) return 1;
    }
    if (p[i] == '0') {
        i++;
    } else if (p[i] >= '1' && p[i] <= '9') {
        while (i < n && is_digit(p[i])) i++;
    } else {
        return 1;
    }
    if (i < n && p[i] == '.') {
        i++;
        if (i >= n || !is_digit(p[i])) return 1;
        while (i < n && is_digit(p[i])) i++;
    }
    if (i < n && (p[i] == 'e' || p[i] == 'E')) {
        i++;
        if (i >= n) return 1;
        if (p[i] == '+' || p[i] == '-') i++;
        if (i >= n || !is_digit(p[i])) return 1;
        while (i < n && is_digit(p[i])) i++;
    }
    *io = i;
    return 0;
}

static int skip_value(const uint8_t* p, int64_t n, int64_t* io, int depth) {
    if (depth > 512) return 1;
    skip_ws(p, n, io);
    int64_t i = *io;
    if (i >= n) return 1;
    uint8_t c = p[i];
    if (c == '"') {
        if (skip_string(p, n, io) != 0) return 1;
        return 0;
    }
    if (c == '{') {
        i++;
        *io = i;
        skip_ws(p, n, io);
        i = *io;
        if (i >= n) return 1;
        if (p[i] == '}') {
            *io = i + 1;
            return 0;
        }
        for (;;) {
            skip_ws(p, n, io);
            i = *io;
            if (i >= n || p[i] != '"') return 1;
            if (skip_string(p, n, io) != 0) return 1;
            skip_ws(p, n, io);
            i = *io;
            if (i >= n || p[i] != ':') return 1;
            *io = i + 1;
            if (skip_value(p, n, io, depth + 1) != 0) return 1;
            skip_ws(p, n, io);
            i = *io;
            if (i >= n) return 1;
            if (p[i] == ',') {
                *io = i + 1;
                continue;
            }
            if (p[i] == '}') {
                *io = i + 1;
                return 0;
            }
            return 1;
        }
    }
    if (c == '[') {
        i++;
        *io = i;
        skip_ws(p, n, io);
        i = *io;
        if (i >= n) return 1;
        if (p[i] == ']') {
            *io = i + 1;
            return 0;
        }
        for (;;) {
            if (skip_value(p, n, io, depth + 1) != 0) return 1;
            skip_ws(p, n, io);
            i = *io;
            if (i >= n) return 1;
            if (p[i] == ',') {
                *io = i + 1;
                continue;
            }
            if (p[i] == ']') {
                *io = i + 1;
                return 0;
            }
            return 1;
        }
    }
    if (c == 't') {
        if (i + 4 > n) return 1;
        if (p[i + 1] != 'r' || p[i + 2] != 'u' || p[i + 3] != 'e') return 1;
        *io = i + 4;
        return 0;
    }
    if (c == 'f') {
        if (i + 5 > n) return 1;
        if (p[i + 1] != 'a' || p[i + 2] != 'l' || p[i + 3] != 's' || p[i + 4] != 'e') return 1;
        *io = i + 5;
        return 0;
    }
    if (c == 'n') {
        if (i + 4 > n) return 1;
        if (p[i + 1] != 'u' || p[i + 2] != 'l' || p[i + 3] != 'l') return 1;
        *io = i + 4;
        return 0;
    }
    if (c == '-' || is_digit(c)) {
        if (skip_number(p, n, io) != 0) return 1;
        return 0;
    }
    return 1;
}

static int scan_top_level_find_value_pos(tua_bytes* b, const uint8_t* p, int64_t n, const char* key, int64_t* out_value_pos) {
    (void)b;
    if (!p || !key || !out_value_pos) return 1;
    *out_value_pos = 0;

    int64_t i = 0;
    skip_ws(p, n, &i);
    if (i >= n || p[i] != '{') return 1;
    i++;
    skip_ws(p, n, &i);
    if (i >= n) return 1;
    if (p[i] == '}') return 2;

    for (;;) {
        skip_ws(p, n, &i);
        if (i >= n || p[i] != '"') return 1;

        int64_t endq = 0;
        int hasEsc = 0;
        if (parse_string_end(p, n, i, &endq, &hasEsc) != 0) return 1;
        int match = string_equals_key(b, p, n, i, endq, hasEsc, key);
        i = endq + 1;

        skip_ws(p, n, &i);
        if (i >= n || p[i] != ':') return 1;
        i++;
        skip_ws(p, n, &i);
        if (i >= n) return 1;

        if (match) {
            *out_value_pos = i;
            return 0;
        }

        if (skip_value(p, n, &i, 0) != 0) return 1;
        skip_ws(p, n, &i);
        if (i >= n) return 1;
        if (p[i] == ',') {
            i++;
            continue;
        }
        if (p[i] == '}') return 2;
        return 1;
    }
}

static int scan_object_find_value_pos_span(
    tua_bytes* b,
    const uint8_t* p,
    int64_t n,
    int64_t obj_pos,
    const char* key,
    size_t key_len,
    int64_t* out_value_pos
) {
    (void)b;
    if (!p || !key || !out_value_pos) return 1;
    *out_value_pos = 0;

    int64_t i = obj_pos;
    skip_ws(p, n, &i);
    if (i >= n || p[i] != '{') return 1;
    i++;
    skip_ws(p, n, &i);
    if (i >= n) return 1;
    if (p[i] == '}') return 2;

    for (;;) {
        skip_ws(p, n, &i);
        if (i >= n || p[i] != '"') return 1;

        int64_t endq = 0;
        int hasEsc = 0;
        if (parse_string_end(p, n, i, &endq, &hasEsc) != 0) return 1;
        int match = string_equals_key_span(b, p, n, i, endq, hasEsc, key, key_len);
        i = endq + 1;

        skip_ws(p, n, &i);
        if (i >= n || p[i] != ':') return 1;
        i++;
        skip_ws(p, n, &i);
        if (i >= n) return 1;

        if (match) {
            *out_value_pos = i;
            return 0;
        }

        if (skip_value(p, n, &i, 0) != 0) return 1;
        skip_ws(p, n, &i);
        if (i >= n) return 1;
        if (p[i] == ',') {
            i++;
            continue;
        }
        if (p[i] == '}') return 2;
        return 1;
    }
}

static int parse_int64_strict(const uint8_t* p, int64_t n, int64_t pos, int64_t* out_val, int64_t* out_end) {
    if (!p || !out_val || !out_end) return 1;
    *out_val = 0;
    *out_end = pos;
    if (pos >= n) return 1;

    int64_t i = pos;
    int negative = 0;
    if (p[i] == '-') {
        negative = 1;
        i++;
        if (i >= n) return 1;
    }
    if (!is_digit(p[i])) return 1;

    int64_t acc = 0;
    while (i < n && is_digit(p[i])) {
        int digit = (int)(p[i] - '0');
        if (!negative) {
            if (acc > (INT64_MAX - digit) / 10) return 1;
            acc = acc * 10 + digit;
        } else {
            if (acc < (INT64_MIN + digit) / 10) return 1;
            acc = acc * 10 - digit;
        }
        i++;
    }
    // Reject float/exponent forms.
    if (i < n && (p[i] == '.' || p[i] == 'e' || p[i] == 'E')) return 1;

    *out_val = acc;
    *out_end = i;
    return 0;
}

static int check_object_value_boundary(const uint8_t* p, int64_t n, int64_t end_pos) {
    int64_t i = end_pos;
    skip_ws(p, n, &i);
    if (i >= n) return 1;
    return (p[i] == ',' || p[i] == '}') ? 0 : 1;
}

int32_t tua_json_scan_top_level_string(tua_bytes* b, const char* key, char** out_str) {
    if (out_str) *out_str = NULL;
    if (!out_str || !b || !key) return 1;

    int64_t n = tua_bytes_len(b);
    uint8_t* data = tua_bytes_data(b);
    if (n < 0 || (n > 0 && !data)) return 1;
    const uint8_t* p = (const uint8_t*)data;

    int64_t valuePos = 0;
    int32_t fe = (int32_t)scan_top_level_find_value_pos(b, p, n, key, &valuePos);
    if (fe != 0) return fe;

    if (valuePos >= n || p[valuePos] != '"') return 1;
    int64_t vendq = 0;
    int vHasEsc = 0;
    if (parse_string_end(p, n, valuePos, &vendq, &vHasEsc) != 0) return 1;
    int64_t rawOff = valuePos + 1;
    int64_t rawLen = vendq - rawOff;
    if (rawLen < 0) return 1;
    char* s = NULL;
    if (!vHasEsc) {
        s = tua_str_from_bytes_copy(b, rawOff, rawLen);
        if (!s) return 1;
    } else {
        if (decode_string_alloc(b, p, n, rawOff, vendq, &s) != 0) return 1;
    }
    if (check_object_value_boundary(p, n, vendq + 1) != 0) {
        tua_free(s);
        return 1;
    }
    *out_str = s;
    return 0;
}

int32_t tua_json_scan_top_level_long(tua_bytes* b, const char* key, int64_t* out_val) {
    if (out_val) *out_val = 0;
    if (!out_val || !b || !key) return 1;

    int64_t n = tua_bytes_len(b);
    uint8_t* data = tua_bytes_data(b);
    if (n < 0 || (n > 0 && !data)) return 1;
    const uint8_t* p = (const uint8_t*)data;

    int64_t valuePos = 0;
    int32_t fe = (int32_t)scan_top_level_find_value_pos(b, p, n, key, &valuePos);
    if (fe != 0) return fe;

    int64_t endPos = 0;
    int64_t v = 0;
    if (parse_int64_strict(p, n, valuePos, &v, &endPos) != 0) return 1;
    if (check_object_value_boundary(p, n, endPos) != 0) return 1;
    *out_val = v;
    return 0;
}

int32_t tua_json_scan_top_level_bool(tua_bytes* b, const char* key, int32_t* out_bool) {
    if (out_bool) *out_bool = 0;
    if (!out_bool || !b || !key) return 1;

    int64_t n = tua_bytes_len(b);
    uint8_t* data = tua_bytes_data(b);
    if (n < 0 || (n > 0 && !data)) return 1;
    const uint8_t* p = (const uint8_t*)data;

    int64_t valuePos = 0;
    int32_t fe = (int32_t)scan_top_level_find_value_pos(b, p, n, key, &valuePos);
    if (fe != 0) return fe;

    if (valuePos >= n) return 1;
    if (p[valuePos] == 't') {
        if (valuePos + 4 > n) return 1;
        if (p[valuePos + 1] != 'r' || p[valuePos + 2] != 'u' || p[valuePos + 3] != 'e') return 1;
        if (check_object_value_boundary(p, n, valuePos + 4) != 0) return 1;
        *out_bool = 1;
        return 0;
    }
    if (p[valuePos] == 'f') {
        if (valuePos + 5 > n) return 1;
        if (p[valuePos + 1] != 'a' || p[valuePos + 2] != 'l' || p[valuePos + 3] != 's' || p[valuePos + 4] != 'e') return 1;
        if (check_object_value_boundary(p, n, valuePos + 5) != 0) return 1;
        *out_bool = 0;
        return 0;
    }
    return 1;
}

static int scan_key_path_find_value_pos(tua_bytes* b, const uint8_t* p, int64_t n, const char* path, int64_t* out_value_pos) {
    if (!b || !p || !path || !out_value_pos) return 1;
    *out_value_pos = 0;

    int64_t objPos = 0;
    skip_ws(p, n, &objPos);
    if (objPos >= n || p[objPos] != '{') return 1;

    const char* seg = path;
    const char* cur = path;
    while (*cur) {
        if (*cur == '.') break;
        cur++;
    }
    if (cur == seg) return 1;

    for (;;) {
        size_t segLen = (size_t)(cur - seg);
        int64_t valuePos = 0;
        int r = scan_object_find_value_pos_span(b, p, n, objPos, seg, segLen, &valuePos);
        if (r != 0) return r;

        if (*cur == '\0') {
            *out_value_pos = valuePos;
            return 0;
        }

        // More segments: value must be an object.
        if (valuePos >= n || p[valuePos] != '{') return 1;
        objPos = valuePos;

        // Advance to next segment.
        cur++; // skip '.'
        seg = cur;
        if (*seg == '\0') return 1;
        while (*cur) {
            if (*cur == '.') break;
            cur++;
        }
        if (cur == seg) return 1;
    }
}

int32_t tua_json_scan_key_path_string(tua_bytes* b, const char* path, char** out_str) {
    if (out_str) *out_str = NULL;
    if (!out_str || !b || !path) return 1;

    int64_t n = tua_bytes_len(b);
    uint8_t* data = tua_bytes_data(b);
    if (n < 0 || (n > 0 && !data)) return 1;
    const uint8_t* p = (const uint8_t*)data;

    int64_t valuePos = 0;
    int32_t fe = (int32_t)scan_key_path_find_value_pos(b, p, n, path, &valuePos);
    if (fe != 0) return fe;

    if (valuePos >= n || p[valuePos] != '"') return 1;
    int64_t vendq = 0;
    int vHasEsc = 0;
    if (parse_string_end(p, n, valuePos, &vendq, &vHasEsc) != 0) return 1;
    int64_t rawOff = valuePos + 1;
    int64_t rawLen = vendq - rawOff;
    if (rawLen < 0) return 1;
    char* s = NULL;
    if (!vHasEsc) {
        s = tua_str_from_bytes_copy(b, rawOff, rawLen);
        if (!s) return 1;
    } else {
        if (decode_string_alloc(b, p, n, rawOff, vendq, &s) != 0) return 1;
    }
    if (check_object_value_boundary(p, n, vendq + 1) != 0) {
        tua_free(s);
        return 1;
    }
    *out_str = s;
    return 0;
}

int32_t tua_json_scan_key_path_long(tua_bytes* b, const char* path, int64_t* out_val) {
    if (out_val) *out_val = 0;
    if (!out_val || !b || !path) return 1;

    int64_t n = tua_bytes_len(b);
    uint8_t* data = tua_bytes_data(b);
    if (n < 0 || (n > 0 && !data)) return 1;
    const uint8_t* p = (const uint8_t*)data;

    int64_t valuePos = 0;
    int32_t fe = (int32_t)scan_key_path_find_value_pos(b, p, n, path, &valuePos);
    if (fe != 0) return fe;

    int64_t endPos = 0;
    int64_t v = 0;
    if (parse_int64_strict(p, n, valuePos, &v, &endPos) != 0) return 1;
    if (check_object_value_boundary(p, n, endPos) != 0) return 1;
    *out_val = v;
    return 0;
}

int32_t tua_json_scan_key_path_bool(tua_bytes* b, const char* path, int32_t* out_bool) {
    if (out_bool) *out_bool = 0;
    if (!out_bool || !b || !path) return 1;

    int64_t n = tua_bytes_len(b);
    uint8_t* data = tua_bytes_data(b);
    if (n < 0 || (n > 0 && !data)) return 1;
    const uint8_t* p = (const uint8_t*)data;

    int64_t valuePos = 0;
    int32_t fe = (int32_t)scan_key_path_find_value_pos(b, p, n, path, &valuePos);
    if (fe != 0) return fe;

    if (valuePos >= n) return 1;
    if (p[valuePos] == 't') {
        if (valuePos + 4 > n) return 1;
        if (p[valuePos + 1] != 'r' || p[valuePos + 2] != 'u' || p[valuePos + 3] != 'e') return 1;
        if (check_object_value_boundary(p, n, valuePos + 4) != 0) return 1;
        *out_bool = 1;
        return 0;
    }
    if (p[valuePos] == 'f') {
        if (valuePos + 5 > n) return 1;
        if (p[valuePos + 1] != 'a' || p[valuePos + 2] != 'l' || p[valuePos + 3] != 's' || p[valuePos + 4] != 'e') return 1;
        if (check_object_value_boundary(p, n, valuePos + 5) != 0) return 1;
        *out_bool = 0;
        return 0;
    }
    return 1;
}
