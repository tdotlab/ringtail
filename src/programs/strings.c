// strings [-n N | -N] [-f] [-t o|d|x | -o] [-e s|S] [-a] [FILE...]
//
// Prints the runs of printable characters in the FILEs (standard input
// when none is named), one per line, as the binutils strings does:
//   -n N  runs of at least N characters (default 4); -N is the same,
//         so is --bytes=N. N may be written 0x10 or 010 as in C.
//   -t R  put the offset of each run before it, in base R: o, d or x,
//         in a field of seven. -o is -t o.
//   -f    put the file name and a colon before each run.
//   -e s  7-bit characters (the default): a space up to ~ and the tab.
//   -e S  the bytes 128 to 255 count as printable too.
//   -a    accepted and ignored: the whole file is always scanned.
// A run ends at any other byte, a newline included, or at the end of
// the file. Not done: -e for 16 and 32-bit characters, -s, -w, -d.
#include "ulib.h"
#include "lineio.h"

static int usage(void) {
    dprintf(STDERR_FILENO, "usage: strings [-a] [-f] [-n N] [-t o|d|x] [-e s|S] [FILE...]\n");
    return 2;
}

static lio_out_t out;
static int status;

static void usage_exit(void) {
    usage();
    sys_exit(2);
}

static void fail(const char* msg) {
    lio_flush(&out);
    dprintf(STDERR_FILENO, "strings: %s\n", msg);
    sys_exit(1);
}

// Digits of v in the base, right-aligned in `width` with spaces.
static int fmt_offset(char* s, uint32_t v, unsigned base, int width) {
    char tmp[16];
    int n = 0;
    do { tmp[n++] = "0123456789abcdef"[v % base]; v /= base; } while (v);
    int k = 0;
    for (int i = n; i < width; i++) s[k++] = ' ';
    while (n) s[k++] = tmp[--n];
    return k;
}

// The value of N as C reads a number (0x hex, a leading 0 octal), after
// the blanks and the sign strtol takes. 0 on success, 1 if it does not
// fit in 64 bits, -1 if it is not a number.
static int parse_len(const char* s, int64_t* out_v) {
    while (*s == ' ' || (*s >= '\t' && *s <= '\r')) s++;
    int neg = 0;
    if (*s == '+' || *s == '-') neg = *s++ == '-';
    unsigned base = 10;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X') && s[2]) { base = 16; s += 2; }
    else if (s[0] == '0') base = 8;
    if (!*s) return -1;
    uint64_t v = 0;
    int over = 0;
    for (; *s; s++) {
        unsigned d;
        if (*s >= '0' && *s <= '9') d = (unsigned)(*s - '0');
        else if (base == 16 && *s >= 'a' && *s <= 'f') d = (unsigned)(*s - 'a' + 10);
        else if (base == 16 && *s >= 'A' && *s <= 'F') d = (unsigned)(*s - 'A' + 10);
        else return -1;
        if (d >= base) return -1;
        // too big for 64 bits, but the rest must still be digits
        if (v > (0x7FFFFFFFFFFFFFFFull - d) / base) over = 1;
        else v = v * base + d;
    }
    if (over) return 1;
    *out_v = neg ? -(int64_t)v : (int64_t)v;
    return 0;
}

static uint32_t minlen = 4;
static int show_name, eight_bit;
static unsigned radix;  // 0: no offsets, else 8, 10 or 16

static void check_len(int64_t n) {
    char msg[64];
    if (n == 0) fail("minimum string length is too small: 0");
    if (n == 4294967295LL) fail("minimum string length 4294967295 is too big");
    if (n < 0 || n > 4294967295LL) {
        // The number is shown the way the binutils does, as a decimal.
        char num[24], tmp[24];
        int neg = n < 0, k = 0, j = 0;
        uint64_t m = neg ? 0 - (uint64_t)n : (uint64_t)n;
        do { tmp[k++] = (char)('0' + m % 10); m /= 10; } while (m);
        if (neg) num[j++] = '-';
        while (k) num[j++] = tmp[--k];
        num[j] = 0;
        snprintf(msg, sizeof msg, "minimum string length is too big: %s", num);
        fail(msg);
    }
    minlen = (uint32_t)n;
}

static void set_len(const char* v) {
    int64_t n;
    char msg[96];
    if (!*v) fail("minimum string length is too small: ");
    int r = parse_len(v, &n);
    if (r == 1) {
        // more than a 64-bit number holds: too big, and shown as it was typed
        snprintf(msg, sizeof msg, "minimum string length is too big: %s", v);
        fail(msg);
    }
    if (r) {
        snprintf(msg, sizeof msg, "invalid integer argument %s", v);
        fail(msg);
    }
    check_len(n);
}

static void set_radix(const char* v) {
    if (v[0] == 'o' && !v[1]) radix = 8;
    else if (v[0] == 'd' && !v[1]) radix = 10;
    else if (v[0] == 'x' && !v[1]) radix = 16;
    else usage_exit();
}

static void set_encoding(const char* v) {
    if (!strcmp(v, "s")) eight_bit = 0;
    else if (!strcmp(v, "S")) eight_bit = 1;
    else usage_exit();
}

// ---- the scan ----

static lio_line_t run;   // the part of a run not yet written
static int emitting;     // the run is long enough: bytes go straight out
static uint32_t run_at;  // offset of the run's first byte

static int printable(unsigned char c) {
    return (c >= 0x20 && c < 0x7F) || c == '\t' || (eight_bit && c >= 0x80);
}

static void begin_output(const char* name) {
    if (show_name) {
        lio_puts(&out, name);
        lio_puts(&out, ": ");
    }
    if (radix) {
        char s[24];
        lio_put(&out, s, (uint32_t)fmt_offset(s, run_at, radix, 7));
        lio_putc(&out, ' ');
    }
}

static void scan(int fd, const char* name) {
    unsigned char buf[2048];
    uint32_t pos = 0;
    run.n = 0;
    emitting = 0;
    int n;
    while ((n = sys_read(fd, buf, sizeof buf)) > 0) {
        for (int i = 0; i < n; i++, pos++) {
            unsigned char c = buf[i];
            if (printable(c)) {
                if (emitting) { lio_putc(&out, (char)c); continue; }
                if (!run.n) run_at = pos;
                lio_line_addc(&run, (char)c, "strings");
                if (run.n >= minlen) {
                    begin_output(name);
                    lio_put(&out, run.s, run.n);
                    run.n = 0;
                    emitting = 1;
                }
            } else {
                if (emitting) lio_putc(&out, '\n');
                emitting = 0;
                run.n = 0;
            }
        }
        if (n < (int)sizeof buf) lio_flush(&out);  // a slow pipe still shows its strings
    }
    if (emitting) lio_putc(&out, '\n');
    emitting = 0;
    run.n = 0;
    if (n < 0) {
        lio_flush(&out);
        dprintf(STDERR_FILENO, "strings: %s: %s\n", name, strerror(n));
        status = 1;
    }
}

int main(int argc, char** argv) {
    lio_out_init(&out, STDOUT_FILENO, "strings");
    int a = 1, dash = 0;
    for (; a < argc && argv[a][0] == '-'; a++) {
        const char* arg = argv[a];
        if (!arg[1]) { dash = 1; continue; }  // a lone "-" means -a, as in the binutils
        if (!strcmp(arg, "--")) { a++; break; }
        if (arg[1] == '-') {
            // Long options, written whole; the ones with a value take it
            // after "=" or as the next argument.
            const char* eq = strchr(arg, '=');
            uint32_t nl = eq ? (uint32_t)(eq - arg) : (uint32_t)strlen(arg);
            const char* name = arg + 2;
            uint32_t len = nl - 2;
            if (len == 3 && !strncmp(name, "all", 3)) continue;
            if (len == 15 && !strncmp(name, "print-file-name", 15)) { show_name = 1; continue; }
            int which = 0;
            if (len == 5 && !strncmp(name, "bytes", 5)) which = 1;
            else if (len == 5 && !strncmp(name, "radix", 5)) which = 2;
            else if (len == 8 && !strncmp(name, "encoding", 8)) which = 3;
            else return usage();
            const char* v = eq ? eq + 1 : 0;
            if (!v) {
                if (++a == argc) return usage();
                v = argv[a];
            }
            if (which == 1) set_len(v);
            else if (which == 2) set_radix(v);
            else set_encoding(v);
            continue;
        }
        for (const char* o = arg + 1; *o; o++) {
            if (*o == 'a') continue;
            else if (*o == 'f') show_name = 1;
            else if (*o == 'o') radix = 8;
            else if (*o >= '0' && *o <= '9') {
                // -NUM: the digits are the length, in decimal.
                int64_t v = 0;
                for (; *o >= '0' && *o <= '9'; o++)
                    if (v < 100000000000000LL) v = v * 10 + (*o - '0');
                o--;
                check_len(v);
            } else if (*o == 'n' || *o == 't' || *o == 'e') {
                const char* v = o + 1;
                if (!*v) {
                    if (++a == argc) return usage();
                    v = argv[a];
                }
                if (*o == 'n') set_len(v);
                else if (*o == 't') set_radix(v);
                else set_encoding(v);
                break;
            } else {
                return usage();
            }
        }
    }
    // A "-" with no file after it is no way to name standard input.
    if (a == argc && (dash || sys_fdtype(STDIN_FILENO) == FDTYPE_TTY)) return usage();

    if (a == argc) {
        scan(STDIN_FILENO, "{standard input}");
    } else {
        for (; a < argc; a++) {
            const char* name = argv[a];
            int fd = sys_open(name, O_RDONLY);
            if (fd < 0) {
                lio_flush(&out);
                if (fd == -EISDIR) dprintf(STDERR_FILENO, "strings: Warning: '%s' is a directory\n", name);
                else dprintf(STDERR_FILENO, "strings: '%s': %s\n", name, fd == -ENOENT ? "No such file" : strerror(fd));
                status = 1;
                continue;
            }
            scan(fd, name);
            sys_close(fd);
        }
    }
    lio_flush(&out);
    return status;
}
