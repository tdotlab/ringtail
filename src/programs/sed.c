// sed [-nsEr] [-i[SUFFIX]] [-e SCRIPT] [-f FILE] [SCRIPT] [FILE...]
//
// A stream editor: for each line of the input it runs the SCRIPT on the
// "pattern space" (the line) and prints the result.
//   -n   print only what the script prints (p, P, l, = and s///p)
//   -e   add a piece of script (several are joined with newlines); -f
//        reads script from a file; with neither, the first operand is it
//   -E, -r   extended regular expressions (default: basic, with the GNU
//        \+ \? \| extras)
//   -s   treat each FILE separately: line numbers, "$" and ranges per file
//   -i[SUFFIX]   edit the files in place (implies -s); with a SUFFIX the
//        old file is kept under its name plus SUFFIX
//
// Addresses (before a command; none = every line):
//   N          line number           $         the last line
//   /RE/       lines matching RE (RE may end in I: ignore case)
//   \cREc      the same with another delimiter
//   FIRST~STEP lines FIRST, FIRST+STEP, ...
//   A,B   a range: from a line matching A to the next one matching B;
//         B may be a line number, $, /RE/, +N (N lines more) or ~N (up to
//         a multiple of N). 0,/RE/ lets RE match on the first line too.
//   ADDR! applies the command to the lines that do NOT match.
// Commands:
//   {  }       a group of commands under one address
//   s/RE/REPL/FLAGS   substitute; & or \0 is the match, \1..\9 the groups,
//         \n newline, \t tab, \L \U \l \u \E change case. FLAGS: g (all),
//         N (the Nth), p (print), i or I (ignore case), w FILE
//   y/ABC/xyz/ translate characters
//   d D        delete the pattern space (D: up to the first newline, and
//              run again on the rest without reading)
//   p P        print it (P: up to the first newline)
//   n N        next line (N: appended to the pattern space after a newline)
//   g G h H x  copy, append or swap with the hold space
//   a TEXT   i TEXT   c TEXT    append, insert, or change (replace) lines
//   r FILE   w FILE   read a file after the line / write the line to it
//   =  l  z    print the line number / the line unambiguously / empty it
//   b L  t L  T L  :L    branch (always / after a successful s / after none)
//   q [N]  Q [N]   quit (q prints the pattern space first); exit status N
//   #          a comment
// A last line without a newline is printed without one.
#include "ulib.h"
#include "lineio.h"
#include "regex.h"

static int usage(void) {
    dprintf(STDERR_FILENO, "usage: sed [-nsEr] [-i[SUFFIX]] [-e SCRIPT] [-f FILE] [SCRIPT] [FILE...]\n");
    return 1;
}

#define MAX_CMDS 4096
#define MAX_REGEXES 256
#define MAX_FILES_W 8
#define NO_REG (-1)

// ---------------------------------------------------------------- script
enum { A_NONE, A_LINE, A_LAST, A_RE, A_STEP, A_ZERO, A_PLUS, A_MULT };

typedef struct {
    int type;
    uint32_t n, step;  // A_LINE: n; A_STEP: n~step; A_PLUS/A_MULT: n
    int re;            // A_RE: the regex, or NO_REG for "the last one used"
} addr_t;

enum { P_LIT, P_GROUP, P_CASE };

typedef struct {
    uint8_t kind;  // P_LIT: text at off, len; P_GROUP: n; P_CASE: c is one of L U l u E
    uint8_t n;
    uint32_t off, len;
} part_t;

typedef struct {
    addr_t a1, a2;
    int negate;
    char op;
    // range state
    int active;
    uint32_t end_line;  // when the range ends at a line number
    // arguments
    int re;                 // s
    part_t* parts;          // s: the replacement
    uint32_t nparts;
    char* pool;             // s: literal text of the replacement
    int global, print, nth;
    int wfile;              // s w / w: index in wfiles, or -1
    char* text;             // a i c r: the text; : b t T: the label; w: the file name
    uint32_t text_len;
    uint8_t* ymap;          // y: 256 bytes
    uint32_t target;        // { b t T: where to go
    int code;               // q Q: exit status
} cmd_t;

static cmd_t* cmds;
static uint32_t ncmds;
static re_t* regexes[MAX_REGEXES];
static int nregexes;
static unsigned re_base;  // RE_ERE or 0
static int last_re = NO_REG;

static int n_flag, separate, in_place, status;
static int line_len = 70;  // l wraps here (-l N, 0 for never)
static const char* suffix;

typedef struct {
    int fd;
    const char* name;
    lio_out_t o;
} wfile_t;
static wfile_t wfiles[MAX_FILES_W];
static int nwfiles;

// ---------------------------------------------------------------- parsing
static const char* script;
static uint32_t script_len, sp;

static void die(const char* msg) {
    dprintf(STDERR_FILENO, "sed: -e expression #1, char %u: %s\n", sp, msg);
    sys_exit(1);
}

static void oom(void) {
    dprintf(STDERR_FILENO, "sed: out of memory\n");
    sys_exit(1);
}

static int peekc(void) { return sp < script_len ? (unsigned char)script[sp] : -1; }
static int getc_(void) { return sp < script_len ? (unsigned char)script[sp++] : -1; }

static void skip_blanks(void) {
    while (sp < script_len && (script[sp] == ' ' || script[sp] == '\t')) sp++;
}

static void skip_space_nl(void) {
    while (sp < script_len && (script[sp] == ' ' || script[sp] == '\t' || script[sp] == '\n' || script[sp] == ';')) sp++;
}

static int digit(int c) { return c >= '0' && c <= '9'; }

static uint32_t number(void) {
    uint64_t v = 0;
    while (digit(peekc())) {
        v = v * 10 + (uint64_t)(getc_() - '0');
        if (v > 0x7FFFFFFF) die("number too large");
    }
    return (uint32_t)v;
}

// The text up to an unescaped `delim`, with sp past the delimiter. For a
// regex (is_re), \n is a newline and \DELIM is DELIM; for a replacement
// the backslashes stay, to be read later. Returns a malloc'd NUL-ended copy.
static char* read_delimited(int delim, int is_re, uint32_t* len_out, const char* what) {
    uint32_t cap = 64, n = 0;
    char* b = malloc(cap);
    if (!b) oom();
    for (;;) {
        int c = getc_();
        if (c < 0) die(what);
        if (c == delim) break;
        if (c == '\n' && is_re) die(what);
        if (c == '\\') {
            int d = getc_();
            if (d < 0) die(what);
            if (d == delim && delim != '\n') {
                c = d;                  // \DELIM is just DELIM
                if (!is_re) {           // in a replacement keep it from meaning something
                    if (n + 3 >= cap) { cap *= 2; b = realloc(b, cap); if (!b) oom(); }
                    b[n++] = '\\';
                }
            } else if (d == 'n' && is_re) {
                c = '\n';
            } else {
                if (n + 3 >= cap) { cap *= 2; b = realloc(b, cap); if (!b) oom(); }
                b[n++] = '\\';
                c = d;
            }
        }
        if (n + 3 >= cap) { cap *= 2; b = realloc(b, cap); if (!b) oom(); }
        b[n++] = (char)c;
    }
    b[n] = 0;
    *len_out = n;
    return b;
}

// \t \f \v \a \r in a regex become the character (inside [] too).
static uint32_t regex_escapes(char* s, uint32_t n) {
    uint32_t o = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (s[i] == '\\' && i + 1 < n) {
            char c = s[i + 1];
            char r = c == 't' ? '\t' : c == 'f' ? '\f' : c == 'v' ? '\v' : c == 'a' ? '\a' : c == 'r' ? '\r' : 0;
            if (r) {
                s[o++] = r;
                i++;
                continue;
            }
            s[o++] = s[i++];
            s[o++] = s[i];
            continue;
        }
        s[o++] = s[i];
    }
    s[o] = 0;
    return o;
}

// Compiles pat. Returns its index in the table.
static int add_regex(char* pat, uint32_t len, unsigned flags) {
    if (nregexes == MAX_REGEXES) die("too many regular expressions");
    len = regex_escapes(pat, len);
    re_t* re = malloc(sizeof *re);
    if (!re) oom();
    const char* err = re_compile(re, pat, len, re_base | flags | RE_GROUPS | RE_ONEPAT);
    if (err) {
        char m[96];
        snprintf(m, sizeof m, "%s", err);
        die(m);
    }
    regexes[nregexes] = re;
    return nregexes++;
}

static int parse_regex_after_delim(int delim, uint32_t* ngroups) {
    uint32_t len;
    char* pat = read_delimited(delim, 1, &len, delim == '/' ? "unterminated address regex" : "unterminated address regex");
    unsigned flags = 0;
    // flags I (and M, which we take as nothing) follow an address regex
    (void)ngroups;
    if (!len) {  // the empty regex: the last one used (flags after it are allowed but ignored)
        free(pat);
        return NO_REG;
    }
    for (;;) {
        int c = peekc();
        if (c == 'I') { flags |= RE_ICASE; sp++; }
        else if (c == 'M') { flags |= RE_MULTI; sp++; }
        else break;
    }
    int r = add_regex(pat, len, flags);
    free(pat);
    return r;
}

// An address at sp, if there is one. 1 if found.
static int parse_addr(addr_t* a, int second) {
    int c = peekc();
    memset(a, 0, sizeof *a);
    if (digit(c)) {
        uint32_t n = number();
        if (peekc() == '~' && !second) {
            sp++;
            a->type = A_STEP;
            a->n = n;
            a->step = digit(peekc()) ? number() : 0;
        } else if (n == 0 && !second) {
            a->type = A_ZERO;
        } else {
            a->type = A_LINE;
            a->n = n;
        }
        return 1;
    }
    if (c == '$') {
        sp++;
        a->type = A_LAST;
        return 1;
    }
    if (second && (c == '+' || c == '~')) {
        sp++;
        if (!digit(peekc())) die("expected a number after + or ~");
        a->type = c == '+' ? A_PLUS : A_MULT;
        a->n = number();
        return 1;
    }
    if (c == '/' || c == '\\') {
        sp++;
        int delim = '/';
        if (c == '\\') {
            delim = getc_();
            if (delim < 0 || delim == '\n' || delim == '\\') die("unexpected `,'");
        }
        a->type = A_RE;
        a->re = parse_regex_after_delim(delim, 0);
        return 1;
    }
    return 0;
}

// Text for a, i, c, after the command letter: `a\` NEWLINE text (kept as
// written), `a\text` (blanks after the backslash are kept) and, as GNU
// allows, `a text` (leading blanks dropped). A backslash at the end of a
// line continues the text; \t and \n and the like are the characters they
// name, and any other \X is X. The text ends at an unescaped newline.
static void parse_text(cmd_t* c) {
    skip_blanks();
    if (peekc() == '\\') {
        sp++;
        uint32_t k = sp;
        while (k < script_len && (script[k] == ' ' || script[k] == '\t')) k++;
        if (k < script_len && script[k] == '\n') sp = k + 1;  // a\ NEWLINE: the text starts on the next line
    } else if (peekc() < 0 || peekc() == '\n') {
        die("expected \\ after `a', `c' or `i'");
    }
    uint32_t cap = 64, n = 0;
    char* b = malloc(cap);
    if (!b) oom();
    for (;;) {
        int ch = getc_();
        if (ch < 0 || ch == '\n') break;
        if (ch == '\\') {
            int d = getc_();
            if (d < 0) break;
            switch (d) {
            case 'a': ch = '\a'; break;
            case 'f': ch = '\f'; break;
            case 'n': ch = '\n'; break;
            case 'r': ch = '\r'; break;
            case 't': ch = '\t'; break;
            case 'v': ch = '\v'; break;
            default: ch = d;  // \X is X; \NEWLINE is a newline in the text
            }
        }
        if (n + 2 >= cap) { cap *= 2; b = realloc(b, cap); if (!b) oom(); }
        b[n++] = (char)ch;
    }
    b[n] = 0;
    c->text = b;
    c->text_len = n;
}

static char* read_word(uint32_t* len, int stop_at_brace) {
    skip_blanks();
    uint32_t cap = 32, n = 0;
    char* b = malloc(cap);
    if (!b) oom();
    for (;;) {
        int ch = peekc();
        if (ch < 0 || ch == '\n' || ch == ';' || ch == ' ' || ch == '\t' || (stop_at_brace && ch == '}')) break;
        if (n + 2 >= cap) { cap *= 2; b = realloc(b, cap); if (!b) oom(); }
        b[n++] = (char)getc_();
    }
    b[n] = 0;
    *len = n;
    return b;
}

static char* read_filename(uint32_t* len) {
    skip_blanks();
    uint32_t cap = 32, n = 0;
    char* b = malloc(cap);
    if (!b) oom();
    for (;;) {
        int ch = peekc();
        if (ch < 0 || ch == '\n') break;
        if (n + 2 >= cap) { cap *= 2; b = realloc(b, cap); if (!b) oom(); }
        b[n++] = (char)getc_();
    }
    b[n] = 0;
    *len = n;
    if (!n) die("missing filename in r/R/w/W commands");
    return b;
}

static int open_wfile(const char* name) {
    for (int i = 0; i < nwfiles; i++)
        if (!strcmp(wfiles[i].name, name)) return i;
    if (nwfiles == MAX_FILES_W) die("too many w files");
    wfile_t* w = &wfiles[nwfiles];
    if (!strcmp(name, "/dev/stdout")) w->fd = STDOUT_FILENO;
    else if (!strcmp(name, "/dev/stderr")) w->fd = STDERR_FILENO;
    else {
        w->fd = sys_open(name, O_WRONLY | O_CREAT | O_TRUNC);
        if (w->fd < 0) {
            dprintf(STDERR_FILENO, "sed: couldn't open file %s: %s\n", name, strerror(w->fd));
            sys_exit(4);
        }
    }
    w->name = name;
    lio_out_init(&w->o, w->fd, "sed");
    return nwfiles++;
}

static void parse_replacement(cmd_t* c, int delim) {
    uint32_t len;
    char* raw = read_delimited(delim, 0, &len, "unterminated `s' command");
    c->pool = malloc(len + 1);
    c->parts = malloc(sizeof(part_t) * (len + 1));
    if (!c->pool || !c->parts) oom();
    uint32_t pool_n = 0;
    c->nparts = 0;
    uint32_t lit_start = 0;
    int in_lit = 0;
#define LIT(ch) do { if (!in_lit) { lit_start = pool_n; in_lit = 1; } c->pool[pool_n++] = (char)(ch); } while (0)
#define FLUSH() do { if (in_lit) { part_t* p_ = &c->parts[c->nparts++]; p_->kind = P_LIT; p_->off = lit_start; p_->len = pool_n - lit_start; in_lit = 0; } } while (0)
    for (uint32_t i = 0; i < len; i++) {
        char ch = raw[i];
        if (ch == '&') {
            FLUSH();
            c->parts[c->nparts++] = (part_t){ P_GROUP, 0, 0, 0 };
        } else if (ch == '\\' && i + 1 < len) {
            char d = raw[++i];
            if (digit(d)) {
                FLUSH();
                c->parts[c->nparts++] = (part_t){ P_GROUP, (uint8_t)(d - '0'), 0, 0 };
            } else if (d == 'L' || d == 'U' || d == 'l' || d == 'u' || d == 'E') {
                FLUSH();
                c->parts[c->nparts++] = (part_t){ P_CASE, (uint8_t)d, 0, 0 };
            } else {
                LIT(d == 'n' ? '\n' : d == 't' ? '\t' : d == 'f' ? '\f' : d == 'v' ? '\v' : d == 'a' ? '\a' : d == 'r' ? '\r' : d);
            }
        } else {
            LIT(ch);
        }
    }
    FLUSH();
#undef LIT
#undef FLUSH
    free(raw);
}

static void parse_s(cmd_t* c) {
    int delim = getc_();
    if (delim < 0 || delim == '\n' || delim == '\\') die("unterminated `s' command");
    uint32_t len;
    char* pat = read_delimited(delim, 1, &len, "unterminated `s' command");
    parse_replacement(c, delim);
    // flags
    unsigned reflags = 0;
    c->wfile = -1;
    c->nth = 0;
    for (;;) {
        int f = peekc();
        if (f == 'g') { if (c->global) die("multiple `g' options to `s' command"); c->global = 1; sp++; }
        else if (f == 'p') { if (c->print) die("multiple `p' options to `s' command"); c->print = 1; sp++; }
        else if (f == 'i' || f == 'I') { reflags |= RE_ICASE; sp++; }
        else if (f == 'm' || f == 'M') { reflags |= RE_MULTI; sp++; }
        else if (digit(f)) {
            if (c->nth) die("multiple number options to `s' command");
            c->nth = (int)number();
            if (!c->nth) die("number option to `s' command may not be zero");
        } else if (f == 'w') {
            sp++;
            uint32_t l;
            char* name = read_filename(&l);
            c->wfile = open_wfile(name);
            break;
        } else break;
    }
    if (!len) {
        free(pat);
        c->re = NO_REG;
    } else {
        c->re = add_regex(pat, len, reflags);
        free(pat);
        uint32_t groups = regexes[c->re]->ngroups;
        for (uint32_t i = 0; i < c->nparts; i++)
            if (c->parts[i].kind == P_GROUP && c->parts[i].n > groups) {
                char m[64];
                snprintf(m, sizeof m, "invalid reference \\%u on `s' command's RHS", c->parts[i].n);
                die(m);
            }
    }
}

static void parse_y(cmd_t* c) {
    int delim = getc_();
    if (delim < 0 || delim == '\n' || delim == '\\') die("unterminated `y' command");
    uint32_t n1, n2;
    char* a = read_delimited(delim, 0, &n1, "unterminated `y' command");
    char* b = read_delimited(delim, 0, &n2, "unterminated `y' command");
    // unescape: \\ \n \t and \DELIM (already plain)
    char* out[2] = { a, b };
    uint32_t lens[2] = { n1, n2 };
    for (int k = 0; k < 2; k++) {
        uint32_t o = 0;
        char* s = out[k];
        for (uint32_t i = 0; i < lens[k]; i++) {
            if (s[i] == '\\' && i + 1 < lens[k]) {
                char d = s[++i];
                s[o++] = d == 'n' ? '\n' : d == 't' ? '\t' : d == '\\' ? '\\' : d;
            } else {
                s[o++] = s[i];
            }
        }
        lens[k] = o;
    }
    if (lens[0] != lens[1]) die("strings for `y' command are different lengths");
    c->ymap = malloc(256);
    if (!c->ymap) oom();
    for (int i = 0; i < 256; i++) c->ymap[i] = (uint8_t)i;
    for (uint32_t i = 0; i < lens[0]; i++) c->ymap[(uint8_t)a[i]] = (uint8_t)b[i];
    free(a);
    free(b);
}

static void end_of_command(void) {
    skip_blanks();
    int c = peekc();
    if (c < 0 || c == '\n' || c == ';') {
        if (c >= 0) sp++;
        return;
    }
    if (c == '}' || c == '#') return;
    die("extra characters after command");
}

static void parse_script(void) {
    uint32_t stack[256];
    int depth = 0;
    cmds = calloc(MAX_CMDS, sizeof *cmds);
    if (!cmds) oom();
    for (;;) {
        skip_space_nl();
        if (peekc() < 0) break;
        if (peekc() == '#') {
            while (peekc() >= 0 && peekc() != '\n') sp++;
            continue;
        }
        if (ncmds == MAX_CMDS) die("script too long");
        cmd_t* c = &cmds[ncmds];
        c->wfile = -1;
        c->re = NO_REG;
        if (parse_addr(&c->a1, 0)) {
            skip_blanks();
            if (peekc() == ',') {
                sp++;
                skip_blanks();
                if (!parse_addr(&c->a2, 1)) die("unexpected `,'");
            }
            if (c->a1.type == A_ZERO && c->a2.type != A_RE) die("invalid usage of line address 0");
        }
        skip_blanks();
        while (peekc() == '!') {
            c->negate = 1;
            sp++;
            skip_blanks();
        }
        int op = getc_();
        if (op < 0) die("missing command");
        c->op = (char)op;
        switch (op) {
        case '{':
            if (depth == 256) die("blocks nested too deeply");
            stack[depth++] = ncmds;
            break;
        case '}':
            if (!depth) die("unexpected `}'");
            if (c->a1.type != A_NONE) die("} doesn't want any addresses");
            cmds[stack[--depth]].target = ncmds + 1;
            end_of_command();
            break;
        case 's': parse_s(c); end_of_command(); break;
        case 'y': parse_y(c); end_of_command(); break;
        case 'a': case 'i': case 'c': parse_text(c); break;
        case 'r': {
            uint32_t l;
            c->text = read_filename(&l);
            c->text_len = l;
            break;
        }
        case 'w': {
            uint32_t l;
            c->text = read_filename(&l);
            c->wfile = open_wfile(c->text);
            break;
        }
        case ':': {
            if (c->a1.type != A_NONE) die(": doesn't want any addresses");
            c->text = read_word(&c->text_len, 0);
            if (!c->text_len) die("\":\" lacks a label");
            end_of_command();
            break;
        }
        case 'b': case 't': case 'T':
            c->text = read_word(&c->text_len, 1);
            end_of_command();
            break;
        case 'q': case 'Q':
            if (c->a2.type != A_NONE) die("command only uses one address");
            skip_blanks();
            c->code = digit(peekc()) ? (int)number() : 0;
            end_of_command();
            break;
        case 'l':
            skip_blanks();
            c->code = digit(peekc()) ? (int)number() : -1;
            end_of_command();
            break;
        case '=': case 'd': case 'D': case 'g': case 'G': case 'h': case 'H': case 'n': case 'N':
        case 'p': case 'P': case 'x': case 'z':
            end_of_command();
            break;
        default: {
            char m[48];
            snprintf(m, sizeof m, "unknown command: `%c'", op);
            die(m);
        }
        }
        ncmds++;
    }
    if (depth) die("unmatched `{'");
    // resolve the labels
    for (uint32_t i = 0; i < ncmds; i++) {
        cmd_t* c = &cmds[i];
        if (c->op != 'b' && c->op != 't' && c->op != 'T') continue;
        if (!c->text_len) {
            c->target = ncmds;  // the end of the script
            continue;
        }
        uint32_t k;
        for (k = 0; k < ncmds; k++)
            if (cmds[k].op == ':' && !strcmp(cmds[k].text, c->text)) break;
        if (k == ncmds) {
            dprintf(STDERR_FILENO, "sed: can't find label for jump to `%s'\n", c->text);
            sys_exit(4);  // GNU treats this one as a panic, not a usage error
        }
        c->target = k;
    }
}

// ---------------------------------------------------------------- input
static char** files;
static int nfiles, file_i;
static int cur_fd = -1;
static lio_in_t in;
static lio_out_t out;
static int input_done;  // nothing more to read from the current file (or any, when not separate)
static const char* cur_name;
static int out_fd = STDOUT_FILENO;
static int missing_nl;

static lio_line_t ps, hold;
static uint32_t lineno;


// Starts reading file i (or standard input for "-"). 0, or -1 after a message.
static int open_input(const char* name) {
    if (cur_fd > STDERR_FILENO) sys_close(cur_fd);
    cur_fd = -1;
    cur_name = name;
    if (!strcmp(name, "-")) {
        cur_fd = STDIN_FILENO;
    } else {
        cur_fd = sys_open(name, O_RDONLY);
        if (cur_fd < 0) {
            lio_flush(&out);
            dprintf(STDERR_FILENO, "sed: can't read %s: %s\n", name, strerror(cur_fd));
            cur_fd = -1;
            status = 2;
            return -1;
        }
        dirent_t d;
        if (sys_stat(name, &d) >= 0 && (d.flags & DIRENT_DIR)) {
            lio_flush(&out);
            dprintf(STDERR_FILENO, "sed: couldn't edit %s: not a regular file\n", name);
            sys_close(cur_fd);
            cur_fd = -1;
            status = 4;
            return -1;
        }
    }
    lio_in_init(&in, cur_fd);
    input_done = 0;
    return 0;
}

// Moves to the next file that opens (not when files are separate).
static int advance_file(void) {
    while (file_i < nfiles) {
        if (open_input(files[file_i++]) == 0) return 1;
    }
    return 0;
}

// The next input byte without taking it, or -1 at the end of all input
// (of this file, when separate).
static int peek_in(void) {
    for (;;) {
        if (in.pos < in.len) return (unsigned char)in.buf[in.pos];
        if (cur_fd < 0 || input_done) {
            if (separate || !advance_file()) return -1;
            continue;
        }
        int c = lio_getc(&in, &out);
        if (c >= 0) {
            in.pos--;
            return c;
        }
        if (in.err) {
            lio_flush(&out);
            dprintf(STDERR_FILENO, "sed: read error on %s: %s\n", cur_name, strerror(in.err));
            status = 2;
        }
        input_done = 1;
    }
}

static int is_last(void) { return peek_in() < 0; }

static void ensure(lio_line_t* l) {
    if (!l->s) {
        lio_line_addc(l, 0, "sed");
        l->n = 0;
    }
}

static int read_line_into(lio_line_t* l) {
    if (peek_in() < 0) return 0;
    lio_line_t tmp = *l;
    int r = lio_getline(&in, &out, &tmp);
    *l = tmp;
    ensure(l);
    return r;
}

// ---------------------------------------------------------------- output
static void emit_text(const char* p, uint32_t n, int nl, lio_out_t* o, int* missing) {
    if (*missing) {
        lio_putc(o, '\n');
        *missing = 0;
    }
    lio_put(o, p, n);
    if (nl) lio_putc(o, '\n');
    else *missing = 1;
}

static void emit(const char* p, uint32_t n, int nl) { emit_text(p, n, nl, &out, &missing_nl); }

static int wmissing[MAX_FILES_W];
static void emit_w(int w, const char* p, uint32_t n, int nl) {
    if (wfiles[w].fd == STDOUT_FILENO) {
        emit(p, n, nl);
        return;
    }
    emit_text(p, n, nl, &wfiles[w].o, &wmissing[w]);
    lio_flush(&wfiles[w].o);
}

typedef struct {
    int is_file;
    const char* text;  // text or file name
    uint32_t len;
} queued_t;

static queued_t queue[64];
static int nqueue;

static void flush_queue(void) {
    for (int i = 0; i < nqueue; i++) {
        if (!queue[i].is_file) {
            emit(queue[i].text, queue[i].len, 1);
        } else {
            int fd = sys_open(queue[i].text, O_RDONLY);
            if (fd < 0) continue;  // a missing file is silently nothing
            char buf[512];
            int n;
            lio_flush(&out);
            while ((n = sys_read(fd, buf, sizeof buf)) > 0) {
                if (missing_nl) {
                    lio_putc(&out, '\n');
                    missing_nl = 0;
                }
                lio_put(&out, buf, (uint32_t)n);
                missing_nl = buf[n - 1] != '\n';
            }
            sys_close(fd);
        }
    }
    nqueue = 0;
}

static void enqueue(int is_file, const char* text, uint32_t len) {
    if (nqueue == 64) flush_queue();
    queue[nqueue++] = (queued_t){ is_file, text, len };
}

// ---------------------------------------------------------------- matching
static re_t* resolve(int idx) {
    if (idx == NO_REG) idx = last_re;
    if (idx == NO_REG) {
        lio_flush(&out);
        dprintf(STDERR_FILENO, "sed: no previous regular expression\n");
        sys_exit(1);
    }
    last_re = idx;
    return regexes[idx];
}

static int match_addr(const addr_t* a) {
    switch (a->type) {
    case A_LINE: return lineno == a->n;
    case A_LAST: return is_last();
    case A_RE: return re_test(resolve(a->re), ps.s, ps.n);
    case A_STEP:
        if (a->step == 0) return lineno == a->n;
        return lineno >= a->n && (lineno - a->n) % a->step == 0;
    }
    return 0;
}

// Ranges start out inactive, except 0,/re/ which is already running.
static void reset_ranges(void) {
    for (uint32_t i = 0; i < ncmds; i++) cmds[i].active = cmds[i].a1.type == A_ZERO;
}

static int range_match(cmd_t* c) {
    if (c->active) {
        switch (c->a2.type) {
        case A_LINE:
        case A_PLUS:
        case A_MULT:
            if (lineno > c->end_line) {  // the end went by while lines were being skipped
                c->active = 0;
                break;
            }
            if (lineno == c->end_line) c->active = 0;
            return 1;
        default:
            if (match_addr(&c->a2)) c->active = 0;
            return 1;
        }
    }
    // not in a range: does one start here?
    if (c->a1.type == A_ZERO) return 0;  // 0,/re/ can only run from the start
    if (!match_addr(&c->a1)) return 0;
    switch (c->a2.type) {
    case A_LINE:
        if (c->a2.n <= lineno) return 1;  // the end is not after the start: one line
        c->end_line = c->a2.n;
        break;
    case A_PLUS:
        if (c->a2.n == 0) return 1;
        c->end_line = lineno + c->a2.n;
        break;
    case A_MULT:
        if (c->a2.n == 0) return 1;
        c->end_line = (lineno / c->a2.n + 1) * c->a2.n;  // the next multiple, even from a multiple (as GNU)
        break;
    case A_LAST:
        if (is_last()) return 1;
        break;
    }
    c->active = 1;
    return 1;
}

static int selected(cmd_t* c) {
    int m;
    if (c->a1.type == A_NONE) m = 1;
    else if (c->a2.type == A_NONE) m = match_addr(&c->a1);
    else m = range_match(c);
    return m != c->negate;
}

// ---------------------------------------------------------------- s
static void add_conv(lio_line_t* res, const char* p, uint32_t n, int* mode, int* once) {
    // mode: 0 none, 'L', 'U'. once: 0, 'l' or 'u' for the next character only
    for (uint32_t i = 0; i < n; i++) {
        char ch = p[i];
        int m = *once ? *once : *mode;
        if (m == 'L' || m == 'l') {
            if (ch >= 'A' && ch <= 'Z') ch = (char)(ch + 32);
        } else if (m == 'U' || m == 'u') {
            if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
        }
        *once = 0;
        lio_line_addc(res, ch, "sed");
    }
}

// Returns 1 if a substitution was made.
static int do_subst(cmd_t* c) {
    re_t* re = resolve(c->re);
    int32_t caps[2 * 32];
    if (re->ngroups > 31) return 0;
    lio_line_t res = { 0, 0, 0, 0 };
    uint32_t copied = 0, pos = 0;
    int64_t prev_end = -1;
    int count = 0, did = 0;
    int want = c->nth ? c->nth : 1;
    while (pos <= ps.n) {
        int r = re_exec(re, ps.s, ps.n, pos, caps);
        if (r < 0) oom();
        if (!r) break;
        uint32_t ms = (uint32_t)caps[0], me = (uint32_t)caps[1];
        if (ms == me && (int64_t)ms == prev_end) {  // an empty match right after one: not allowed
            if (ms >= ps.n) break;
            pos = ms + 1;
            continue;
        }
        count++;
        if (count >= want) {
            lio_line_add(&res, ps.s + copied, ms - copied, "sed");
            int mode = 0, once = 0;
            for (uint32_t i = 0; i < c->nparts; i++) {
                const part_t* p = &c->parts[i];
                if (p->kind == P_LIT) {
                    add_conv(&res, c->pool + p->off, p->len, &mode, &once);
                } else if (p->kind == P_GROUP) {
                    int32_t gs = caps[2 * p->n], ge = caps[2 * p->n + 1];
                    if (gs >= 0 && ge >= gs) add_conv(&res, ps.s + gs, (uint32_t)(ge - gs), &mode, &once);
                } else if (p->n == 'L' || p->n == 'U') {
                    mode = p->n;
                    once = 0;
                } else if (p->n == 'E') {
                    mode = 0;
                    once = 0;
                } else {
                    once = p->n;
                }
            }
            copied = me;
            did = 1;
            if (!c->global) break;
        }
        prev_end = me;
        if (me == ms) {
            if (ms >= ps.n) break;
            pos = ms + 1;
        } else {
            pos = me;
        }
    }
    if (!did) {
        free(res.s);
        return 0;
    }
    lio_line_add(&res, ps.s + copied, ps.n - copied, "sed");
    res.had_nl = ps.had_nl;
    free(ps.s);
    ps = res;
    ensure(&ps);
    return 1;
}

// ---------------------------------------------------------------- l
static void do_l(int width) {
    char line[128];
    uint32_t col = 0;
    lio_line_t o = { 0, 0, 0, 0 };
    for (uint32_t i = 0; i < ps.n; i++) {
        uint8_t ch = (uint8_t)ps.s[i];
        char t[8];
        int n;
        switch (ch) {
        case '\\': n = snprintf(t, sizeof t, "\\\\"); break;
        case '\a': n = snprintf(t, sizeof t, "\\a"); break;
        case '\b': n = snprintf(t, sizeof t, "\\b"); break;
        case '\f': n = snprintf(t, sizeof t, "\\f"); break;
        case '\n': n = snprintf(t, sizeof t, "\\n"); break;
        case '\r': n = snprintf(t, sizeof t, "\\r"); break;
        case '\t': n = snprintf(t, sizeof t, "\\t"); break;
        case '\v': n = snprintf(t, sizeof t, "\\v"); break;
        default:
            if (ch >= 32 && ch < 127) {
                t[0] = (char)ch;
                n = 1;
            } else {
                t[0] = '\\';
                t[1] = (char)('0' + (ch >> 6));
                t[2] = (char)('0' + ((ch >> 3) & 7));
                t[3] = (char)('0' + (ch & 7));
                n = 4;
            }
        }
        if (width > 0 && col + (uint32_t)n > (uint32_t)width - 1) {  // a line is `width` wide with its closing backslash
            lio_line_add(&o, "\\\n", 2, "sed");
            col = 0;
        }
        lio_line_add(&o, t, (uint32_t)n, "sed");
        col += (uint32_t)n;
    }
    (void)line;
    lio_line_add(&o, "$", 1, "sed");
    emit(o.s, o.n, 1);
    free(o.s);
}

// ---------------------------------------------------------------- run
enum { END_CYCLE, END_NO_PRINT, RESTART, QUIT_PRINT, QUIT };
static int quit_code;
static int replaced;  // for t: a substitution since the last line was read or t taken

static void print_ps(void) { emit(ps.s, ps.n, ps.had_nl); }

static int run_script(void) {
    uint32_t pc = 0;
    while (pc < ncmds) {
        cmd_t* c = &cmds[pc];
        if (c->op == '}' || c->op == ':') {
            pc++;
            continue;
        }
        if (!selected(c)) {
            pc = c->op == '{' ? c->target : pc + 1;
            continue;
        }
        switch (c->op) {
        case '{': break;
        case 's':
            if (do_subst(c)) {
                replaced = 1;
                if (c->print) print_ps();
                if (c->wfile >= 0) emit_w(c->wfile, ps.s, ps.n, ps.had_nl);
            }
            break;
        case 'y':
            for (uint32_t i = 0; i < ps.n; i++) ps.s[i] = (char)c->ymap[(uint8_t)ps.s[i]];
            break;
        case 'd': return END_NO_PRINT;
        case 'D': {
            uint32_t i = 0;
            while (i < ps.n && ps.s[i] != '\n') i++;
            if (i == ps.n) return END_NO_PRINT;
            memmove(ps.s, ps.s + i + 1, ps.n - i - 1);
            ps.n -= i + 1;
            return RESTART;
        }
        case 'p': print_ps(); break;
        case 'P': {
            uint32_t i = 0;
            while (i < ps.n && ps.s[i] != '\n') i++;
            emit(ps.s, i, 1);
            break;
        }
        case 'n':
            if (is_last()) return END_CYCLE;  // GNU: print and quit
            if (!n_flag) print_ps();
            flush_queue();
            if (!read_line_into(&ps)) return END_NO_PRINT;
            lineno++;
            replaced = 0;
            break;
        case 'N': {
            if (is_last()) return END_CYCLE;
            flush_queue();
            lio_line_t next = { 0, 0, 0, 0 };
            if (!read_line_into(&next)) return END_CYCLE;
            lineno++;
            replaced = 0;
            lio_line_addc(&ps, '\n', "sed");
            lio_line_add(&ps, next.s, next.n, "sed");
            ps.had_nl = next.had_nl;
            free(next.s);
            break;
        }
        case 'g': ps.n = 0; lio_line_add(&ps, hold.s, hold.n, "sed"); ps.had_nl = hold.had_nl; ensure(&ps); break;
        case 'G': lio_line_addc(&ps, '\n', "sed"); lio_line_add(&ps, hold.s, hold.n, "sed"); break;
        case 'h': hold.n = 0; lio_line_add(&hold, ps.s, ps.n, "sed"); hold.had_nl = ps.had_nl; ensure(&hold); break;
        case 'H': lio_line_addc(&hold, '\n', "sed"); lio_line_add(&hold, ps.s, ps.n, "sed"); break;
        case 'x': {
            lio_line_t t = ps;
            ps = hold;
            hold = t;
            break;
        }
        case 'z': ps.n = 0; break;
        case '=': {
            char b[16];
            int n = snprintf(b, sizeof b, "%u", lineno);
            emit(b, (uint32_t)n, 1);
            break;
        }
        case 'l': do_l(c->code >= 0 ? c->code : line_len); break;
        case 'a': enqueue(0, c->text, c->text_len); break;
        case 'i': emit(c->text, c->text_len, 1); break;
        case 'c':
            if (!c->active || c->a2.type == A_NONE) emit(c->text, c->text_len, 1);
            return END_NO_PRINT;
        case 'r': enqueue(1, c->text, c->text_len); break;
        case 'w': emit_w(c->wfile, ps.s, ps.n, ps.had_nl); break;
        case 'b': pc = c->target; continue;
        case 't':
            if (replaced) {
                replaced = 0;
                pc = c->target;
                continue;
            }
            break;
        case 'T':
            if (!replaced) {
                pc = c->target;
                continue;
            }
            replaced = 0;
            break;
        case 'q':
            quit_code = c->code;
            return QUIT_PRINT;
        case 'Q':
            quit_code = c->code;
            return QUIT;
        }
        pc++;
    }
    return END_CYCLE;
}

// Runs the script over the input that is open. Returns 1 if a q or Q ended it.
static int process(void) {
    int skip_read = 0;
    reset_ranges();
    for (;;) {
        if (!skip_read) {
            if (!read_line_into(&ps)) return 0;
            lineno++;
            replaced = 0;
        }
        skip_read = 0;
        int r = run_script();
        switch (r) {
        case END_CYCLE:
            if (!n_flag) print_ps();
            flush_queue();
            break;
        case END_NO_PRINT:
            flush_queue();
            break;
        case RESTART:
            flush_queue();
            skip_read = 1;
            break;
        case QUIT_PRINT:
            if (!n_flag) print_ps();
            flush_queue();
            return 1;
        case QUIT:
            nqueue = 0;  // Q says nothing more, not even what a and r queued
            return 1;
        }
    }
}

static int quit_seen;

static void edit_in_place(const char* name) {
    dirent_t d;
    if (sys_stat(name, &d) < 0) {
        dprintf(STDERR_FILENO, "sed: can't read %s: %s\n", name, strerror(sys_stat(name, &d)));
        status = 2;
        return;
    }
    char tmp[PATH_MAX + 8];
    if (strlen(name) + 8 >= sizeof tmp) {
        dprintf(STDERR_FILENO, "sed: %s: name too long\n", name);
        status = 4;
        return;
    }
    snprintf(tmp, sizeof tmp, "%s.sedtmp", name);
    int fd = sys_open(tmp, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        dprintf(STDERR_FILENO, "sed: couldn't open temporary file %s: %s\n", tmp, strerror(fd));
        status = 4;
        return;
    }
    out_fd = fd;
    lio_out_init(&out, fd, "sed");
    missing_nl = 0;
    file_i = nfiles;  // only this one
    lineno = 0;
    if (open_input(name) == 0) {
        quit_seen = process();
        lio_flush(&out);
        sys_close(fd);
        if (cur_fd > STDERR_FILENO) sys_close(cur_fd);
        cur_fd = -1;
        if (suffix && *suffix) {
            char bak[PATH_MAX + 64];
            snprintf(bak, sizeof bak, "%s%s", name, suffix);
            sys_delete(bak);
            int r = sys_rename(name, bak);
            if (r < 0) {
                fs_error("sed", bak, r);
                status = 4;
                sys_delete(tmp);
                return;
            }
        } else {
            sys_delete(name);
        }
        int r = sys_rename(tmp, name);
        if (r < 0) {
            fs_error("sed", name, r);
            status = 4;
        }
    } else {
        sys_close(fd);
        sys_delete(tmp);
    }
    out_fd = STDOUT_FILENO;
}

// ---------------------------------------------------------------- main
static char* read_script_file(const char* name, uint32_t* len) {
    int fd = !strcmp(name, "-") ? STDIN_FILENO : sys_open(name, O_RDONLY);
    if (fd < 0) {
        dprintf(STDERR_FILENO, "sed: couldn't open file %s: %s\n", name, strerror(fd));
        sys_exit(1);
    }
    uint32_t cap = 256, n = 0;
    char* b = malloc(cap);
    if (!b) oom();
    int r;
    for (;;) {
        if (n + 256 > cap) {
            cap *= 2;
            b = realloc(b, cap);
            if (!b) oom();
        }
        r = sys_read(fd, b + n, cap - n);
        if (r <= 0) break;
        n += (uint32_t)r;
    }
    if (fd != STDIN_FILENO) sys_close(fd);
    while (n && b[n - 1] == '\n') n--;  // the trailing newline is not part of the script
    *len = n;
    return b;
}

static int to_len(const char* s) {
    int v = 0;
    if (!*s) v = -1;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || v > 100000) {
            dprintf(STDERR_FILENO, "sed: invalid line length\n");
            sys_exit(1);
        }
        v = v * 10 + (*s - '0');
    }
    if (v < 0) {
        dprintf(STDERR_FILENO, "sed: invalid line length\n");
        sys_exit(1);
    }
    return v;
}

int main(int argc, char** argv) {
    char* pieces[64];
    uint32_t plens[64];
    int npieces = 0, have_script = 0;
    int a = 1, only_ops = 0;
    char** ops = argv + 1;
    int nops = 0;
    for (; a < argc; a++) {
        const char* s = argv[a];
        if (only_ops || s[0] != '-' || !s[1]) {
            ops[nops++] = argv[a];
            continue;
        }
        if (!strcmp(s, "--")) {
            only_ops = 1;
            continue;
        }
        if (s[1] == '-') {
            if (!strcmp(s, "--quiet") || !strcmp(s, "--silent")) n_flag = 1;
            else if (!strcmp(s, "--regexp-extended")) re_base = RE_ERE;
            else if (!strcmp(s, "--separate")) separate = 1;
            else if (!strncmp(s, "--line-length=", 14)) line_len = to_len(s + 14);
            else if (!strncmp(s, "--expression=", 13) || !strncmp(s, "--file=", 7)) {
                if (npieces == 64) return usage();
                if (s[2] == 'e') {
                    pieces[npieces] = (char*)s + 13;
                    plens[npieces++] = (uint32_t)strlen(s + 13);
                } else {
                    pieces[npieces] = read_script_file(s + 7, &plens[npieces]);
                    npieces++;
                }
                have_script = 1;
            } else if (!strcmp(s, "--posix") || !strcmp(s, "--unbuffered")) {
            } else {
                dprintf(STDERR_FILENO, "sed: unrecognized option '%s'\n", s);
                return usage();
            }
            continue;
        }
        for (const char* o = s + 1; *o; o++) {
            if (*o == 'n') n_flag = 1;
            else if (*o == 'E' || *o == 'r') re_base = RE_ERE;
            else if (*o == 's') separate = 1;
            else if (*o == 'u') { }
            else if (*o == 'l') {
                const char* v = o + 1;
                if (!*v) {
                    if (++a == argc) return usage();
                    v = argv[a];
                }
                line_len = to_len(v);
                break;
            }
            else if (*o == 'i') {
                in_place = separate = 1;
                suffix = o + 1;
                break;
            } else if (*o == 'e' || *o == 'f') {
                const char* v = o + 1;
                if (!*v) {
                    if (++a == argc) {
                        dprintf(STDERR_FILENO, "sed: option requires an argument -- '%c'\n", *o);
                        return usage();
                    }
                    v = argv[a];
                }
                if (npieces == 64) return usage();
                if (*o == 'e') {
                    pieces[npieces] = (char*)v;
                    plens[npieces++] = (uint32_t)strlen(v);
                } else {
                    pieces[npieces] = read_script_file(v, &plens[npieces]);
                    npieces++;
                }
                have_script = 1;
                break;
            } else {
                dprintf(STDERR_FILENO, "sed: invalid option -- '%c'\n", *o);
                return usage();
            }
        }
    }
    if (!have_script) {
        if (!nops) return usage();
        pieces[0] = ops[0];
        plens[0] = (uint32_t)strlen(ops[0]);
        npieces = 1;
        ops++;
        nops--;
    }
    // join the pieces with newlines
    uint32_t total = 0;
    for (int i = 0; i < npieces; i++) total += plens[i] + 1;
    char* all = malloc(total + 1);
    if (!all) oom();
    uint32_t o = 0;
    for (int i = 0; i < npieces; i++) {
        memcpy(all + o, pieces[i], plens[i]);
        o += plens[i];
        all[o++] = '\n';
    }
    all[o] = 0;
    script = all;
    script_len = o;
    if (script_len >= 3 && !memcmp(script, "#n\n", 3)) n_flag = 1;  // "#n" on the first line means -n
    parse_script();

    static char* dash[] = { "-", 0 };
    files = nops ? ops : dash;
    nfiles = nops ? nops : 1;
    if (in_place && !nops) {
        dprintf(STDERR_FILENO, "sed: no input files\n");
        return 4;
    }
    lio_out_init(&out, STDOUT_FILENO, "sed");
    ensure(&ps);
    ensure(&hold);
    hold.had_nl = 1;

    if (in_place) {
        for (int i = 0; i < nfiles && !quit_seen; i++) edit_in_place(files[i]);
    } else if (separate) {
        for (int i = 0; i < nfiles; i++) {
            file_i = i + 1;
            lineno = 0;
            if (open_input(files[i]) < 0) continue;
            if (process()) break;
        }
    } else {
        file_i = 0;
        if (advance_file()) process();
    }
    lio_flush(&out);
    for (int i = 0; i < nwfiles; i++) lio_flush(&wfiles[i].o);
    if (quit_code) return quit_code;
    return status;
}
