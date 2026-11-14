// echo [-n] [-e | -E] [TEXT...]    write the words, with one space between them
//
// -n leaves off the newline. -e reads backslash escapes in the text
// (\n \t \r \a \b \e \f \v \\, \0NNN for a byte in octal, \xHH in hex, and \c
// to stop there with no newline); -E, the default, leaves them as
// typed. The flags may be bundled (-ne) and only count while every
// letter in the word is one of n, e, E; anything else is text. To keep
// the blanks inside a piece of text as typed, quote it: echo "a   b".
#include "ulib.h"

static char ob[1024];
static uint32_t on;

// A reader that has gone away ends echo quietly.
static void flush(void) {
    const char* p = ob;
    while (on) {
        int w = sys_write(STDOUT_FILENO, p, on);
        if (w <= 0) {
            if (w != -EPIPE) dprintf(STDERR_FILENO, "echo: write error: %s\n", w ? strerror(w) : "no progress");
            sys_exit(1);
        }
        p += w;
        on -= (uint32_t)w;
    }
}

static void out(char c) {
    if (on == sizeof ob) flush();
    ob[on++] = c;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// One piece of text with escapes read. 1 if a \c asked to stop.
static int out_escaped(const char* s) {
    for (; *s; s++) {
        if (*s != '\\' || !s[1]) {
            out(*s);
            continue;
        }
        s++;
        switch (*s) {
        case 'n': out('\n'); break;
        case 't': out('\t'); break;
        case 'r': out('\r'); break;
        case 'a': out('\a'); break;
        case 'b': out('\b'); break;
        case 'e': out(27); break;
        case 'f': out('\f'); break;
        case 'v': out('\v'); break;
        case '\\': out('\\'); break;
        case 'c': return 1;
        case '0': {
            int v = 0, n = 0;
            while (n < 3 && s[1] >= '0' && s[1] <= '7') v = v * 8 + (*++s - '0'), n++;
            out((char)v);
            break;
        }
        case 'x': {
            int v = 0, n = 0;
            while (n < 2 && hexval(s[1]) >= 0) v = v * 16 + hexval(*++s), n++;
            if (n) out((char)v);
            else { out('\\'); out('x'); }
            break;
        }
        default: out('\\'); out(*s); break;
        }
    }
    return 0;
}

int main(int argc, char** argv) {
    int a = 1, newline = 1, escapes = 0;
    for (; a < argc; a++) {
        const char* s = argv[a];
        if (s[0] != '-' || !s[1]) break;
        int ok = 1;
        for (const char* o = s + 1; *o; o++) ok &= *o == 'n' || *o == 'e' || *o == 'E';
        if (!ok) break;
        for (const char* o = s + 1; *o; o++) {
            if (*o == 'n') newline = 0;
            else escapes = *o == 'e';
        }
    }
    for (int i = a; i < argc; i++) {
        if (escapes) {
            if (out_escaped(argv[i])) {
                newline = 0;
                break;
            }
        } else {
            for (const char* s = argv[i]; *s; s++) out(*s);
        }
        if (i + 1 < argc) out(' ');
    }
    if (newline) out('\n');
    flush();
    return 0;
}
