#include "ulib.h"
#include "tabstops.h"

static int is_blank(char c) { return c == ' ' || c == '\t'; }

void tabs_init(tabs_t* t) { memset(t, 0, sizeof *t); }

static int fail(const char* prog, const char* what) {
    dprintf(STDERR_FILENO, "%s: %s\n", prog, what);
    return 1;
}

static int add_stop(tabs_t* t, uint64_t v, const char* prog) {
    if (t->n == t->cap) {
        uint32_t cap = t->cap ? t->cap * 2 : 8;
        uint64_t* s = realloc(t->stops, sizeof(uint64_t) * cap);
        if (!s) return fail(prog, "out of memory");
        t->stops = s;
        t->cap = cap;
    }
    t->stops[t->n++] = v;
    return 0;
}

// Quotes s as GNU does for a message: in '...', a tab as \t and any other
// byte that is not printable as \ and three octal digits.
static char* quoted(const char* s) {
    uint32_t n = 0;
    for (const char* p = s; *p; p++) n += (unsigned char)*p < 32 || (unsigned char)*p > 126 ? 4 : 1;
    char* q = malloc(n + 1);
    if (!q) return 0;
    char* o = q;
    for (const char* p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '\t') {
            *o++ = '\\';
            *o++ = 't';
        } else if (c < 32 || c > 126) {
            *o++ = '\\';
            *o++ = (char)('0' + (c >> 6));
            *o++ = (char)('0' + ((c >> 3) & 7));
            *o++ = (char)('0' + (c & 7));
        } else {
            *o++ = (char)c;
        }
    }
    *o = 0;
    return q;
}

// "PROG: WHAT 'ARG'" with ARG quoted. Always returns 1.
static int complain(const char* prog, const char* what, const char* arg) {
    char* q = quoted(arg);
    dprintf(STDERR_FILENO, "%s: %s '%s'\n", prog, what, q ? q : arg);
    free(q);
    return 1;
}

// A number (or a / or + item) ends: it is a stop, or what comes after the
// last one. The last / or + seen in this list says which, for every item
// after it too, so "4,+3,5" is an error and "4,+0,3" means "4,+3". 0 for
// fine, else 1 once the user has been told.
static int end_item(tabs_t* t, char mode, uint64_t val, const char* prog) {
    if (mode == '/' || mode == '+') {
        uint64_t* slot = mode == '/' ? &t->extend : &t->incr;
        if (*slot) {
            dprintf(STDERR_FILENO, "%s: '%c' specifier only allowed with the last value\n", prog, mode);
            return 1;
        }
        *slot = val;
        return 0;
    }
    return add_stop(t, val, prog);
}

int tabs_add(tabs_t* t, const char* list, const char* prog) {
    int ok = 1;
    int have = 0;          // digits have been read for this item
    uint64_t val = 0;
    char mode = 0;         // the last / or + in this list
    const char* num = 0;   // where this item's digits began

    for (const char* p = list;; p++) {
        char c = *p;
        if (!c || c == ',' || is_blank(c)) {
            // The last item is left alone once something has been said to be
            // wrong, as in GNU; the others are still looked at.
            if (have && (c || ok) && end_item(t, mode, val, prog)) return 1;
            have = 0;
            val = 0;
            num = 0;
            if (!c) break;
        } else if (c == '/' || c == '+') {
            if (num) {
                char what[48];
                snprintf(what, sizeof what, "'%c' specifier not at start of number:", c);
                complain(prog, what, p);
                ok = 0;
            }
            mode = c;
        } else if (c >= '0' && c <= '9') {
            // The whole number at once: a number that is too large is
            // one complaint, not one for each digit.
            num = p;
            have = 1;
            val = 0;
            int big = 0;
            for (; *p >= '0' && *p <= '9'; p++) {
                uint64_t d = (uint64_t)(*p - '0');
                if (val > (UINT64_MAX - d) / 10) big = 1;
                else val = val * 10 + d;
            }
            if (big) {
                uint32_t n = (uint32_t)(p - num);
                char* digits = malloc(n + 1);
                if (digits) {
                    memcpy(digits, num, n);
                    digits[n] = 0;
                }
                complain(prog, "tab stop is too large", digits ? digits : "");
                free(digits);
                ok = 0;
            }
            p--;
        } else {
            complain(prog, "tab size contains invalid character(s):", p);
            ok = 0;
            break;
        }
    }
    return !ok;
}

int tabs_finish(tabs_t* t, const char* prog) {
    for (uint32_t i = 0; i < t->n; i++) {
        if (!t->stops[i]) return fail(prog, "tab size cannot be 0");
        if (i && t->stops[i] <= t->stops[i - 1]) return fail(prog, "tab sizes must be ascending");
    }
    if (t->extend && t->incr) return fail(prog, "'/' specifier is mutually exclusive with '+'");
    if (!t->n) {
        t->size = t->extend ? t->extend : t->incr ? t->incr : 8;
    } else if (t->n == 1 && !t->extend && !t->incr) {
        t->size = t->stops[0];
        t->n = 0;
    }
    return 0;
}

uint64_t tabs_next(const tabs_t* t, uint64_t col, int* last) {
    *last = 0;
    if (t->size) return col + (t->size - col % t->size);
    for (uint32_t i = 0; i < t->n; i++)
        if (col < t->stops[i]) return t->stops[i];
    if (t->extend) return col + (t->extend - col % t->extend);
    if (t->incr) return col + (t->incr - (col - t->stops[t->n - 1]) % t->incr);
    *last = 1;
    return 0;
}
