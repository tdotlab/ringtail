#include "shasum.h"

static int blank(char c) {
    return c == ' ' || c == '\t';
}

// Whether s starts with the 6 characters of "SHA256".
static int is_tag(const char* s) {
    static const char tag[6] = {'S', 'H', 'A', '2', '5', '6'};
    for (int i = 0; i < 6; i++)
        if (s[i] != tag[i]) return 0;
    return 1;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// 64 hex digits at s, or not.
static int hash64(const char* s, uint8_t hash[32]) {
    for (int i = 0; i < 32; i++) {
        int hi = hexval(s[2 * i]), lo = hexval(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return 0;
        hash[i] = (uint8_t)(hi << 4 | lo);
    }
    return 1;
}

// A name that was written with backslashes: only "\\", "\n" and "\r" may
// follow a backslash, and a NUL may not be there at all.
static int unescape(const char* s, uint32_t n, char* name, uint32_t* len) {
    uint32_t o = 0;
    for (uint32_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == 0) return 0;
        if (c == '\\') {
            if (++i == n) return 0;
            if (s[i] == '\\') c = '\\';
            else if (s[i] == 'n') c = '\n';
            else if (s[i] == 'r') c = '\r';
            else return 0;
        }
        name[o++] = c;
    }
    *len = o;
    return 1;
}

int shasum_parse(const char* s, uint32_t n, int* style, uint8_t hash[32], char* name, uint32_t* namelen) {
    if (n && s[n - 1] == '\r') n--;
    if (n == 0 || s[0] == '#') return SHASUM_SKIP;
    uint32_t i = 0;
    while (i < n && blank(s[i])) i++;
    if (i == n) return SHASUM_IMPROPER;
    int escaped = s[i] == '\\';
    if (escaped) i++;
    const char* r = s + i;
    uint32_t rn = n - i;
    const char* nm;
    uint32_t nl;
    if (rn >= 6 && is_tag(r)) {
        // SHA256 (NAME) = HASH: the hash is the last 64 characters, and the
        // name runs to the last ")" before the "=".
        uint32_t j = 6;
        if (j < rn && r[j] == ' ') j++;
        if (j >= rn || r[j] != '(') return SHASUM_IMPROPER;
        uint32_t ns = j + 1;
        if (rn < 64) return SHASUM_IMPROPER;
        uint32_t k = rn - 64;
        if (!hash64(r + k, hash)) return SHASUM_IMPROPER;
        while (k > ns && blank(r[k - 1])) k--;
        if (k <= ns || r[k - 1] != '=') return SHASUM_IMPROPER;
        k--;
        while (k > ns && blank(r[k - 1])) k--;
        if (r[k - 1] != ')') return SHASUM_IMPROPER;
        nm = r + ns;
        nl = k - 1 - ns;
    } else {
        if (rn < 64 || !hash64(r, hash)) return SHASUM_IMPROPER;
        // One blank (GNU lets a NUL do too), then, in the "mode" style, a
        // space or "*" saying how the file was read, then the name. The
        // first line with anything after the blank decides the style.
        if (rn == 64 || !(blank(r[64]) || r[64] == 0)) return SHASUM_IMPROPER;
        nm = r + 65;
        nl = rn - 65;
        if (nl == 0) return SHASUM_IMPROPER;
        int mode = nl >= 2 && (nm[0] == ' ' || nm[0] == '*');
        if (*style == SHASUM_STYLE_NONE) *style = mode ? SHASUM_STYLE_MODE : SHASUM_STYLE_PLAIN;
        if (*style == SHASUM_STYLE_MODE) {
            if (!mode) return SHASUM_IMPROPER;
            nm++;
            nl--;
        }
    }
    if (escaped) {
        if (!unescape(nm, nl, name, namelen)) return SHASUM_IMPROPER;
    } else {
        // GNU holds the line as a C string, so a name stops at a NUL.
        uint32_t o = 0;
        while (o < nl && nm[o]) {
            name[o] = nm[o];
            o++;
        }
        *namelen = o;
    }
    name[*namelen] = 0;
    return SHASUM_LINE;
}

int shasum_needs_escape(const char* name, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        if (name[i] == '\\' || name[i] == '\n' || name[i] == '\r') return 1;
    return 0;
}
