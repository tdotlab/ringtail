// join [OPTION]... FILE1 FILE2
//
// Writes a line for each pair of lines, one from FILE1 and one from FILE2,
// that have the same join field (the first field unless told otherwise):
// the join field, the rest of the line of FILE1, the rest of the line of
// FILE2. Both files must be sorted bytewise on their join fields (see
// below for what happens if they are not). "-" is standard input, for one
// of the two. Fields are separated by blanks (any run of them; blanks at the
// start of a line are ignored) unless -t says otherwise.
//   -1 N, -2 N, -j N  the join field of FILE1, of FILE2, of both (counted from
//                     1); -j1 and -j2 written like that mean -1 and -2 only
//                     when a file name follows them, as in GNU join
//   -t C              fields are separated by C (and the output too); an empty
//                     C makes the whole line one field; \0 is a NUL
//   -a N              also write the lines of file N that have no partner
//   -v N              the same, but write only those (-v1 -v2: only the
//                     lines without a partner)
//   -e STR            put STR for a field that is missing or empty
//   -i                ignore case in the join fields
//   -o LIST           what to write: a list (commas or blanks) of 0 (the join
//                     field) and N.M (field M of file N), or "auto": as many
//                     fields as the first line of each file has
//   -z                lines end in NUL, not newline
//   --header          the first line of each file is a heading: write the two
//                     joined, and pair the rest
//   --check-order     stop with an error at the first line out of order
//   --nocheck-order   never check
// Without either, the order is checked only once a line has had no partner,
// and a line out of order is reported (once for each file) while the work
// goes on; the end result is then an error status. Every line is checked
// against the one before it in the same file as it is read. A long option
// may be shortened as long as it stays unique. A usage error is status 2;
// other errors are status 1.
#include "ulib.h"
#include "lineio.h"
#include "joincore.h"
#include "quotearg.h"

#define TRY "Try 'join --help' for more information.\n"

typedef struct {
    lio_line_t raw;
    jn_field_t* f;
    uint32_t nf, fcap;
} jline_t;

enum { CHECK_DEFAULT, CHECK_ON, CHECK_OFF };

typedef struct {
    lio_in_t in;
    const char* name;
    int isdir;
    uint32_t line_no;
    char* pkey;  // the join field of the line read before this one
    uint32_t pn, pcap;
    int have_prev;
    int warned;
    jline_t** lines;  // the lines held: the first, and those that equal it
    uint32_t count, alloc;
} file_t;

static file_t files[2];
static lio_out_t out;
static jline_t blank;  // the line that is not there, for a line without a partner

static int tab = JN_BLANKS;
static char eol = '\n';
static int icase, autoformat, header;
static int print_pairables = 1, unpair[2];
static int order_mode = CHECK_DEFAULT;
static int seen_unpairable;
static const char* filler;
static int64_t jf[2] = { -1, -1 };
static uint32_t autocount[2];

static struct {
    int file;
    uint64_t field;
} * outlist;
static uint32_t nout, outcap;

static void* xrealloc(void* p, uint32_t n) {
    void* r = realloc(p, n);
    if (!r) {
        dprintf(STDERR_FILENO, "join: memory exhausted\n");
        sys_exit(1);
    }
    return r;
}

static __attribute__((noreturn)) void fail(void) { sys_exit(1); }

// ---- quoting names in messages ---------------------------------------------
// GNU join quotes what it prints in three ways (see quotearg.h), and the
// messages are only the same if the quoting is.
enum { Q_C = QA_C, Q_ALWAYS = QA_ALWAYS, Q_FILE = QA_FILE };

// Returns the quoted text of `s`, in memory that is never freed (the
// program ends soon after a message).
static const char* quoted(const char* s, int style) {
    char* o = xrealloc(0, QA_SPACE(strlen(s)));
    qa_quote(s, style, o);
    return o;
}

static __attribute__((noreturn)) void fail_with(const char* what, const char* arg) {
    dprintf(STDERR_FILENO, "join: %s %s\n", what, quoted(arg, Q_C));
    fail();
}

// ---- reading ---------------------------------------------------------------

static void key_of(const jline_t* l, uint64_t field, const char** p, uint32_t* n) {
    if (field < l->nf) {
        *p = l->raw.s + l->f[field].beg;
        *n = l->f[field].len;
    } else {
        *p = "";
        *n = 0;
    }
}

static int keycmp(const jline_t* a, const jline_t* b, uint64_t f1, uint64_t f2) {
    const char *pa, *pb;
    uint32_t na, nb;
    key_of(a, f1, &pa, &na);
    key_of(b, f2, &pb, &nb);
    return jn_keycmp(pa, na, pb, nb, icase);
}

// Checks the line just read from input w against the one before it.
static void check_order(int w, const jline_t* cur) {
    file_t* f = &files[w];
    if (order_mode == CHECK_OFF) return;
    const char* k;
    uint32_t kn;
    key_of(cur, (uint64_t)jf[w], &k, &kn);
    if (f->have_prev && !f->warned && (order_mode == CHECK_ON || seen_unpairable) && jn_keycmp(f->pkey, f->pn, k, kn, icase) > 0) {
        lio_flush(&out);
        dprintf(STDERR_FILENO, "join: %s:%u: is not sorted: ", f->name, f->line_no);
        uint32_t n = 0;
        while (n < cur->raw.n && cur->raw.s[n]) n++;  // as printf("%.*s") does, it stops at a NUL
        if (n) sys_write(STDERR_FILENO, cur->raw.s, n);
        sys_write(STDERR_FILENO, "\n", 1);
        if (order_mode == CHECK_ON) fail();
        f->warned = 1;
    }
    if (kn > f->pcap) {
        f->pcap = kn;
        f->pkey = xrealloc(f->pkey, kn);
    }
    if (kn) memcpy(f->pkey, k, kn);
    f->pn = kn;
    f->have_prev = 1;
}

// Reads the next line of input w into *slot (made if there is none yet).
static int get_line(int w, jline_t** slot) {
    file_t* f = &files[w];
    jline_t* l = *slot;
    if (!l) {
        l = xrealloc(0, sizeof *l);
        memset(l, 0, sizeof *l);
        *slot = l;
    }
    if (f->isdir) {
        lio_flush(&out);
        dprintf(STDERR_FILENO, "join: read error: %s\n", strerror(-EISDIR));
        fail();
    }
    if (!lio_getline_delim(&f->in, &out, &l->raw, eol)) {
        if (f->in.err) {
            lio_flush(&out);
            dprintf(STDERR_FILENO, "join: read error: %s\n", strerror(f->in.err));
            fail();
        }
        return 0;
    }
    f->line_no++;
    l->nf = jn_split(l->raw.s, l->raw.n, tab, l->f, l->fcap);
    if (l->nf > l->fcap) {
        l->fcap = l->nf;
        l->f = xrealloc(l->f, l->fcap * (uint32_t)sizeof *l->f);
        jn_split(l->raw.s, l->raw.n, tab, l->f, l->fcap);
    }
    check_order(w, l);
    return 1;
}

static int getseq(int w) {
    file_t* f = &files[w];
    if (f->count == f->alloc) {
        uint32_t na = f->alloc ? f->alloc * 2 : 4;
        f->lines = xrealloc(f->lines, na * (uint32_t)sizeof *f->lines);
        for (uint32_t i = f->alloc; i < na; i++) f->lines[i] = 0;
        f->alloc = na;
    }
    if (!get_line(w, &f->lines[f->count])) return 0;
    f->count++;
    return 1;
}

// Reads a line as the first (`first`) or the next one held.
static int advance_seq(int w, int first) {
    if (first) files[w].count = 0;
    return getseq(w);
}

// ---- writing ---------------------------------------------------------------

static void prfield(uint64_t n, const jline_t* l) {
    if (n < l->nf && l->f[n].len) lio_put(&out, l->raw.s + l->f[n].beg, l->f[n].len);
    else if (filler) lio_puts(&out, filler);
}

// The fields of l except the join field, each after a separator.
static void prfields(const jline_t* l, uint64_t join_field, uint32_t count) {
    uint64_t nfields = autoformat ? count : l->nf;
    char sep = tab < 0 ? ' ' : (char)tab;
    for (uint64_t i = 0; i < join_field && i < nfields; i++) {
        lio_putc(&out, sep);
        prfield(i, l);
    }
    for (uint64_t i = join_field + 1; i < nfields; i++) {
        lio_putc(&out, sep);
        prfield(i, l);
    }
}

static void prjoin(const jline_t* a, const jline_t* b) {
    char sep = tab < 0 ? ' ' : (char)tab;
    if (nout) {
        for (uint32_t i = 0; i < nout; i++) {
            const jline_t* l;
            uint64_t field;
            if (outlist[i].file == 0) {
                if (a == &blank) {
                    l = b;
                    field = (uint64_t)jf[1];
                } else {
                    l = a;
                    field = (uint64_t)jf[0];
                }
            } else {
                l = outlist[i].file == 1 ? a : b;
                field = outlist[i].field;
            }
            if (i) lio_putc(&out, sep);
            prfield(field, l);
        }
    } else {
        const jline_t* l = a == &blank ? b : a;
        prfield((uint64_t)(a == &blank ? jf[1] : jf[0]), l);
        prfields(a, (uint64_t)jf[0], autocount[0]);
        prfields(b, (uint64_t)jf[1], autocount[1]);
    }
    lio_putc(&out, eol);
}

// ---- joining ---------------------------------------------------------------

static void swap_lines(jline_t** a, jline_t** b) {
    jline_t* t = *a;
    *a = *b;
    *b = t;
}

static void join_files(void) {
    file_t *a = &files[0], *b = &files[1];
    getseq(0);
    getseq(1);
    if (autoformat) {
        autocount[0] = a->count ? a->lines[0]->nf : 0;
        autocount[1] = b->count ? b->lines[0]->nf : 0;
    }
    if (header && (a->count || b->count)) {
        prjoin(a->count ? a->lines[0] : &blank, b->count ? b->lines[0] : &blank);
        a->have_prev = b->have_prev = 0;
        if (a->count) advance_seq(0, 1);
        if (b->count) advance_seq(1, 1);
    }

    while (a->count && b->count) {
        int diff = keycmp(a->lines[0], b->lines[0], (uint64_t)jf[0], (uint64_t)jf[1]);
        if (diff < 0) {
            if (unpair[0]) prjoin(a->lines[0], &blank);
            advance_seq(0, 1);
            seen_unpairable = 1;
            continue;
        }
        if (diff > 0) {
            if (unpair[1]) prjoin(&blank, b->lines[0]);
            advance_seq(1, 1);
            seen_unpairable = 1;
            continue;
        }
        // Read on in each file while the lines still equal the first of the other.
        int eof1 = 0, eof2 = 0;
        do {
            if (!advance_seq(0, 0)) {
                eof1 = 1;
                a->count++;
                break;
            }
        } while (!keycmp(a->lines[a->count - 1], b->lines[0], (uint64_t)jf[0], (uint64_t)jf[1]));
        do {
            if (!advance_seq(1, 0)) {
                eof2 = 1;
                b->count++;
                break;
            }
        } while (!keycmp(a->lines[0], b->lines[b->count - 1], (uint64_t)jf[0], (uint64_t)jf[1]));

        if (print_pairables)
            for (uint32_t i = 0; i + 1 < a->count; i++)
                for (uint32_t j = 0; j + 1 < b->count; j++) prjoin(a->lines[i], b->lines[j]);

        if (!eof1) {
            swap_lines(&a->lines[0], &a->lines[a->count - 1]);
            a->count = 1;
        } else {
            a->count = 0;
        }
        if (!eof2) {
            swap_lines(&b->lines[0], &b->lines[b->count - 1]);
            b->count = 1;
        } else {
            b->count = 0;
        }
    }

    // What is left of the file that has not ended is read to the end: to write
    // the lines without a partner, and to check their order.
    jline_t* line = 0;
    int checktail = order_mode != CHECK_OFF && !(a->warned && b->warned);
    for (int w = 0; w < 2; w++) {
        file_t* f = &files[w];
        if (!((unpair[w] || checktail) && f->count)) continue;
        if (unpair[w]) {
            if (w) prjoin(&blank, f->lines[0]);
            else prjoin(f->lines[0], &blank);
        }
        while (get_line(w, &line)) {
            if (unpair[w]) {
                if (w) prjoin(&blank, line);
                else prjoin(line, &blank);
            }
            if (f->warned && !unpair[w]) break;
        }
    }
}

// ---- options ---------------------------------------------------------------

static int usage_error(void) {
    dprintf(STDERR_FILENO, TRY);
    return 2;
}

static void help(void) {
    static const char text[] =
        "usage: join [OPTION]... FILE1 FILE2\n"
        "For each pair of lines with identical join fields, writes a line made of the\n"
        "join field, the rest of the line of FILE1 and the rest of the line of FILE2.\n"
        "Both files must be sorted on their join fields. \"-\" is standard input.\n"
        "  -a N              also write the lines of file N (1 or 2) without a partner\n"
        "  -v N              like -a N, but write only those lines\n"
        "  -e STR            put STR in place of a missing or empty field\n"
        "  -i, --ignore-case ignore case in the join fields\n"
        "  -j N              join on field N of both files\n"
        "  -1 N, -2 N        join on field N of file 1, of file 2\n"
        "  -o LIST           write these fields: 0 (the join field) and N.M (field M of\n"
        "                    file N), separated by commas or blanks; or \"auto\" for as\n"
        "                    many fields as the first line of each file has\n"
        "  -t CHAR           fields are separated by CHAR (empty: the line is one field)\n"
        "  -z, --zero-terminated  lines end in NUL, not newline\n"
        "      --check-order     stop at the first line that is out of order\n"
        "      --nocheck-order   do not check the order\n"
        "      --header          the first lines are headings: join them, pair the rest\n"
        "      --help            this text\n"
        "Fields are separated by blanks unless -t is given; FIELD counts from 1.\n";
    sys_write(STDOUT_FILENO, text, sizeof text - 1);
}

static void set_join_field(int which, uint64_t v) {
    if (jf[which] >= 0 && (uint64_t)jf[which] != v) {
        char x[24], y[24];
        jn_u64_text((uint64_t)jf[which], x);
        jn_u64_text(v, y);
        dprintf(STDERR_FILENO, "join: incompatible join fields %s, %s\n", x, y);
        fail();
    }
    jf[which] = (int64_t)v;
}

static uint64_t field_number(const char* s) {
    uint64_t v;
    if (jn_parse_field(s, &v)) fail_with("invalid field number:", s);
    return v;
}

static void add_spec(int file, uint64_t field) {
    if (nout == outcap) {
        outcap = outcap ? outcap * 2 : 8;
        outlist = xrealloc(outlist, outcap * (uint32_t)sizeof *outlist);
    }
    outlist[nout].file = file;
    outlist[nout].field = field;
    nout++;
}

// Adds the items of an -o list (separated by commas, spaces and tabs).
static void add_field_list(const char* list) {
    uint32_t len = (uint32_t)strlen(list);
    char* copy = xrealloc(0, len + 1);
    memcpy(copy, list, len + 1);
    char* p = copy;
    for (;;) {
        char* item = p;
        while (*p && *p != ',' && *p != ' ' && *p != '\t') p++;
        int more = *p != 0;
        *p = 0;
        int file;
        uint64_t field;
        int r = jn_decode_spec(item, &file, &field);
        if (r == JN_BAD_SPEC) fail_with("invalid field specifier:", item);
        if (r == JN_BAD_FILE) fail_with("invalid file number in field spec:", item);
        if (r == JN_BAD_FIELD) fail_with("invalid field number:", item + 2);
        add_spec(file, field);
        if (!more) break;
        p++;
    }
    free(copy);
}

enum { MUST_BE_OPERAND, MIGHT_BE_J1_ARG, MIGHT_BE_J2_ARG, MIGHT_BE_O_ARG };

static const char* names[2];
static int operand_status[2], joption_count[2], nfiles, prev_status, status;

// -1 to go on, or the status to end with.
static int add_file_name(const char* name) {
    int n = nfiles;
    if (n == 2) {
        int op0 = operand_status[0] == MUST_BE_OPERAND;
        const char* arg = names[op0];
        switch (operand_status[op0]) {
            case MUST_BE_OPERAND:
                dprintf(STDERR_FILENO, "join: extra operand %s\n", quoted(name, Q_ALWAYS));
                return usage_error();
            case MIGHT_BE_J1_ARG:
                joption_count[0]--;
                set_join_field(0, field_number(arg));
                break;
            case MIGHT_BE_J2_ARG:
                joption_count[1]--;
                set_join_field(1, field_number(arg));
                break;
            case MIGHT_BE_O_ARG:
                add_field_list(arg);
                break;
        }
        if (!op0) {
            operand_status[0] = operand_status[1];
            names[0] = names[1];
        }
        n = 1;
    }
    operand_status[n] = prev_status;
    names[n] = name;
    nfiles = n + 1;
    if (prev_status == MIGHT_BE_O_ARG) status = MIGHT_BE_O_ARG;
    return -1;
}

static const struct {
    const char* name;
    char id;
} LONG[] = { { "ignore-case", 'i' }, { "check-order", 'c' }, { "nocheck-order", 'n' }, { "zero-terminated", 'z' },
             { "header", 'H' },      { "help", 'h' },        { "version", 'V' } };
#define NLONG ((int)(sizeof LONG / sizeof LONG[0]))

// One option. `attached_j` says -j had its value right after it (-j1).
static int apply(char c, const char* arg, int attached_j) {
    switch (c) {
        case 'v':
            print_pairables = 0;
            // fall through
        case 'a': {
            int n = jn_parse_filenum(arg);
            if (!n) fail_with("invalid field number:", arg);
            unpair[n - 1] = 1;
            break;
        }
        case 'e':
            if (filler && strcmp(filler, arg)) {
                dprintf(STDERR_FILENO, "join: conflicting empty-field replacement strings\n");
                fail();
            }
            filler = arg;
            break;
        case 'i':
            icase = 1;
            break;
        case '1':
            set_join_field(0, field_number(arg));
            break;
        case '2':
            set_join_field(1, field_number(arg));
            break;
        case 'j':
            if ((arg[0] == '1' || arg[0] == '2') && !arg[1] && attached_j) {
                joption_count[arg[0] - '1']++;
                status = MIGHT_BE_J1_ARG + (arg[0] - '1');
            } else {
                uint64_t v = field_number(arg);
                set_join_field(0, v);
                set_join_field(1, v);
            }
            break;
        case 'o':
            if (!strcmp(arg, "auto")) {
                autoformat = 1;
            } else {
                add_field_list(arg);
                status = MIGHT_BE_O_ARG;
            }
            break;
        case 't': {
            int nt = (unsigned char)arg[0];
            if (!nt) {
                nt = '\n';
            } else if (arg[1]) {
                if (!strcmp(arg, "\\0")) {
                    nt = 0;
                } else {
                    fail_with("multi-character tab", arg);
                }
            }
            if (tab >= 0 && tab != nt) {
                dprintf(STDERR_FILENO, "join: incompatible tabs\n");
                fail();
            }
            tab = nt;
            break;
        }
        case 'z':
            eol = 0;
            break;
        case 'n':
            order_mode = CHECK_OFF;
            break;
        case 'c':
            order_mode = CHECK_ON;
            break;
        case 'H':
            header = 1;
            break;
    }
    return -1;
}

int main(int argc, char** argv) {
    int a = 1;
    for (; a < argc; a++) {
        const char* s = argv[a];
        status = MUST_BE_OPERAND;
        if (s[0] != '-' || !s[1]) {
            int r = add_file_name(s);
            if (r >= 0) return r;
            prev_status = status;
            continue;
        }
        if (!strcmp(s, "--")) {
            a++;
            break;
        }
        if (s[1] == '-') {
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
            if (!hits) {
                dprintf(STDERR_FILENO, "join: unrecognized option '%s'\n", s);
                return usage_error();
            }
            if (hits > 1) {
                dprintf(STDERR_FILENO, "join: option '%s' is ambiguous; possibilities:", s);
                for (int i = 0; i < NLONG; i++)
                    if (!strncmp(LONG[i].name, s + 2, len)) dprintf(STDERR_FILENO, " '--%s'", LONG[i].name);
                dprintf(STDERR_FILENO, "\n");
                return usage_error();
            }
            if (s[2 + len] == '=') {
                dprintf(STDERR_FILENO, "join: option '--%s' doesn't allow an argument\n", LONG[hit].name);
                return usage_error();
            }
            char id = LONG[hit].id;
            if (id == 'h') {
                help();
                return 0;
            }
            if (id == 'V') {
                dprintf(STDERR_FILENO, "join: option '--version' is not supported\n");
                return 2;
            }
            apply(id, "", 0);
            prev_status = status;
            continue;
        }
        for (const char* o = s + 1; *o; o++) {
            char c = *o;
            status = MUST_BE_OPERAND;
            if (c == 'i' || c == 'z') {
                apply(c, "", 0);
            } else if (strchr("ae12jotv", c)) {
                const char* arg;
                int attached = 0;
                if (o[1]) {
                    arg = o + 1;
                    attached = o == s + 1;
                } else if (a + 1 < argc) {
                    arg = argv[++a];
                } else {
                    dprintf(STDERR_FILENO, "join: option requires an argument -- '%c'\n", c);
                    return usage_error();
                }
                apply(c, arg, attached);
                prev_status = status;
                break;  // the rest of the argument was the value
            } else {
                dprintf(STDERR_FILENO, "join: invalid option -- '%c'\n", c);
                return usage_error();
            }
            prev_status = status;
        }
    }
    // What follows "--" is all operands.
    prev_status = MUST_BE_OPERAND;
    for (; a < argc; a++) {
        int r = add_file_name(argv[a]);
        if (r >= 0) return r;
    }

    if (nfiles != 2) {
        if (nfiles == 0) dprintf(STDERR_FILENO, "join: missing operand\n");
        else dprintf(STDERR_FILENO, "join: missing operand after %s\n", quoted(argv[argc - 1], Q_C));
        return usage_error();
    }
    // A -j1 or -j2 that was given no value after it was "-j 1" or "-j 2".
    for (int i = 0; i < 2; i++) {
        if (joption_count[i]) {
            set_join_field(0, (uint64_t)i);
            set_join_field(1, (uint64_t)i);
        }
    }
    if (jf[0] < 0) jf[0] = 0;
    if (jf[1] < 0) jf[1] = 0;

    int stdin_used = 0;
    for (int i = 0; i < 2; i++) {
        files[i].name = names[i];
        int fd = STDIN_FILENO;
        if (!strcmp(names[i], "-")) {
            stdin_used++;
        } else {
            fd = sys_open(names[i], O_RDONLY);
            if (fd == -EISDIR) {
                files[i].isdir = 1;  // fopen works on a directory; the first read is what fails
            } else if (fd < 0) {
                dprintf(STDERR_FILENO, "join: %s: %s\n", quoted(names[i], Q_FILE), strerror(fd));
                return 1;
            }
        }
        lio_in_init(&files[i].in, fd);
    }
    if (stdin_used == 2) {
        dprintf(STDERR_FILENO, "join: both files cannot be standard input\n");
        return 1;
    }

    lio_out_init(&out, STDOUT_FILENO, "join");
    join_files();
    lio_flush(&out);
    if (files[0].warned || files[1].warned) {
        dprintf(STDERR_FILENO, "join: input is not in sorted order\n");
        return 1;
    }
    return 0;
}
