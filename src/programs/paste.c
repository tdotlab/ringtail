// paste [-s] [-d LIST] [FILE...]
//
// Puts the lines of the FILEs side by side: line 1 of each, joined by a
// tab, then line 2 of each, and so on. A file that has run out gives
// empty fields until the longest is done. "-" is standard input, and
// every "-" shares it, so `paste - -` joins pairs of lines. With no FILE,
// standard input is what is read, unless that is the keyboard, which
// would only wait: then paste shows its usage.
//   -s  one line for each file instead: its lines joined into one, and
//       an empty file gives an empty line
//   -d  the delimiters, instead of tabs: LIST is used one character at a
//       time, round and round, starting again on each line of output
//       (and, with -s, on each file). In it \n \t \b \f \r \v are the
//       control characters, \\ is a backslash, \0 is an empty delimiter
//       (so is an empty LIST), and a backslash before anything else is
//       dropped. A backslash at the very end is an error.
//
// Every file is opened before anything is written, so a file that is not
// there stops paste with nothing printed, as in GNU paste. A last line
// with no newline gets one. It works on bytes, so a delimiter is one
// byte; there is no -z.
#include "ulib.h"
#include "lineio.h"

static int usage(void) {
    dprintf(STDERR_FILENO, "usage: paste [-s] [-d LIST] [FILE...]\n");
    return 2;
}

static lio_out_t out;

typedef struct {
    int fd;
    const char* name;
    lio_in_t* in;      // shared by every "-"
    lio_line_t l;      // the line read most recently
    int done;
} file_t;

static lio_in_t stdin_in;
static int bad;

// Delimiter number k, round and round; -1 is the empty one.
static int* dl;
static int ndl;

static void put_delim(int k) {
    int d = dl[k % ndl];
    if (d >= 0) lio_putc(&out, (char)d);
}

// The next line of f into f->l. 0 at the end (or on an error, said here).
static int next_line(file_t* f) {
    if (f->done) return 0;
    if (lio_getline(f->in, &out, &f->l)) return 1;
    f->done = 1;  // and lio_getline has emptied the line
    if (f->in->err) {
        lio_flush(&out);
        dprintf(STDERR_FILENO, "paste: %s: %s\n", f->name, strerror(f->in->err));
        bad = 1;
    }
    return 0;
}

// Turns the LIST of -d into dl. 0, or -1 for a backslash at the end.
static int parse_delims(const char* s) {
    int n = (int)strlen(s);
    dl = malloc(sizeof(int) * (uint32_t)(n + 1));
    if (!dl) {
        dprintf(STDERR_FILENO, "paste: out of memory\n");
        sys_exit(1);
    }
    ndl = 0;
    for (; *s; s++) {
        int c = (unsigned char)*s;
        if (c == '\\') {
            s++;
            switch (*s) {
            case 0: return -1;
            case '0': c = -1; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'v': c = '\v'; break;
            default: c = (unsigned char)*s; break;  // \\ and anything else
            }
        }
        dl[ndl++] = c;
    }
    if (!ndl) dl[ndl++] = -1;
    return 0;
}

int main(int argc, char** argv) {
    int nops = 0, only_ops = 0, serial = 0;
    const char* list = "\t";
    for (int a = 1; a < argc; a++) {
        const char* s = argv[a];
        if (only_ops || s[0] != '-' || !s[1]) {
            argv[1 + nops++] = argv[a];
            continue;
        }
        if (!strcmp(s, "--")) {
            only_ops = 1;
            continue;
        }
        for (const char* o = s + 1; *o; o++) {
            if (*o == 's') {
                serial = 1;
            } else if (*o == 'd') {
                list = o + 1;
                if (!*list) {
                    if (++a == argc) return usage();
                    list = argv[a];
                }
                break;
            } else {
                return usage();
            }
        }
    }
    if (parse_delims(list)) {
        dprintf(STDERR_FILENO, "paste: delimiter list ends with an unescaped backslash: %s\n", list);
        return 1;
    }
    if (!nops && sys_fdtype(STDIN_FILENO) == FDTYPE_TTY) return usage();

    int n = nops ? nops : 1;
    file_t* f = malloc(sizeof(file_t) * (uint32_t)n);
    if (!f) {
        dprintf(STDERR_FILENO, "paste: out of memory\n");
        return 1;
    }
    lio_in_init(&stdin_in, STDIN_FILENO);
    for (int i = 0; i < n; i++) {
        int stdin_arg = !nops || !strcmp(argv[1 + i], "-");
        f[i].name = stdin_arg ? "standard input" : argv[1 + i];
        f[i].l = (lio_line_t){ 0, 0, 0, 0 };
        f[i].done = 0;
        if (stdin_arg) {
            f[i].fd = -1;
            f[i].in = &stdin_in;
        } else {
            f[i].fd = sys_open(f[i].name, O_RDONLY);
            if (f[i].fd < 0) {
                fs_error("paste", f[i].name, f[i].fd);
                return 1;
            }
            f[i].in = malloc(sizeof(lio_in_t));
            if (!f[i].in) {
                dprintf(STDERR_FILENO, "paste: out of memory\n");
                return 1;
            }
            lio_in_init(f[i].in, f[i].fd);
        }
    }

    lio_out_init(&out, STDOUT_FILENO, "paste");
    if (serial) {
        for (int i = 0; i < n; i++) {
            for (int k = 0; next_line(&f[i]); k++) {
                if (k) put_delim(k - 1);
                lio_put(&out, f[i].l.s, f[i].l.n);
            }
            lio_putc(&out, '\n');
        }
    } else {
        for (;;) {
            int any = 0;
            for (int i = 0; i < n; i++) any |= next_line(&f[i]);
            if (!any) break;
            for (int i = 0; i < n; i++) {
                lio_put(&out, f[i].l.s, f[i].l.n);
                if (i + 1 < n) put_delim(i);
            }
            lio_putc(&out, '\n');
        }
    }
    lio_flush(&out);
    return bad;
}
