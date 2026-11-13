// base64 [-d] [-i] [-w COLS] [FILE]
//
// Turns a file (or standard input, or "-") into base64 text, or back: the
// bytes of the file in letters and digits, which pass through anything
// that handles text. Used to carry binary data through a pipe or a
// message, and to read what others send that way.
//   -d, --decode           decode instead of encode
//   -i, --ignore-garbage   when decoding, skip characters that are not
//                          part of the alphabet (a newline is always skipped)
//   -w COLS, --wrap=COLS   break encoded lines after COLS characters
//                          (76 unless this says another; 0 for one long line)
// Long options may be shortened to anything that is not ambiguous, as in GNU.
// Decoding stops at the first invalid group of four, after the bytes that
// came before it, with "invalid input" and status 1.
#include "ulib.h"
#include "lineio.h"
#include "b64.h"

static int usage(void) {
    dprintf(STDERR_FILENO, "usage: base64 [-d] [-i] [-w COLS] [FILE]\n");
    return 2;
}

// If s is an abbreviation of long option name (at least one letter after
// the two dashes, up to an "="), the length of what is typed. 0 if not.
static int is_long(const char* s, const char* name) {
    int n = 0;
    while (s[2 + n] && s[2 + n] != '=') {
        if (s[2 + n] != name[n]) return 0;
        n++;
    }
    return n;
}

static int encode(int fd, uint32_t wrap, lio_out_t* out) {
    lio_in_t in;
    lio_in_init(&in, fd);
    b64enc_t e;
    b64enc_init(&e, wrap);
    char buf[16];
    int c;
    while ((c = lio_getc(&in, out)) >= 0) lio_put(out, buf, (uint32_t)b64enc_put(&e, (uint8_t)c, buf));
    if (in.err) {
        lio_flush(out);
        dprintf(STDERR_FILENO, "base64: read error: %s\n", strerror(in.err));
        return 1;
    }
    lio_put(out, buf, (uint32_t)b64enc_end(&e, buf));
    return 0;
}

static int decode(int fd, int ignore, lio_out_t* out) {
    lio_in_t in;
    lio_in_init(&in, fd);
    b64dec_t d;
    b64dec_init(&d, ignore);
    uint8_t buf[3];
    int c;
    while ((c = lio_getc(&in, out)) >= 0) {
        lio_put(out, (const char*)buf, (uint32_t)b64dec_put(&d, (uint8_t)c, buf));
        if (d.bad) break;
    }
    if (!d.bad && in.err) {
        lio_flush(out);
        dprintf(STDERR_FILENO, "base64: read error: %s\n", strerror(in.err));
        return 1;
    }
    if (!d.bad) lio_put(out, (const char*)buf, (uint32_t)b64dec_end(&d, buf));
    if (d.bad) {
        lio_flush(out);
        dprintf(STDERR_FILENO, "base64: invalid input\n");
        return 1;
    }
    return 0;
}

int main(int argc, char** argv) {
    int dec = 0, ignore = 0;
    uint32_t wrap = 76;
    const char* file = 0;
    int only_files = 0;
    for (int a = 1; a < argc; a++) {
        const char* s = argv[a];
        if (only_files || s[0] != '-' || !s[1]) {
            if (file) {
                dprintf(STDERR_FILENO, "base64: extra operand '%s'\n", s);
                return usage();
            }
            file = s;
            continue;
        }
        if (!strcmp(s, "--")) {
            only_files = 1;
            continue;
        }
        const char* wv = 0;  // the text after -w, if this argument had one
        if (s[1] == '-') {
            int hasval = strchr(s, '=') != 0;
            if (is_long(s, "decode") || is_long(s, "ignore-garbage")) {
                if (hasval) {
                    dprintf(STDERR_FILENO, "base64: option '%s' doesn't allow an argument\n", s);
                    return usage();
                }
                if (is_long(s, "decode")) dec = 1;
                else ignore = 1;
                continue;
            }
            if (!is_long(s, "wrap")) {
                dprintf(STDERR_FILENO, "base64: unrecognized option '%s'\n", s);
                return usage();
            }
            if (hasval) wv = strchr(s, '=') + 1;
            else if (a + 1 < argc) wv = argv[++a];
            else {
                dprintf(STDERR_FILENO, "base64: option '--wrap' requires an argument\n");
                return usage();
            }
        } else {
            for (const char* o = s + 1; *o && !wv; o++) {
                if (*o == 'd') dec = 1;
                else if (*o == 'i') ignore = 1;
                else if (*o == 'w') {
                    if (o[1]) wv = o + 1;
                    else if (a + 1 < argc) wv = argv[++a];
                    else {
                        dprintf(STDERR_FILENO, "base64: option requires an argument -- 'w'\n");
                        return usage();
                    }
                } else {
                    dprintf(STDERR_FILENO, "base64: invalid option -- '%c'\n", *o);
                    return usage();
                }
            }
        }
        if (wv && b64_parse_wrap(wv, &wrap)) {
            dprintf(STDERR_FILENO, "base64: invalid wrap size: '%s'\n", wv);
            return 2;
        }
    }

    int fd = STDIN_FILENO;
    if (file && strcmp(file, "-")) {
        fd = sys_open(file, O_RDONLY);
        if (fd < 0) {
            dprintf(STDERR_FILENO, "base64: %s: %s\n", file, strerror(fd));
            return 1;
        }
    }
    lio_out_t out;
    lio_out_init(&out, STDOUT_FILENO, "base64");
    int bad = dec ? decode(fd, ignore, &out) : encode(fd, wrap, &out);
    lio_flush(&out);
    return bad;
}
