// hexdump [-C | -x] [-v] [-n LENGTH] [-s OFFSET] [FILE...]
//
// Shows the bytes of the FILEs, taken as one stream, or of standard
// input when none is named (or for "-"). The default and -x print
// 16-bit words, low byte first, as util-linux does:
//   0000000 6548 6c6c 0a6f
//   0000006
// -C is the canonical form: offset, sixteen bytes in hex, and the
// printable ones again between bars:
//   00000000  48 65 6c 6c 6f 0a                                 |Hello.|
//   00000006
// A line identical to the one before it is replaced by a single "*"
// unless -v is given. -n stops after LENGTH bytes and -s starts at
// OFFSET (both accept 0x hex); the offsets shown count from the
// start of the stream. The last line is not padded in the default
// format, and a final line with just the offset reached closes the
// listing.
#include "ulib.h"
#include "bstream.h"

static int usage(void) {
    dprintf(STDERR_FILENO, "usage: hexdump [-C | -x] [-v] [-n LENGTH] [-s OFFSET] [FILE...]\n");
    return 2;
}

static char ob[1024];
static uint32_t on;

static void flush(void) {
    const char* p = ob;
    while (on) {
        int w = sys_write(STDOUT_FILENO, p, on);
        if (w <= 0) {
            if (w != -EPIPE) dprintf(STDERR_FILENO, "hexdump: write error: %s\n", w ? strerror(w) : "no progress");
            sys_exit(1);
        }
        p += w;
        on -= (uint32_t)w;
    }
}

static void emit(const char* s, uint32_t n) {
    if (on + n > sizeof ob) flush();
    memcpy(ob + on, s, n);
    on += n;
}

// ---- the lines ----

static void line_canonical(uint32_t off, const unsigned char* b, uint32_t n) {
    char s[96];
    int len = snprintf(s, sizeof s, "%08x ", off);
    for (uint32_t i = 0; i < 16; i++) {
        if (i == 8) s[len++] = ' ';
        if (i < n) len += snprintf(s + len, sizeof s - (size_t)len, " %02x", b[i]);
        else { s[len++] = ' '; s[len++] = ' '; s[len++] = ' '; }
    }
    s[len++] = ' ';
    s[len++] = ' ';
    s[len++] = '|';
    for (uint32_t i = 0; i < n; i++) s[len++] = b[i] >= 0x20 && b[i] < 0x7F ? (char)b[i] : '.';
    s[len++] = '|';
    s[len++] = '\n';
    emit(s, (uint32_t)len);
}

static void line_words(uint32_t off, const unsigned char* b, uint32_t n) {
    char s[96];
    int len = snprintf(s, sizeof s, "%07x", off);
    for (uint32_t i = 0; i < n; i += 2) {
        unsigned w = b[i] | (i + 1 < n ? (unsigned)b[i + 1] << 8 : 0);
        len += snprintf(s + len, sizeof s - (size_t)len, " %04x", w);
    }
    s[len++] = '\n';
    emit(s, (uint32_t)len);
}

int main(int argc, char** argv) {
    int canonical = 0, verbose = 0, a = 1;
    uint32_t limit = 0xFFFFFFFFu, start = 0;
    for (; a < argc && argv[a][0] == '-' && argv[a][1]; a++) {
        if (!strcmp(argv[a], "--")) { a++; break; }
        for (const char* o = argv[a] + 1; *o; o++) {
            if (*o == 'C') canonical = 1;
            else if (*o == 'x') canonical = 0;
            else if (*o == 'v') verbose = 1;
            else if (*o == 'n' || *o == 's') {
                const char* v = o + 1;
                if (!*v) {
                    if (++a == argc) return usage();
                    v = argv[a];
                }
                uint32_t* dst = *o == 'n' ? &limit : &start;
                if (parse_uint(v, dst)) {
                    dprintf(STDERR_FILENO, "hexdump: invalid %s: '%s'\n", *o == 'n' ? "length" : "offset", v);
                    return 2;
                }
                break;
            } else {
                return usage();
            }
        }
    }
    if (a == argc && sys_fdtype(STDIN_FILENO) == FDTYPE_TTY) return usage();
    bstream_t in;
    bs_init(&in, "hexdump", argv + a, argc - a, start, flush);

    unsigned char line[16], prev[16];
    uint32_t off = start;
    int have_prev = 0, starred = 0, any = 0;
    while (limit) {
        uint32_t n = bs_get(&in, line, limit < 16 ? limit : 16);
        if (!n) break;
        limit -= n;
        any = 1;
        if (n == 16 && have_prev && !verbose && !memcmp(line, prev, 16)) {
            if (!starred) emit("*\n", 2);
            starred = 1;
            off += 16;
            continue;
        }
        starred = 0;
        if (canonical) line_canonical(off, line, n);
        else line_words(off, line, n);
        have_prev = n == 16;
        if (have_prev) memcpy(prev, line, 16);
        off += n;
    }
    if (any) {
        char s[16];
        int len = snprintf(s, sizeof s, canonical ? "%08x\n" : "%07x\n", off);
        emit(s, (uint32_t)len);
    }
    flush();
    return in.status;
}
