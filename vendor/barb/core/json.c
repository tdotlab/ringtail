// JSON: see json.h.
#include "json.h"
#include <string.h>

enum { M_VALUE, M_VALUE_OR_END, M_KEY, M_KEY_OR_END, M_AFTER };

void json_open(json_t* j, str_t src) {
    memset(j, 0, sizeof *j);
    j->src = src;
    j->mode = M_VALUE;
}

static jtok_t fail(json_t* j, const char* msg) {
    if (!j->err) { j->err = msg; j->err_pos = j->pos; }
    return JT_ERR;
}

static int is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
static int is_digit(char c) { return c >= '0' && c <= '9'; }
static int is_hex(char c) {
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// pos is at the opening quote. Leaves pos after the closing one.
static int scan_string(json_t* j) {
    const char* s = j->src.p;
    size_t n = j->src.n, i = j->pos + 1, start = i;
    for (;;) {
        if (i >= n) { j->pos = i; fail(j, "unterminated string"); return 0; }
        unsigned char c = (unsigned char)s[i];
        if (c == '"') break;
        if (c < 0x20) { j->pos = i; fail(j, "control character in string"); return 0; }
        if (c == '\\') {
            if (i + 1 >= n) { j->pos = i; fail(j, "unterminated string"); return 0; }
            char e = s[i + 1];
            if (e == 'u') {
                if (i + 5 >= n) { j->pos = i; fail(j, "short \\u escape"); return 0; }
                for (int k = 2; k < 6; k++)
                    if (!is_hex(s[i + k])) { j->pos = i; fail(j, "bad \\u escape"); return 0; }
                i += 6;
                continue;
            }
            if (!(e == '"' || e == '\\' || e == '/' || e == 'b' || e == 'f' ||
                  e == 'n' || e == 'r' || e == 't')) {
                j->pos = i; fail(j, "bad escape"); return 0;
            }
            i += 2;
            continue;
        }
        i++;
    }
    j->tok = str_of(s + start, i - start);
    j->pos = i + 1;
    return 1;
}

static int scan_number(json_t* j) {
    const char* s = j->src.p;
    size_t n = j->src.n, i = j->pos, start = i;
    if (i < n && s[i] == '-') i++;
    if (i >= n || !is_digit(s[i])) { j->pos = i; fail(j, "bad number"); return 0; }
    if (s[i] == '0') {
        i++;
        if (i < n && is_digit(s[i])) { j->pos = i; fail(j, "leading zero in number"); return 0; }
    } else {
        while (i < n && is_digit(s[i])) i++;
    }
    if (i < n && s[i] == '.') {
        i++;
        if (i >= n || !is_digit(s[i])) { j->pos = i; fail(j, "bad number"); return 0; }
        while (i < n && is_digit(s[i])) i++;
    }
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        i++;
        if (i < n && (s[i] == '+' || s[i] == '-')) i++;
        if (i >= n || !is_digit(s[i])) { j->pos = i; fail(j, "bad number"); return 0; }
        while (i < n && is_digit(s[i])) i++;
    }
    j->tok = str_of(s + start, i - start);
    j->pos = i;
    return 1;
}

static int scan_word(json_t* j, const char* w) {
    size_t len = strlen(w);
    if (j->src.n - j->pos < len || memcmp(j->src.p + j->pos, w, len) != 0) {
        fail(j, "bad literal");
        return 0;
    }
    j->pos += len;
    // "truex" is not a literal followed by x; the next token will be refused anyway,
    // but "true1" would read as two tokens, so insist on a delimiter here.
    if (j->pos < j->src.n) {
        char c = j->src.p[j->pos];
        if (!(is_ws(c) || c == ',' || c == ']' || c == '}')) { fail(j, "bad literal"); return 0; }
    }
    return 1;
}

static jtok_t after_value(json_t* j, jtok_t t) {
    j->mode = M_AFTER;
    return t;
}

jtok_t json_next(json_t* j) {
    if (j->err) return JT_ERR;
    for (;;) {
        while (j->pos < j->src.n && is_ws(j->src.p[j->pos])) j->pos++;
        if (j->pos >= j->src.n) {
            if (j->depth == 0 && j->mode == M_AFTER) return JT_END;
            return fail(j, "unexpected end of input");
        }
        char c = j->src.p[j->pos];
        j->tok_start = j->pos;

        if (j->mode == M_AFTER) {
            if (j->depth == 0) return fail(j, "data after the value");
            int arr = j->is_arr[j->depth - 1];
            if (c == ',') {
                j->pos++;
                j->mode = arr ? M_VALUE : M_KEY;
                continue;
            }
            if (c == (arr ? ']' : '}')) {
                j->pos++;
                j->depth--;
                return after_value(j, arr ? JT_ARR_END : JT_OBJ_END);
            }
            return fail(j, arr ? "expected , or ]" : "expected , or }");
        }

        if (j->mode == M_KEY || j->mode == M_KEY_OR_END) {
            if (c == '}' && j->mode == M_KEY_OR_END) {
                j->pos++;
                j->depth--;
                return after_value(j, JT_OBJ_END);
            }
            if (c != '"') return fail(j, "expected a key");
            if (!scan_string(j)) return JT_ERR;
            while (j->pos < j->src.n && is_ws(j->src.p[j->pos])) j->pos++;
            if (j->pos >= j->src.n || j->src.p[j->pos] != ':') return fail(j, "expected :");
            j->pos++;
            j->mode = M_VALUE;
            return JT_KEY;
        }

        // M_VALUE or M_VALUE_OR_END
        if (c == ']' && j->mode == M_VALUE_OR_END) {
            j->pos++;
            j->depth--;
            return after_value(j, JT_ARR_END);
        }
        switch (c) {
        case '{':
        case '[':
            if (j->depth >= JSON_MAX_DEPTH) return fail(j, "nested too deeply");
            j->is_arr[j->depth++] = (c == '[');
            j->pos++;
            j->mode = (c == '[') ? M_VALUE_OR_END : M_KEY_OR_END;
            return c == '[' ? JT_ARR : JT_OBJ;
        case '"':
            if (!scan_string(j)) return JT_ERR;
            return after_value(j, JT_STR);
        case 't': if (!scan_word(j, "true")) return JT_ERR; return after_value(j, JT_TRUE);
        case 'f': if (!scan_word(j, "false")) return JT_ERR; return after_value(j, JT_FALSE);
        case 'n': if (!scan_word(j, "null")) return JT_ERR; return after_value(j, JT_NULL);
        default:
            if (c == '-' || is_digit(c)) {
                if (!scan_number(j)) return JT_ERR;
                return after_value(j, JT_NUM);
            }
            return fail(j, "unexpected character");
        }
    }
}

int json_skip(json_t* j, jtok_t first) {
    if (first == JT_ERR || first == JT_END) return 0;
    if (first == JT_OBJ || first == JT_ARR) {
        int target = j->depth - 1;
        while (j->depth > target) {
            jtok_t t = json_next(j);
            if (t == JT_ERR || t == JT_END) return 0;
        }
    }
    return 1;
}

int json_capture(json_t* j, jtok_t first, str_t* out) {
    size_t start = j->tok_start;
    if (!json_skip(j, first)) return 0;
    *out = str_of(j->src.p + start, j->pos - start);
    return 1;
}

int json_get(str_t obj, const char* key, str_t* val) {
    json_t j;
    json_open(&j, obj);
    if (json_next(&j) != JT_OBJ) return 0;
    size_t klen = strlen(key);
    for (;;) {
        jtok_t t = json_next(&j);
        if (t == JT_OBJ_END) return 0;
        if (t != JT_KEY) return 0;
        int match = j.tok.n == klen && memcmp(j.tok.p, key, klen) == 0;
        jtok_t v = json_next(&j);
        str_t raw;
        if (v == JT_ERR || v == JT_END || v == JT_OBJ_END || v == JT_ARR_END || v == JT_KEY) return 0;
        if (!json_capture(&j, v, &raw)) return 0;
        if (match) { *val = raw; return 1; }
    }
}

static void put_utf8(char* out, size_t* n, uint32_t cp) {
    if (cp < 0x80) {
        out[(*n)++] = (char)cp;
    } else if (cp < 0x800) {
        out[(*n)++] = (char)(0xC0 | (cp >> 6));
        out[(*n)++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out[(*n)++] = (char)(0xE0 | (cp >> 12));
        out[(*n)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[(*n)++] = (char)(0x80 | (cp & 0x3F));
    } else {
        out[(*n)++] = (char)(0xF0 | (cp >> 18));
        out[(*n)++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[(*n)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[(*n)++] = (char)(0x80 | (cp & 0x3F));
    }
}

static uint32_t hex4(const char* p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v = v * 16 + (uint32_t)(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
    }
    return v;
}

str_t json_unescape(arena_t* a, str_t raw) {
    // Decoding never makes the text longer: \uXXXX (6 bytes) gives at most 3,
    // and a surrogate pair (12 bytes) gives 4.
    char* out = arena_alloc(a, raw.n + 1);
    if (!out) return str_of(0, 0);
    size_t o = 0;
    for (size_t i = 0; i < raw.n;) {
        char c = raw.p[i];
        if (c != '\\') { out[o++] = c; i++; continue; }
        char e = raw.p[i + 1];
        i += 2;
        switch (e) {
        case 'b': out[o++] = '\b'; break;
        case 'f': out[o++] = '\f'; break;
        case 'n': out[o++] = '\n'; break;
        case 'r': out[o++] = '\r'; break;
        case 't': out[o++] = '\t'; break;
        case 'u': {
            uint32_t cp = hex4(raw.p + i);
            i += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                if (i + 6 <= raw.n && raw.p[i] == '\\' && raw.p[i + 1] == 'u') {
                    uint32_t lo = hex4(raw.p + i + 2);
                    if (lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        i += 6;
                    } else {
                        cp = 0xFFFD;
                    }
                } else {
                    cp = 0xFFFD;
                }
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                cp = 0xFFFD;
            }
            put_utf8(out, &o, cp);
            break;
        }
        default: out[o++] = e; break;  // " \ /
        }
    }
    return str_of(out, o);
}

int json_as_str(arena_t* a, str_t raw, str_t* out) {
    if (raw.n < 2 || raw.p[0] != '"' || raw.p[raw.n - 1] != '"') return 0;
    str_t s = json_unescape(a, str_of(raw.p + 1, raw.n - 2));
    if (!s.p) return 0;
    *out = s;
    return 1;
}

int json_as_int(str_t raw, int64_t* out) {
    size_t i = 0;
    int neg = 0;
    if (raw.n && raw.p[0] == '-') { neg = 1; i = 1; }
    if (i >= raw.n) return 0;
    uint64_t v = 0;
    for (; i < raw.n; i++) {
        if (!is_digit(raw.p[i])) return 0;
        uint64_t d = (uint64_t)(raw.p[i] - '0');
        if (v > (UINT64_MAX - d) / 10) return 0;
        v = v * 10 + d;
    }
    if (neg) {
        if (v > (uint64_t)INT64_MAX + 1) return 0;
        *out = (int64_t)(0 - v);
    } else {
        if (v > (uint64_t)INT64_MAX) return 0;
        *out = (int64_t)v;
    }
    return 1;
}

int json_as_bool(str_t raw, int* out) {
    if (str_eqz(raw, "true")) { *out = 1; return 1; }
    if (str_eqz(raw, "false")) { *out = 0; return 1; }
    return 0;
}

int json_valid(str_t s) {
    json_t j;
    json_open(&j, s);
    jtok_t t = json_next(&j);
    if (t == JT_ERR || t == JT_END || t == JT_OBJ_END || t == JT_ARR_END || t == JT_KEY) return 0;
    if (!json_skip(&j, t)) return 0;
    return json_next(&j) == JT_END;
}

// ---- writer ----

void jw_init(jw_t* w, sb_t* sb) {
    memset(w, 0, sizeof *w);
    w->sb = sb;
    w->first[0] = 1;
}

// What comes before any value or key: a comma unless it is the first thing
// at this depth, and nothing at all right after a key.
static void pre(jw_t* w) {
    if (w->after_key) { w->after_key = 0; return; }
    if (!w->first[w->depth]) sb_putc(w->sb, ',');
    w->first[w->depth] = 0;
}

static void open(jw_t* w, char c) {
    pre(w);
    sb_putc(w->sb, c);
    if (w->depth >= JW_MAX_DEPTH) { w->bad = 1; return; }
    w->depth++;
    w->first[w->depth] = 1;
}

static void close(jw_t* w, char c) {
    if (w->depth == 0) { w->bad = 1; return; }
    w->depth--;
    sb_putc(w->sb, c);
}

void jw_obj(jw_t* w) { open(w, '{'); }
void jw_obj_end(jw_t* w) { close(w, '}'); }
void jw_arr(jw_t* w) { open(w, '['); }
void jw_arr_end(jw_t* w) { close(w, ']'); }

// The length of the valid UTF-8 sequence at p (n bytes remain), or 0.
static size_t utf8_len(const uint8_t* p, size_t n) {
    uint8_t c = p[0];
    if (c < 0x80) return 1;
    if (c >= 0xC2 && c <= 0xDF) return (n >= 2 && (p[1] & 0xC0) == 0x80) ? 2 : 0;
    if (c >= 0xE0 && c <= 0xEF) {
        if (n < 3 || (p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80) return 0;
        if (c == 0xE0 && p[1] < 0xA0) return 0;   // overlong
        if (c == 0xED && p[1] >= 0xA0) return 0;  // a surrogate
        return 3;
    }
    if (c >= 0xF0 && c <= 0xF4) {
        if (n < 4 || (p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80 || (p[3] & 0xC0) != 0x80) return 0;
        if (c == 0xF0 && p[1] < 0x90) return 0;   // overlong
        if (c == 0xF4 && p[1] >= 0x90) return 0;  // past U+10FFFF
        return 4;
    }
    return 0;
}

static void put_str(jw_t* w, str_t s) {
    static const char hex[] = "0123456789abcdef";
    sb_t* b = w->sb;
    sb_putc(b, '"');
    const uint8_t* p = (const uint8_t*)s.p;
    size_t run = 0;  // start of the bytes not yet copied, to copy them in big pieces
    for (size_t i = 0; i < s.n;) {
        uint8_t c = p[i];
        if (c >= 0x20 && c != '"' && c != '\\' && c < 0x80) { i++; continue; }
        if (c >= 0x80) {
            size_t len = utf8_len(p + i, s.n - i);
            if (len) { i += len; continue; }
        }
        sb_put(b, p + run, i - run);
        if (c == '"') sb_putz(b, "\\\"");
        else if (c == '\\') sb_putz(b, "\\\\");
        else if (c == '\n') sb_putz(b, "\\n");
        else if (c == '\r') sb_putz(b, "\\r");
        else if (c == '\t') sb_putz(b, "\\t");
        else if (c < 0x20) {
            char t[6] = { '\\', 'u', '0', '0', hex[c >> 4], hex[c & 15] };
            sb_put(b, t, 6);
        } else {
            sb_putz(b, "\\ufffd");  // a byte that is not valid UTF-8
        }
        i++;
        run = i;
    }
    sb_put(b, p + run, s.n - run);
    sb_putc(b, '"');
}

void jw_key(jw_t* w, const char* key) {
    pre(w);
    put_str(w, str_z(key));
    sb_putc(w->sb, ':');
    w->after_key = 1;
}

void jw_str(jw_t* w, str_t s) { pre(w); put_str(w, s); }
void jw_strz(jw_t* w, const char* z) { jw_str(w, str_z(z)); }
void jw_int(jw_t* w, int64_t v) { pre(w); sb_puti(w->sb, v); }
void jw_bool(jw_t* w, int v) { pre(w); sb_putz(w->sb, v ? "true" : "false"); }
void jw_null(jw_t* w) { pre(w); sb_putz(w->sb, "null"); }
void jw_raw(jw_t* w, str_t json) { pre(w); sb_puts(w->sb, json); }
