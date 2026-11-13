// diff [OPTION]... FILE1 FILE2
//
// Says how two files differ, line by line, the way GNU diff does (the
// same choice of changes, the same output, byte for byte). "-" is
// standard input. With a directory and a file, compares the file with
// the one of the same name in the directory; with two directories,
// compares the files they share and says which are only in one.
//   -u, -U N, --unified[=N]  unified format with N lines of context (3)
//   -c, -C N, --context[=N]  context format with N lines of context (3)
//   -q, --brief              only say whether the files differ
//   -s, --report-identical-files  say so when they do not
//   -i, --ignore-case        upper and lower case are the same
//   -b, --ignore-space-change     runs of blanks are one blank
//   -w, --ignore-all-space   blanks do not count at all
//   -a, --text               compare binary files line by line too
//   -r, --recursive          go into subdirectories too
//   -N, --new-file           a file only on one side is compared with
//                            an empty one
//   -d, --minimal            try harder for the smallest set of changes
//   --label LABEL            the name to print for a file in -u and -c
//                            headers (give it twice for both)
// Exit status: 0 the same, 1 different, 2 trouble.
#include "ulib.h"
#include "lineio.h"
#include "diffcore.h"

enum { NORMAL, UNIFIED, CONTEXT };

static int style = NORMAL;
static uint32_t context;  // the lines around each change in -u and -c
static unsigned cmp_flags;
static int brief, report_same, text, recursive, new_file;
static const char* label[2];
static char switches[512];  // the options as given, each after a space
static lio_out_t out;

static void put(void* ctx, const char* s, uint32_t n) {
    (void)ctx;
    lio_put(&out, s, n);
}

static void error(const char* fmt, const char* a, const char* b) {
    lio_flush(&out);
    dprintf(STDERR_FILENO, "diff: ");
    dprintf(STDERR_FILENO, fmt, a, b);
    dprintf(STDERR_FILENO, "\n");
}

static int usage_hint(void) {
    dprintf(STDERR_FILENO, "diff: Try 'diff --help' for more information.\n");
    return 2;
}

// A name as GNU prints it in the "diff ..." line and the headers: in
// double quotes, with C escapes, if it has a space or anything odd.
static void put_name(const char* s) {
    int quote = 0;
    for (const char* p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c <= ' ' || c == '"' || c == '\\' || c >= 0x80) quote = 1;
    }
    if (!quote) {
        lio_puts(&out, s);
        return;
    }
    lio_putc(&out, '"');
    for (const char* p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        const char* esc = 0;
        switch (c) {
        case '\a': esc = "\\a"; break;
        case '\b': esc = "\\b"; break;
        case '\t': esc = "\\t"; break;
        case '\n': esc = "\\n"; break;
        case '\v': esc = "\\v"; break;
        case '\f': esc = "\\f"; break;
        case '\r': esc = "\\r"; break;
        case '"': esc = "\\\""; break;
        case '\\': esc = "\\\\"; break;
        }
        if (esc) lio_puts(&out, esc);
        else if (c < 32 || c >= 0x80) {
            char o[5] = {'\\', (char)('0' + (c >> 6)), (char)('0' + ((c >> 3) & 7)), (char)('0' + (c & 7)), 0};
            lio_puts(&out, o);
        } else lio_putc(&out, (char)c);
    }
    lio_putc(&out, '"');
}

typedef struct {
    const char* path;
    int is_stdin, missing, is_dir;
    dirent_t st;
    char* buf;
    uint32_t len;
} input_t;

// Standard input is read once, so "diff - -" works.
static char* stdin_buf;
static uint32_t stdin_len;
static int stdin_read;

static int read_all(int fd, char** buf, uint32_t* len) {
    uint32_t cap = 4096, n = 0;
    char* b = malloc(cap);
    if (!b) return -ENOMEM;
    for (;;) {
        if (n == cap) {
            char* nb = realloc(b, cap * 2);
            if (!nb) {
                free(b);
                return -ENOMEM;
            }
            b = nb;
            cap *= 2;
        }
        int r = sys_read(fd, b + n, cap - n);
        if (r < 0) {
            free(b);
            return r;
        }
        if (r == 0) break;
        n += (uint32_t)r;
    }
    *buf = b;
    *len = n;
    return 0;
}

static int load(input_t* in) {
    if (in->missing || in->buf) return 0;
    if (in->is_stdin) {
        if (!stdin_read) {
            int r = read_all(STDIN_FILENO, &stdin_buf, &stdin_len);
            if (r < 0) {
                error("-: %s", strerror(r), 0);
                return -1;
            }
            stdin_read = 1;
        }
        in->buf = stdin_buf;
        in->len = stdin_len;
        return 0;
    }
    int fd = sys_open(in->path, O_RDONLY);
    if (fd < 0) {
        error("%s: %s", in->path, strerror(fd));
        return -1;
    }
    int r = read_all(fd, &in->buf, &in->len);
    sys_close(fd);
    if (r < 0) {
        error("%s: %s", in->path, strerror(r));
        return -1;
    }
    return 0;
}

static void unload(input_t* in) {
    if (in->buf && !in->is_stdin) free(in->buf);
    in->buf = 0;
}

// The time a header shows: the file's last write (to the FAT's two
// seconds), now for standard input, the start of 1970 for a missing file.
typedef struct { unsigned year, month, day, hour, min, sec; } stamp_t;

static stamp_t stamp_of(const input_t* in) {
    stamp_t s = {1970, 1, 1, 0, 0, 0};
    if (in->is_stdin) {
        rtc_time_t t;
        if (sys_time(&t) == 0) s = (stamp_t){t.year, t.month, t.day, t.hour, t.min, t.sec};
    } else if (!in->missing && in->st.mdate) {
        unsigned d = in->st.mdate, t = in->st.mtime;
        s = (stamp_t){1980 + (d >> 9), (d >> 5) & 15, d & 31, t >> 11, (t >> 5) & 63, (t & 31) * 2};
    }
    return s;
}

static void put_header(const char* mark, const input_t* in, int which) {
    lio_puts(&out, mark);
    lio_putc(&out, ' ');
    if (label[which]) {
        lio_puts(&out, label[which]);
        lio_putc(&out, '\n');
        return;
    }
    put_name(in->path);
    stamp_t s = stamp_of(in);
    char buf[64];
    if (style == UNIFIED) {
        snprintf(buf, sizeof buf, "\t%04u-%02u-%02u %02u:%02u:%02u.000000000 +0000\n", s.year, s.month, s.day, s.hour,
                 s.min, s.sec);
    } else {
        static const char* const days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
        static const char* const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                             "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
        unsigned m = s.month >= 1 && s.month <= 12 ? s.month : 1;
        unsigned y = m < 3 ? s.year - 1 : s.year;
        unsigned wd = (y + y / 4 - y / 100 + y / 400 + (unsigned)t[m - 1] + s.day) % 7;
        snprintf(buf, sizeof buf, "\t%s %s %2u %02u:%02u:%02u %u\n", days[wd], months[m - 1], s.day, s.hour, s.min,
                 s.sec, s.year);
    }
    lio_puts(&out, buf);
}

static const char* shown(const input_t* in, int which) {
    return label[which] ? label[which] : in->path;
}

// Two files (either may be missing under -N). depth > 0 inside a
// directory walk, where each diff gets a "diff ..." line first.
static int diff_files(input_t* in, int depth) {
    if (!in[0].missing && !in[1].missing && !in[0].is_stdin && !in[1].is_stdin && in[0].st.id == in[1].st.id) {
        if (report_same) {
            lio_puts(&out, "Files ");
            lio_puts(&out, shown(&in[0], 0));
            lio_puts(&out, " and ");
            lio_puts(&out, shown(&in[1], 1));
            lio_puts(&out, " are identical\n");
        }
        return 0;
    }
    if (load(&in[0]) || load(&in[1])) return 2;
    int same_bytes = in[0].len == in[1].len && (!in[0].len || !memcmp(in[0].buf, in[1].buf, in[0].len));
    int status;
    if (same_bytes) status = 0;
    else {
        int binary = 0;
        if (!text) {
            for (int k = 0; k < 2 && !binary; k++) {
                uint32_t n = in[k].len < 4096 ? in[k].len : 4096;
                for (uint32_t x = 0; x < n && !binary; x++) binary = in[k].buf[x] == 0;
            }
        }
        if (binary || (brief && !(cmp_flags & (DF_ICASE | DF_SPACE | DF_ALLSPACE)))) {
            lio_puts(&out, binary && !brief ? "Binary files " : "Files ");
            lio_puts(&out, shown(&in[0], 0));
            lio_puts(&out, " and ");
            lio_puts(&out, shown(&in[1], 1));
            lio_puts(&out, " differ\n");
            unload(&in[0]);
            unload(&in[1]);
            return 1;
        }
        df_file_t f[2];
        df_change_t* script = 0;
        if (df_split(&f[0], in[0].buf, in[0].len) || df_split(&f[1], in[1].buf, in[1].len) ||
            df_compare(f, cmp_flags, style == NORMAL ? 0 : context, &script)) {
            error("memory exhausted", 0, 0);
            return 2;
        }
        status = script ? 1 : 0;
        if (script && brief) {
            lio_puts(&out, "Files ");
            lio_puts(&out, shown(&in[0], 0));
            lio_puts(&out, " and ");
            lio_puts(&out, shown(&in[1], 1));
            lio_puts(&out, " differ\n");
        } else if (script) {
            if (depth > 0) {
                lio_puts(&out, "diff");
                lio_puts(&out, switches);
                lio_putc(&out, ' ');
                put_name(shown(&in[0], 0));
                lio_putc(&out, ' ');
                put_name(shown(&in[1], 1));
                lio_putc(&out, '\n');
            }
            if (style == UNIFIED) {
                put_header("---", &in[0], 0);
                put_header("+++", &in[1], 1);
                df_print_unified(put, 0, f, script, context);
            } else if (style == CONTEXT) {
                put_header("***", &in[0], 0);
                put_header("---", &in[1], 1);
                df_print_context(put, 0, f, script, context);
            } else df_print_normal(put, 0, f, script);
        }
        df_free_script(script);
        df_free_file(&f[0]);
        df_free_file(&f[1]);
    }
    if (status == 0 && report_same) {
        lio_puts(&out, "Files ");
        lio_puts(&out, shown(&in[0], 0));
        lio_puts(&out, " and ");
        lio_puts(&out, shown(&in[1], 1));
        lio_puts(&out, " are identical\n");
    }
    unload(&in[0]);
    unload(&in[1]);
    return status;
}

static int compare(const char* p0, const char* p1, int depth, int missing0, int missing1);

// Names in byte order, as GNU diff has them in the C locale.
static void sort_names(dirent_t* d, uint32_t n) {
    for (uint32_t i = 1; i < n; i++) {
        dirent_t x = d[i];
        uint32_t j = i;
        for (; j > 0 && strcmp(d[j - 1].name, x.name) > 0; j--) d[j] = d[j - 1];
        d[j] = x;
    }
}

static int diff_dirs(const char* p0, const char* p1, int depth, int missing0, int missing1) {
    dirent_t* list[2] = {0, 0};
    uint32_t count[2] = {0, 0};
    const char* path[2] = {p0, p1};
    int missing[2] = {missing0, missing1};
    for (int k = 0; k < 2; k++) {
        if (missing[k]) continue;
        int err = 0;
        list[k] = read_dir(path[k], &count[k], &err);
        if (!list[k]) {
            if (err != 0) {
                error("%s: %s", path[k], strerror(err));
                free(list[0]);
                return 2;
            }
            count[k] = 0;
        }
        sort_names(list[k], count[k]);
    }
    int status = 0;
    uint32_t i = 0, j = 0;
    char a[PATH_MAX], b[PATH_MAX];
    while (i < count[0] || j < count[1]) {
        int c = i == count[0] ? 1 : j == count[1] ? -1 : strcmp(list[0][i].name, list[1][j].name);
        const char* name = c <= 0 ? list[0][i].name : list[1][j].name;
        if (join_path(a, sizeof a, p0, name) || join_path(b, sizeof b, p1, name)) {
            error("%s/%s: file name too long", c <= 0 ? p0 : p1, name);
            status = 2;
        } else if (c != 0 && !new_file) {
            lio_puts(&out, "Only in ");
            lio_puts(&out, c < 0 ? p0 : p1);
            lio_puts(&out, ": ");
            lio_puts(&out, name);
            lio_putc(&out, '\n');
            if (status < 1) status = 1;
        } else {
            int r = compare(a, b, depth + 1, c > 0, c < 0);
            if (r > status) status = r;
        }
        if (c <= 0) i++;
        if (c >= 0) j++;
    }
    free(list[0]);
    free(list[1]);
    return status;
}

static int stat_input(input_t* in, const char* path, int missing) {
    memset(in, 0, sizeof *in);
    in->path = path;
    in->missing = missing;
    if (missing) return 0;
    if (!strcmp(path, "-")) {
        in->is_stdin = 1;
        return 0;
    }
    int r = sys_stat(path, &in->st);
    if (r < 0) return r;
    in->is_dir = (in->st.flags & DIRENT_DIR) != 0;
    return 0;
}

static int compare(const char* p0, const char* p1, int depth, int missing0, int missing1) {
    input_t in[2];
    int r0 = stat_input(&in[0], p0, missing0), r1 = stat_input(&in[1], p1, missing1);
    if (depth == 0 && new_file && r0 == -ENOENT && r1 == 0) r0 = stat_input(&in[0], p0, 1);
    if (depth == 0 && new_file && r1 == -ENOENT && r0 == 0) r1 = stat_input(&in[1], p1, 1);
    if (r0 < 0 || r1 < 0) {
        if (r0 < 0) error("%s: %s", p0, strerror(r0));
        if (r1 < 0) error("%s: %s", p1, strerror(r1));
        return 2;
    }
    // A missing side takes the kind of the other.
    if (in[0].missing) in[0].is_dir = in[1].is_dir;
    if (in[1].missing) in[1].is_dir = in[0].is_dir;
    if (in[0].is_dir && in[1].is_dir) {
        if (depth > 0 && !recursive) {
            lio_puts(&out, "Common subdirectories: ");
            lio_puts(&out, p0);
            lio_puts(&out, " and ");
            lio_puts(&out, p1);
            lio_putc(&out, '\n');
            return 0;
        }
        return diff_dirs(p0, p1, depth, in[0].missing, in[1].missing);
    }
    if (in[0].is_dir != in[1].is_dir) {
        if (depth == 0) {
            // A file and a directory: the file against the one of its name in there.
            int d = in[0].is_dir ? 0 : 1;
            if (in[1 - d].is_stdin) {
                error("cannot compare '-' to a directory", 0, 0);
                return 2;
            }
            const char* file = in[1 - d].path;
            const char* base = strrchr(file, '/') ? strrchr(file, '/') + 1 : file;
            static char joined[PATH_MAX];
            if (join_path(joined, sizeof joined, in[d].path, base)) {
                error("%s/%s: file name too long", in[d].path, base);
                return 2;
            }
            int r = d == 0 ? stat_input(&in[0], joined, 0) : stat_input(&in[1], joined, 0);
            if (r < 0) {
                error("%s: %s", joined, strerror(r));
                return 2;
            }
            if (in[d].is_dir) {
                error("%s: Is a directory", joined, 0);
                return 2;
            }
            return diff_files(in, 0);
        }
        lio_puts(&out, "File ");
        lio_puts(&out, shown(&in[0], 0));
        lio_puts(&out, in[0].is_dir ? " is a directory while file " : " is a regular file while file ");
        lio_puts(&out, shown(&in[1], 1));
        lio_puts(&out, in[1].is_dir ? " is a directory\n" : " is a regular file\n");
        return 1;
    }
    return diff_files(in, depth);
}

static void add_switch(const char* s) {
    size_t n = strlen(switches);
    snprintf(switches + n, sizeof switches - n, " %s", s);
}

// "--name" or "--name=value" against a long option; 1 if it matches
// (*value set to the value or 0), 0 if not.
static int long_opt(const char* arg, const char* name, const char** value) {
    size_t n = strlen(name);
    if (strncmp(arg + 2, name, n)) return 0;
    if (arg[2 + n] == 0) {
        *value = 0;
        return 1;
    }
    if (arg[2 + n] == '=') {
        *value = arg + 3 + n;
        return 1;
    }
    return 0;
}

// Like GNU: one output style only, and the largest context asked for.
static int set_style(int st, const char* v) {
    if (style != NORMAL && style != st) {
        dprintf(STDERR_FILENO, "diff: conflicting output style options\n");
        return usage_hint();
    }
    style = st;
    uint32_t n = 3;
    if (v && parse_uint(v, &n)) {
        dprintf(STDERR_FILENO, "diff: invalid context length '%s'\n", v);
        return usage_hint();
    }
    if (n > context) context = n;
    return 0;
}

int main(int argc, char** argv) {
    const char* operand[2];
    int nops = 0, labels = 0, only_operands = 0;
    for (int a = 1; a < argc; a++) {
        const char* arg = argv[a];
        if (only_operands || arg[0] != '-' || !arg[1]) {
            if (nops == 2) {
                dprintf(STDERR_FILENO, "diff: extra operand '%s'\n", arg);
                return usage_hint();
            }
            operand[nops++] = arg;
            continue;
        }
        if (!strcmp(arg, "--")) {
            only_operands = 1;
            continue;
        }
        add_switch(arg);
        if (arg[1] == '-') {
            const char* v;
            if (long_opt(arg, "unified", &v) || long_opt(arg, "context", &v)) {
                if (set_style(arg[2] == 'u' ? UNIFIED : CONTEXT, v)) return 2;
            } else if (!strcmp(arg, "--label")) {
                if (++a == argc) {
                    dprintf(STDERR_FILENO, "diff: option '--label' requires an argument\n");
                    return usage_hint();
                }
                add_switch(argv[a]);
                if (labels == 2) {
                    dprintf(STDERR_FILENO, "diff: too many file label options\n");
                    return 2;
                }
                label[labels++] = argv[a];
            } else if (!strncmp(arg, "--label=", 8)) {
                if (labels == 2) {
                    dprintf(STDERR_FILENO, "diff: too many file label options\n");
                    return 2;
                }
                label[labels++] = arg + 8;
            } else if (!strcmp(arg, "--brief")) brief = 1;
            else if (!strcmp(arg, "--report-identical-files")) report_same = 1;
            else if (!strcmp(arg, "--ignore-case")) cmp_flags |= DF_ICASE;
            else if (!strcmp(arg, "--ignore-space-change")) cmp_flags |= DF_SPACE;
            else if (!strcmp(arg, "--ignore-all-space")) cmp_flags |= DF_ALLSPACE;
            else if (!strcmp(arg, "--text")) text = 1;
            else if (!strcmp(arg, "--recursive")) recursive = 1;
            else if (!strcmp(arg, "--new-file")) new_file = 1;
            else if (!strcmp(arg, "--minimal")) cmp_flags |= DF_MINIMAL;
            else {
                dprintf(STDERR_FILENO, "diff: unrecognized option '%s'\n", arg);
                return usage_hint();
            }
            continue;
        }
        for (const char* o = arg + 1; *o; o++) {
            switch (*o) {
            case 'u':
            case 'c':
                if (set_style(*o == 'u' ? UNIFIED : CONTEXT, 0)) return 2;
                break;
            case 'q': brief = 1; break;
            case 's': report_same = 1; break;
            case 'i': cmp_flags |= DF_ICASE; break;
            case 'b': cmp_flags |= DF_SPACE; break;
            case 'w': cmp_flags |= DF_ALLSPACE; break;
            case 'a': text = 1; break;
            case 'r': recursive = 1; break;
            case 'N': new_file = 1; break;
            case 'd': cmp_flags |= DF_MINIMAL; break;
            case 'U':
            case 'C': {
                const char* v = o + 1;
                if (!*v) {
                    if (++a == argc) {
                        dprintf(STDERR_FILENO, "diff: option requires an argument -- '%c'\n", *o);
                        return usage_hint();
                    }
                    v = argv[a];
                    add_switch(v);
                }
                if (set_style(*o == 'U' ? UNIFIED : CONTEXT, v)) return 2;
                o += strlen(o) - 1;
                break;
            }
            default:
                dprintf(STDERR_FILENO, "diff: invalid option -- '%c'\n", *o);
                return usage_hint();
            }
        }
    }
    if (nops < 2) {
        if (nops == 0) dprintf(STDERR_FILENO, "diff: missing operand after 'diff'\n");
        else dprintf(STDERR_FILENO, "diff: missing operand after '%s'\n", operand[0]);
        return usage_hint();
    }
    lio_out_init(&out, STDOUT_FILENO, "diff");
    int status = compare(operand[0], operand[1], 0, 0, 0);
    lio_flush(&out);
    return status;
}
