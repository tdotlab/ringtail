// sha256 [-b | -t] [--tag] [-z] [FILE...]
// sha256 -c [-w] [--quiet] [--status] [--strict] [--ignore-missing] [FILE...]
//
// A fingerprint of each file (or of standard input, or of "-"): 64 hex
// digits that change if even one bit of the file does, so a download or a
// copy can be checked for coming through whole. The output is GNU
// sha256sum's, "HASH  NAME" with two spaces (or " *" for -b, which means
// nothing on a system with no text mode but is kept as GNU writes it); a
// name with a newline, a carriage return or a backslash in it is written
// with a "\" in front of the line and "\n", "\r" and "\\" in the name.
//   --tag      "SHA256 (NAME) = HASH", the BSD way
//   -z, --zero end each line with a NUL instead of a newline (and write
//              names as they are)
//   -c, --check
//              the files are lists of the above: read each line, hash
//              the file it names, and say "NAME: OK" or "NAME: FAILED"
//              (a file that cannot be read is "NAME: FAILED open or read").
//              Lines that are empty or begin with "#" are skipped; lines
//              that are not in either format are counted, and said to be
//              badly formatted at the end. GNU's closing warnings follow
//              ("WARNING: 2 computed checksums did NOT match").
//   -w, --warn         say which lines of a list are badly formatted, as they are met
//   --quiet            do not say OK for the files that matched
//   --status           say nothing; the exit status tells
//   --strict           badly formatted lines make the exit status 1
//   --ignore-missing   leave out the files that are not there, without a word
//                      (but "no file was verified" if that is all of them)
//              Whether the lines of the lists have a mode character after the
//              blank that follows the hash (the second space of "HASH  NAME",
//              or the "*") is settled by the first line that shows it, for all
//              the lists of the run, and later lines of the other kind are badly
//              formatted, as in GNU. So is a line that names "-" in a list read
//              from standard input.
// Long options may be shortened to anything that is not ambiguous, as in GNU.
// The exit status is 1 if any file could not be read or any checksum did not
// match, or a list had no usable line. As with base64, a mistake in how the
// program was run is status 2 where GNU's is 1. No --help or --version.
// Names in messages are written as they are, not in shell quotes as GNU
// writes those with a space or a special character in them.
#include "ulib.h"
#include "lineio.h"
#include "sha256.h"
#include "shasum.h"

static lio_out_t out;

// A message to standard error, after what standard output holds so far, so
// the two read in the order they happened.
#define ERR(...) \
    do { \
        lio_flush(&out); \
        dprintf(STDERR_FILENO, __VA_ARGS__); \
    } while (0)

static int usage(void) {
    dprintf(STDERR_FILENO,
            "usage: sha256 [-b | -t] [--tag] [-z] [FILE...]\n"
            "       sha256 -c [-w] [--quiet] [--status] [--strict] [--ignore-missing] [FILE...]\n");
    return 2;
}

static int bad_usage(const char* msg) {
    dprintf(STDERR_FILENO, "sha256: %s\n", msg);
    return usage();
}

static const struct {
    const char* name;
    char id;
} LONG[] = {{"binary", 'b'}, {"check", 'c'},  {"ignore-missing", 'i'}, {"quiet", 'q'}, {"status", 's'},
            {"strict", 'S'}, {"tag", 'g'},    {"text", 't'},           {"warn", 'w'},  {"zero", 'z'}};
#define NLONG ((int)(sizeof LONG / sizeof LONG[0]))

static int check_mode, binary = -1, tag, zero, quiet, status, warn, strict, ignore_missing;
// Whether the lines of lists have a mode character: settled by the first line
// that shows it, for every list of the run, as GNU does.
static int style = SHASUM_STYLE_NONE;

static void set_opt(char id) {
    switch (id) {
    case 'b': binary = 1; break;
    case 't': binary = 0; break;
    case 'c': check_mode = 1; break;
    case 'g': tag = 1; binary = 1; break;
    case 'z': zero = 1; break;
    case 'i': ignore_missing = 1; break;
    case 'S': strict = 1; break;
    // The last of these three said is the one that counts.
    case 'q': quiet = 1; status = warn = 0; break;
    case 's': status = 1; quiet = warn = 0; break;
    case 'w': warn = 1; status = quiet = 0; break;
    }
}

// Writes the bytes of a name, in backslash form if esc says so.
static void put_name(const char* name, uint32_t n, int esc) {
    for (uint32_t i = 0; i < n; i++) {
        if (esc && name[i] == '\\') lio_puts(&out, "\\\\");
        else if (esc && name[i] == '\n') lio_puts(&out, "\\n");
        else if (esc && name[i] == '\r') lio_puts(&out, "\\r");
        else lio_putc(&out, name[i]);
    }
}

static void put_hex(const uint8_t* d) {
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        lio_putc(&out, digits[d[i] >> 4]);
        lio_putc(&out, digits[d[i] & 15]);
    }
}

// Opens a file for hashing ("-" is standard input) and hashes all of it.
// 0 for success, else the negative error.
static int hash_file(const char* name, uint8_t digest[32]) {
    int fd = STDIN_FILENO;
    if (strcmp(name, "-")) {
        fd = sys_open(name, O_RDONLY);
        if (fd < 0) return fd;
    }
    sha256_t s;
    sha256_init(&s);
    char buf[4096];
    int n;
    while ((n = sys_read(fd, buf, sizeof buf)) > 0) sha256_update(&s, buf, (size_t)n);
    if (fd != STDIN_FILENO) sys_close(fd);
    if (n < 0) return n;
    sha256_final(&s, digest);
    return 0;
}

static int hash_one(const char* name) {
    uint8_t d[32];
    int r = hash_file(name, d);
    if (r) {
        ERR("sha256: %s: %s\n", name, strerror(r));
        return 1;
    }
    uint32_t n = (uint32_t)strlen(name);
    int esc = !zero && shasum_needs_escape(name, n);
    if (esc) lio_putc(&out, '\\');
    if (tag) {
        lio_puts(&out, "SHA256 (");
        put_name(name, n, esc);
        lio_puts(&out, ") = ");
        put_hex(d);
    } else {
        put_hex(d);
        lio_puts(&out, binary == 1 ? " *" : "  ");
        put_name(name, n, esc);
    }
    lio_putc(&out, zero ? '\0' : '\n');
    return 0;
}

// "NAME: VERDICT\n", the name in backslash form if it needs it.
static void verdict(const char* name, uint32_t n, const char* what) {
    int esc = shasum_needs_escape(name, n);
    if (esc) lio_putc(&out, '\\');
    put_name(name, n, esc);
    lio_puts(&out, what);
}

static void count_warning(uint32_t n, const char* one, const char* many) {
    ERR("sha256: WARNING: %u %s\n", n, n == 1 ? one : many);
}

// One list. 0 if all of it was good, else 1.
static int check_list(const char* list) {
    int fd = STDIN_FILENO;
    int is_stdin = !strcmp(list, "-");
    const char* shown = is_stdin ? "'standard input'" : list;
    if (!is_stdin) {
        fd = sys_open(list, O_RDONLY);
        if (fd < 0) {
            ERR("sha256: %s: %s\n", list, fd == -EISDIR ? "read error" : strerror(fd));
            return 1;
        }
    }
    lio_in_t in;
    lio_in_init(&in, fd);
    lio_line_t line = {0};
    char* name = 0;
    uint32_t name_cap = 0;
    uint32_t lineno = 0, found = 0, improper = 0, unreadable = 0, mismatch = 0, verified = 0;
    while (lio_getline(&in, &out, &line)) {
        lineno++;
        if (line.n + 1 > name_cap) {
            name_cap = line.n + 64;
            name = realloc(name, name_cap);
            if (!name) {
                ERR("sha256: out of memory\n");
                sys_exit(1);
            }
        }
        uint8_t want[32];
        uint32_t namelen;
        int r = shasum_parse(line.s, line.n, &style, want, name, &namelen);
        // The list is standard input, so a line that names "-" cannot be read.
        if (r == SHASUM_LINE && is_stdin && namelen == 1 && name[0] == '-') r = SHASUM_IMPROPER;
        if (r == SHASUM_SKIP) continue;
        if (r == SHASUM_IMPROPER) {
            improper++;
            if (warn) ERR("sha256: %s: %u: improperly formatted SHA256 checksum line\n", shown, lineno);
            continue;
        }
        found++;
        uint8_t got[32];
        int e = hash_file(name, got);
        if (e) {
            if (ignore_missing && e == -ENOENT) continue;
            ERR("sha256: %s: %s\n", name, strerror(e));
            unreadable++;
            if (!status) verdict(name, namelen, ": FAILED open or read\n");
            continue;
        }
        if (memcmp(got, want, 32)) {
            mismatch++;
            if (!status) verdict(name, namelen, ": FAILED\n");
        } else {
            verified++;
            if (!quiet && !status) verdict(name, namelen, ": OK\n");
        }
    }
    int bad = 0;
    if (in.err) {
        ERR("sha256: %s: read error\n", shown);
        bad = 1;
    }
    if (fd != STDIN_FILENO) sys_close(fd);
    free(line.s);
    free(name);
    if (!found) {
        ERR("sha256: %s: no properly formatted checksum lines found\n", shown);
        return 1;
    }
    if (!status) {
        if (improper) count_warning(improper, "line is improperly formatted", "lines are improperly formatted");
        if (unreadable) count_warning(unreadable, "listed file could not be read", "listed files could not be read");
        if (mismatch) count_warning(mismatch, "computed checksum did NOT match", "computed checksums did NOT match");
    }
    if (ignore_missing && !verified) {
        if (!status) ERR("sha256: %s: no file was verified\n", shown);
        bad = 1;
    }
    if (mismatch || unreadable || (strict && improper)) bad = 1;
    return bad;
}

int main(int argc, char** argv) {
    const char** files = malloc((size_t)argc * sizeof *files);
    int nfiles = 0;
    int only_files = 0;
    for (int a = 1; a < argc; a++) {
        const char* s = argv[a];
        if (only_files || s[0] != '-' || !s[1]) {
            files[nfiles++] = s;
            continue;
        }
        if (!strcmp(s, "--")) {
            only_files = 1;
            continue;
        }
        if (s[1] != '-') {
            for (const char* o = s + 1; *o; o++) {
                if (!strchr("bctwz", *o)) {
                    dprintf(STDERR_FILENO, "sha256: invalid option -- '%c'\n", *o);
                    return usage();
                }
                set_opt(*o);
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
            dprintf(STDERR_FILENO, "sha256: unrecognized option '%s'\n", s);
            return usage();
        }
        if (hits > 1) {
            dprintf(STDERR_FILENO, "sha256: option '%s' is ambiguous; possibilities:", s);
            for (int i = 0; i < NLONG; i++)
                if (!strncmp(LONG[i].name, s + 2, len)) dprintf(STDERR_FILENO, " '--%s'", LONG[i].name);
            dprintf(STDERR_FILENO, "\n");
            return usage();
        }
        if (s[2 + len] == '=') {
            dprintf(STDERR_FILENO, "sha256: option '--%s' doesn't allow an argument\n", LONG[hit].name);
            return usage();
        }
        set_opt(LONG[hit].id);
    }

    if (tag && binary == 0) return bad_usage("--tag does not support --text mode");
    if (check_mode) {
        if (zero) return bad_usage("the --zero option is not supported when verifying checksums");
        if (tag) return bad_usage("the --tag option is meaningless when verifying checksums");
        if (binary >= 0) return bad_usage("the --binary and --text options are meaningless when verifying checksums");
    } else {
        const char* what = ignore_missing ? "ignore-missing" : status ? "status" : warn ? "warn" : quiet ? "quiet" : strict ? "strict" : 0;
        if (what) {
            dprintf(STDERR_FILENO, "sha256: the --%s option is meaningful only when verifying checksums\n", what);
            return usage();
        }
    }

    lio_out_init(&out, STDOUT_FILENO, "sha256");
    if (!nfiles) files[nfiles++] = "-";
    int bad = 0;
    for (int i = 0; i < nfiles; i++) bad |= check_mode ? check_list(files[i]) : hash_one(files[i]);
    lio_flush(&out);
    return bad;
}
