#include "quotearg.h"
#include "ulib.h"

static int is_ctl(unsigned char c) { return c < 0x20 || c >= 0x7f; }

void qa_quote(const char* s, int style, char* o) {
    uint32_t len = (uint32_t)strlen(s);
    char* p = o;
    if (style == QA_C) {
        *p++ = '\'';
        for (uint32_t i = 0; i < len; i++) {
            unsigned char c = (unsigned char)s[i];
            static const char letters[] = "\a\b\f\n\r\t\v", names_[] = "abfnrtv";
            const char* l = c ? strchr(letters, c) : 0;
            if (c == '\\' || c == '\'') {
                *p++ = '\\';
                *p++ = (char)c;
            } else if (l) {
                *p++ = '\\';
                *p++ = names_[l - letters];
            } else if (is_ctl(c)) {
                *p++ = '\\';
                *p++ = (char)('0' + (c >> 6));
                *p++ = (char)('0' + ((c >> 3) & 7));
                *p++ = (char)('0' + (c & 7));
            } else {
                *p++ = (char)c;
            }
        }
        *p++ = '\'';
        *p = 0;
        return;
    }

    int ctl = 0, apos = 0, special = !len, other = 0;
    for (uint32_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (is_ctl(c)) ctl = special = 1;
        if (c == '\'') apos = special = 1;
        if (c && strchr(" !\"$&()*:;<=>?[\\^`|", c)) special = other = 1;
    }
    if ((s[0] == '~' || s[0] == '#') && len) special = other = 1;
    if (style == QA_FILE && !special) {
        memcpy(o, s, len + 1);
        return;
    }
    if (!ctl && apos && !other) {
        *p++ = '"';
        memcpy(p, s, len);
        p += len;
        *p++ = '"';
        *p = 0;
        return;
    }
    if (!ctl && !apos) {
        *p++ = '\'';
        memcpy(p, s, len);
        p += len;
        *p++ = '\'';
        *p = 0;
        return;
    }
    // Single quotes, with $'...' for the odd bytes and '\'' for an apostrophe:
    // 'a'$'\001''b'. An escape that comes first has an empty '' before it.
    int esc = 0;
    *p++ = '\'';
    for (uint32_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (is_ctl(c)) {
            if (!esc) {
                *p++ = '\'';
                *p++ = '$';
                *p++ = '\'';
                esc = 1;
            }
            *p++ = '\\';
            static const char letters[] = "\a\b\t\n\v\f\r", names_[] = "abtnvfr";
            const char* l = c ? strchr(letters, c) : 0;
            if (l) {
                *p++ = names_[l - letters];
            } else {
                *p++ = (char)('0' + (c >> 6));
                *p++ = (char)('0' + ((c >> 3) & 7));
                *p++ = (char)('0' + (c & 7));
            }
            continue;
        }
        if (c == '\'') {
            *p++ = '\'';  // close the quote (or the $'...') ...
            *p++ = '\\';
            *p++ = '\'';
            *p++ = '\'';  // ... say the apostrophe, and open again
            esc = 0;
            continue;
        }
        if (esc) {
            *p++ = '\'';
            *p++ = '\'';
            esc = 0;
        }
        *p++ = (char)c;
    }
    *p++ = '\'';
    *p = 0;
}

