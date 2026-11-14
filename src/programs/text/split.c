// split [-l N | -b SIZE | -N] [-a LEN] [-d | -x] [FILE [PREFIX]]
//
// Cuts FILE (or standard input) into pieces and writes them to the files
// PREFIXaa, PREFIXab, ... (PREFIX is x if not given). Pieces are 1000 lines
// long unless told otherwise:
//   -l N, --lines=N        N lines to a piece (-N is the same)
//   -b SIZE, --bytes=SIZE  SIZE bytes to a piece; SIZE may end in b (512), K, M,
//                          G, ... (powers of 1024, also KiB) or KB, MB, ... (1000)
//   -a N, --suffix-length=N  N letters after PREFIX (without it: two, and more
//                          when the files run out, as GNU split does)
//   -d, --numeric-suffixes[=FROM]  00, 01, ... in place of aa, ab, ..., starting
//                          at FROM; -x, --hex-suffixes[=FROM] the same in hex
//   --additional-suffix=S  S after the suffix (xaa.txt)
//   --verbose              "creating file 'xaa'" before each file
// A file is made only when there is something to put in it, so empty input
// makes none, and the last piece is what is left. The options of GNU split
// that cut by lines of at most N bytes, by number of pieces, by a separator,
// through a filter (-C -n -t --filter) and --version are known, and stop with
// a message; -e and -u only matter with -n, so they are taken and do nothing.
// A usage error is
// status 2; a bad number or a file that cannot be used is status 1.
#include "ulib.h"
#include "splitcore.h"

static char ibuf[4096], obuf[4096];
static int ofd = -1;
static uint32_t on;
static char oname[PATH_MAX];

static int mode;  // 0, 'l' (lines), 'b' (bytes) or 'd' (the -NUM way)
static uint64_t units = 1000;
static uint64_t digits_val;
static int digits_arg = -1;
static int kind = SP_ALPHA, verbose;
static uint32_t suflen;
static const char* from;
static const char* addsuf = "";
static const char* prefix = "x";
static const char* infile;  // 0 for standard input
static sp_suffix_t suf;
static int suffix_too_long, nfiles;

#define TRY "Try 'split --help' for more information.\n"

static int usage_error(void) {
    dprintf(STDERR_FILENO, TRY);
    return 2;
}

static int unsupported(const char* shown) {
    dprintf(STDERR_FILENO, "split: option '%s' is not supported\n", shown);
    return 2;
}

static void help(void) {
    static const char text[] =
        "usage: split [OPTION]... [FILE [PREFIX]]\n"
        "Writes FILE (standard input if none or -) in pieces of 1000 lines to PREFIXaa,\n"
        "PREFIXab, ... (PREFIX is x if not given).\n"
        "  -l, --lines=N                N lines to a piece\n"
        "  -b, --bytes=SIZE             SIZE bytes to a piece; SIZE may end in b, K, M, G, ...\n"
        "                               (powers of 1024; KiB too) or KB, MB, ... (of 1000)\n"
        "  -N                           the same as -l N\n"
        "  -a, --suffix-length=N        N letters after PREFIX (default 2, more when needed)\n"
        "  -d, --numeric-suffixes[=FROM]  00, 01, ... in place of aa, ab, ... (from FROM)\n"
        "  -x, --hex-suffixes[=FROM]    the same in hex\n"
        "      --additional-suffix=S    S after the suffix\n"
        "      --verbose                say \"creating file 'NAME'\" before each file\n"
        "      --help                   this text\n";
    sys_write(STDOUT_FILENO, text, sizeof text - 1);
}

// What the options do. Returns -1 to go on, or the status to end with.
static int apply(char id, const char* arg, int a) {
    switch (id) {
        case 'l':
        case 'b': {
            if (mode) {
                dprintf(STDERR_FILENO, "split: cannot split in more than one way\n");
                return usage_error();
            }
            mode = id;
            if (id == 'l' ? sp_parse_lines(arg, &units) : sp_parse_bytes(arg, &units)) {
                dprintf(STDERR_FILENO, "split: invalid number of %s: '%s'\n", id == 'l' ? "lines" : "bytes", arg);
                return 1;
            }
            return -1;
        }
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9': {
            if (mode && mode != 'd') {
                dprintf(STDERR_FILENO, "split: cannot split in more than one way\n");
                return usage_error();
            }
            mode = 'd';
            // The digits of one argument make one number; another argument
            // starts a new one.
            if (digits_arg != a) digits_val = 0;
            digits_arg = a;
            uint64_t d = (uint64_t)(id - '0');
            digits_val = digits_val > (UINT64_MAX - d) / 10 ? UINT64_MAX : digits_val * 10 + d;
            units = digits_val;
            return -1;
        }
        case 'a': {
            int r = sp_parse_suffix_len(arg, &suflen);
            if (r) {
                dprintf(STDERR_FILENO, "split: invalid suffix length: '%s'%s\n", arg,
                        r == -2 ? ": Numerical result out of range" : r == -3 ? ": Value too large for defined data type" : "");
                return 1;
            }
            return -1;
        }
        case 'd':
        case 'x':
            kind = id == 'd' ? SP_NUM : SP_HEX;
            if (arg && *arg) from = arg;
            return -1;
        case 'A':
            if (strchr(arg, '/')) {
                dprintf(STDERR_FILENO, "split: invalid suffix '%s', contains directory separator\n", arg);
                return usage_error();
            }
            addsuf = arg;
            return -1;
        case 'v':
            verbose = 1;
            return -1;
    }
    return -1;
}

static const struct {
    const char* name;
    char has_arg;  // 0 none, 1 needed, 2 optional (with =)
    char id;
} LONG[] = {{"bytes", 1, 'b'},           {"lines", 1, 'l'},          {"line-bytes", 1, 'C'},      {"number", 1, 'n'},
            {"elide-empty-files", 0, 'e'}, {"filter", 1, 'F'},       {"numeric-suffixes", 2, 'd'}, {"hex-suffixes", 2, 'x'},
            {"suffix-length", 1, 'a'},   {"additional-suffix", 1, 'A'}, {"separator", 1, 't'},       {"unbuffered", 0, 'u'},
            {"verbose", 0, 'v'},         {"help", 0, 'h'},           {"version", 0, 'V'}};
#define NLONG ((int)(sizeof LONG / sizeof LONG[0]))

// A message to standard error ends the files being written first.
static void flush_out(void) {
    const char* p = obuf;
    while (on) {
        int w = sys_write(ofd, p, on);
        if (w <= 0) {
            dprintf(STDERR_FILENO, "split: %s: %s\n", oname, w ? strerror(w) : "no progress");
            sys_exit(1);
        }
        p += w;
        on -= (uint32_t)w;
    }
}

static void close_piece(void) {
    if (ofd < 0) return;
    flush_out();
    sys_close(ofd);
    ofd = -1;
}

static void open_piece(void) {
    if (suffix_too_long) {
        dprintf(STDERR_FILENO, "split: File name too long\n");
        sys_exit(1);
    }
    if (nfiles++ && sp_suffix_next(&suf)) {
        dprintf(STDERR_FILENO, "split: output file suffixes exhausted\n");
        sys_exit(1);
    }
    char text[SP_MAXLEN + 1];
    sp_suffix_text(&suf, text);
    if (strlen(prefix) + strlen(text) + strlen(addsuf) >= sizeof oname) {
        dprintf(STDERR_FILENO, "split: File name too long\n");
        sys_exit(1);
    }
    snprintf(oname, sizeof oname, "%s%s%s", prefix, text, addsuf);
    if (infile && !strcmp(infile, oname)) {
        dprintf(STDERR_FILENO, "split: '%s' would overwrite input; aborting\n", oname);
        sys_exit(1);
    }
    if (verbose) dprintf(STDOUT_FILENO, "creating file '%s'\n", oname);
    ofd = sys_open(oname, O_WRONLY | O_CREAT | O_TRUNC);
    if (ofd < 0) {
        dprintf(STDERR_FILENO, "split: %s: %s\n", oname, strerror(ofd));
        sys_exit(1);
    }
}

static void put(const char* p, uint32_t n) {
    if (ofd < 0) open_piece();
    while (n) {
        if (on == sizeof obuf) flush_out();
        uint32_t k = sizeof obuf - on < n ? (uint32_t)sizeof obuf - on : n;
        memcpy(obuf + on, p, k);
        on += k;
        p += k;
        n -= k;
    }
}

int main(int argc, char** argv) {
    const char** ops = malloc((size_t)argc * sizeof *ops);
    int nops = 0, only_ops = 0;
    for (int a = 1; a < argc; a++) {
        const char* s = argv[a];
        if (only_ops || s[0] != '-' || !s[1]) {
            ops[nops++] = s;
            continue;
        }
        if (!strcmp(s, "--")) {
            only_ops = 1;
            continue;
        }
        if (s[1] != '-') {
            for (const char* o = s + 1; *o; o++) {
                char c = *o;
                int r;
                if (c >= '0' && c <= '9') {
                    r = apply(c, 0, a);
                } else if (c == 'd' || c == 'x') {
                    r = apply(c, 0, a);
                } else if (c == 'e' || c == 'u') {
                    r = -1;  // only matter with -n, which is not here
                } else if (c && strchr("ablCnt", c)) {
                    const char* arg;
                    if (o[1]) {
                        arg = o + 1;
                    } else if (a + 1 < argc) {
                        arg = argv[++a];
                    } else {
                        dprintf(STDERR_FILENO, "split: option requires an argument -- '%c'\n", c);
                        return usage_error();
                    }
                    if (c == 'C' || c == 'n' || c == 't') {
                        char shown[3] = {'-', c, 0};
                        return unsupported(shown);
                    }
                    r = apply(c, arg, a);
                    if (r >= 0) return r;
                    break;  // the rest of the argument was the value
                } else {
                    dprintf(STDERR_FILENO, "split: invalid option -- '%c'\n", c);
                    return usage_error();
                }
                if (r >= 0) return r;
            }
            continue;
        }
        // A long option, or the start of one: exact, or the only one that begins so.
        uint32_t len = 0;
        while (s[2 + len] && s[2 + len] != '=') len++;
        int hit = -1, hits = 0;
        for (int i = 0; i < NLONG; i++) {
            if (strncmp(LONG[i].name, s + 2, len)) continue;
            if (strlen(LONG[i].name) == len) {
                hit = i;
                hits = 1;
                break;
            }
            hit = i;
            hits++;
        }
        if (len == 0 || !hits) {
            dprintf(STDERR_FILENO, "split: unrecognized option '%s'\n", s);
            return usage_error();
        }
        if (hits > 1) {
            dprintf(STDERR_FILENO, "split: option '%s' is ambiguous; possibilities:", s);
            for (int i = 0; i < NLONG; i++)
                if (!strncmp(LONG[i].name, s + 2, len)) dprintf(STDERR_FILENO, " '--%s'", LONG[i].name);
            dprintf(STDERR_FILENO, "\n");
            return usage_error();
        }
        const char* arg = s[2 + len] == '=' ? s + 3 + len : 0;
        if (arg && !LONG[hit].has_arg) {
            dprintf(STDERR_FILENO, "split: option '--%s' doesn't allow an argument\n", LONG[hit].name);
            return usage_error();
        }
        if (!arg && LONG[hit].has_arg == 1) {
            if (a + 1 >= argc) {
                dprintf(STDERR_FILENO, "split: option '--%s' requires an argument\n", LONG[hit].name);
                return usage_error();
            }
            arg = argv[++a];
        }
        char id = LONG[hit].id;
        if (id == 'h') {
            help();
            return 0;
        }
        if (strchr("CnFtV", id)) {
            char shown[40];
            snprintf(shown, sizeof shown, "--%s", LONG[hit].name);
            return unsupported(shown);
        }
        int r = apply(id, arg, a);
        if (r >= 0) return r;
    }

    if (mode == 'd' && units == 0) {
        dprintf(STDERR_FILENO, "split: invalid number of lines: '0'\n" TRY);
        return 1;
    }
    if (nops > 2) {
        dprintf(STDERR_FILENO, "split: extra operand '%s'\n", ops[2]);
        return usage_error();
    }
    if (nops > 0 && strcmp(ops[0], "-")) infile = ops[0];
    if (nops > 1) prefix = ops[1];
    int r = sp_suffix_init(&suf, kind, suflen, from);
    if (r == SP_BAD_START) {
        dprintf(STDERR_FILENO, "split: '%s': invalid start value for %s suffix\n", from, kind == SP_HEX ? "hexadecimal" : "numerical");
        return usage_error();
    }
    if (r == SP_START_TOO_LARGE) {
        dprintf(STDERR_FILENO, "split: numerical suffix start value is too large for the suffix length\n");
        return usage_error();
    }
    suffix_too_long = r == SP_TOO_LONG;

    int fd = STDIN_FILENO;
    if (infile) {
        fd = sys_open(infile, O_RDONLY);
        if (fd < 0) {
            if (fd == -EISDIR) dprintf(STDERR_FILENO, "split: %s: %s\n", infile, strerror(fd));
            else dprintf(STDERR_FILENO, "split: cannot open '%s' for reading: %s\n", infile, strerror(fd));
            return 1;
        }
    }

    uint64_t left = units;
    int n;
    while ((n = sys_read(fd, ibuf, sizeof ibuf)) > 0) {
        uint32_t i = 0;
        while (i < (uint32_t)n) {
            uint32_t j;
            if (mode == 'b') {
                uint64_t take = left < (uint64_t)n - i ? left : (uint64_t)n - i;
                j = i + (uint32_t)take;
                left -= take;
            } else {
                for (j = i; j < (uint32_t)n;)
                    if (ibuf[j++] == '\n' && --left == 0) break;
            }
            put(ibuf + i, j - i);
            if (left == 0) {
                close_piece();
                left = units;
            }
            i = j;
        }
    }
    close_piece();
    if (n < 0) {
        dprintf(STDERR_FILENO, "split: %s: %s\n", infile ? infile : "standard input", strerror(n));
        return 1;
    }
    return 0;
}
