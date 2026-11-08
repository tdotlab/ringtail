// grep [OPTION]... PATTERNS [FILE]...    print the lines that match
//
// Of the FILEs, or of standard input when none is named (or for "-").
// PATTERNS is a basic regular expression (-E: extended, -F: plain
// strings); a pattern with newlines in it is several patterns, and -e
// and -f give more. Matching is POSIX with the GNU \w \s \b \< \>
// extras, no back-references.
//
//   -e PAT  use PAT (repeatable)         -f FILE  patterns, one a line
//   -E -F -G  extended / fixed / basic   -i (-y)  ignore case
//   -w  whole words only                 -x  whole lines only
//   -v  the lines that do not match      -o  only the matching parts
//   -c  count the lines                  -l / -L  file names with / without
//   -m N  stop after N lines             -q  say nothing, exit at once
//   -n  line numbers                     -H / -h  with / without file names
//   -A / -B / -C N (or -N)  lines of context after / before / both
//   -r (-R)  walk directories, "." with no file   -s  hush file errors
//   -a  treat binary files as text (else "Binary file X matches")
//
// Lines may be any length. Output is flushed when grep is about to wait
// for more input, so it works at the end of a pipe. Exits 0 if a line
// was selected, 1 if none, 2 on an error.
#include "ulib.h"
#include "regex.h"

static int usage(void) {
    dprintf(STDERR_FILENO, "usage: grep [OPTION]... PATTERNS [FILE]...\n");
    return 2;
}

// ---- growable bytes ----

typedef struct { char* s; uint32_t n, cap; } buf_t;

static void oom(void) {
    dprintf(STDERR_FILENO, "grep: out of memory\n");
    sys_exit(2);
}

static void buf_room(buf_t* b, uint32_t extra) {
    if (b->n + extra <= b->cap) return;
    uint32_t want = b->cap ? b->cap : 256;
    while (want < b->n + extra) {
        if (want > 0x20000000u) oom();
        want *= 2;
    }
    char* p = realloc(b->s, want);
    if (!p) oom();
    b->s = p;
    b->cap = want;
}

static void buf_put(buf_t* b, const char* p, uint32_t n) {
    buf_room(b, n);
    memcpy(b->s + b->n, p, n);
    b->n += n;
}

// ---- output, buffered, flushed before any wait for input ----

static char ob[1024];
static uint32_t on;

static void flush(void) {
    const char* p = ob;
    while (on) {
        int w = sys_write(STDOUT_FILENO, p, on);
        if (w <= 0) {
            if (w != -EPIPE) dprintf(STDERR_FILENO, "grep: write error: %s\n", w ? strerror(w) : "no progress");
            sys_exit(2);
        }
        p += w;
        on -= (uint32_t)w;
    }
}

static void emit(const char* p, uint32_t n) {
    while (n) {
        uint32_t take = sizeof ob - on < n ? (uint32_t)sizeof ob - on : n;
        memcpy(ob + on, p, take);
        on += take;
        p += take;
        n -= take;
        if (on == sizeof ob) flush();
    }
}

static void emit_str(const char* s) { emit(s, (uint32_t)strlen(s)); }

static void emit_uint(uint32_t v) {
    char t[12];
    int n = snprintf(t, sizeof t, "%u", v);
    emit(t, (uint32_t)n);
}

// ---- options ----

static re_t re;
static int no_patterns;  // -f of an empty file: nothing matches
static int invert, only_matching, count_only, list_with, list_without, quiet;
static int with_number, show_name, name_forced, recursive, hush, as_text;
static uint32_t max_count = 0xffffffffu, ctx_after, ctx_before;

// ---- the input ----

static char* find_byte(const char* p, uint32_t n, char c) {
    for (uint32_t i = 0; i < n; i++)
        if (p[i] == c) return (char*)p + i;
    return 0;
}

static int in_fd;
static char ib[1024];
static int ilen, ipos, read_err, binary;

// The next line, without its newline. 0 at the end of the input.
static int read_line(buf_t* l) {
    l->n = 0;
    int got = 0;
    for (;;) {
        if (ipos == ilen) {
            flush();
            ilen = sys_read(in_fd, ib, sizeof ib);
            ipos = 0;
            if (ilen <= 0) {
                if (ilen < 0) read_err = ilen;
                ilen = 0;
                return got;
            }
            if (!binary && find_byte(ib, (uint32_t)ilen, 0)) binary = 1;
        }
        got = 1;
        char* nl = find_byte(ib + ipos, (uint32_t)(ilen - ipos), '\n');
        uint32_t take = nl ? (uint32_t)(nl - (ib + ipos)) : (uint32_t)(ilen - ipos);
        buf_put(l, ib + ipos, take);
        ipos += (int)take;
        if (nl) {
            ipos++;
            return 1;
        }
    }
}

// ---- the lines kept back for -B ----

typedef struct { buf_t b; uint32_t no; } slot_t;
static slot_t* ring;
static uint32_t rcap, rhead, rcount;

static void ring_push(const char* line, uint32_t len, uint32_t no) {
    if (!ctx_before) return;
    if (rcount == rcap && rcap < ctx_before) {
        uint32_t ncap = rcap ? rcap * 2 : 16;
        if (ncap > ctx_before) ncap = ctx_before;
        slot_t* n = calloc(ncap, sizeof *n);
        if (!n) oom();
        for (uint32_t i = 0; i < rcount; i++) n[i] = ring[(rhead + i) % rcap];
        free(ring);
        ring = n;
        rcap = ncap;
        rhead = 0;
    }
    slot_t* s;
    if (rcount == rcap) {  // full: the oldest goes
        s = &ring[rhead];
        rhead = (rhead + 1) % rcap;
    } else {
        s = &ring[(rhead + rcount) % rcap];
        rcount++;
    }
    s->b.n = 0;
    buf_put(&s->b, line, len);
    s->no = no;
}

// ---- one file ----

static const char* cur_name;
static int printed_any;  // anything printed yet, in any file (for "--")
static int selected_any, bad;

typedef struct {
    uint32_t lineno, count, last_printed;  // last_printed: 0 = none in this file
    uint32_t after;                        // context lines still owed
    int stop;                              // no more of this file
} file_t;

static void prefix(file_t* f, uint32_t no, char sep) {
    if (show_name) {
        emit_str(cur_name);
        emit(&sep, 1);
    }
    if (with_number) {
        emit_uint(no);
        emit(&sep, 1);
    }
    (void)f;
}

static int context_on(void) {
    return (ctx_after || ctx_before) && !only_matching && !count_only && !list_with && !list_without && !quiet;
}

static void gap_mark(file_t* f, uint32_t no) {
    if (context_on() && printed_any && (!f->last_printed || no != f->last_printed + 1)) emit_str("--\n");
}

static void print_context(file_t* f, const char* line, uint32_t len, uint32_t no) {
    gap_mark(f, no);
    prefix(f, no, '-');
    emit(line, len);
    emit("\n", 1);
    f->last_printed = no;
    printed_any = 1;
}

static void print_selected(file_t* f, const char* line, uint32_t len) {
    if (binary && !as_text) {
        emit_str("Binary file ");
        emit_str(cur_name);
        emit_str(" matches\n");
        f->stop = 1;
        return;
    }
    for (uint32_t i = 0; i < rcount; i++) {
        slot_t* s = &ring[(rhead + i) % rcap];
        print_context(f, s->b.s, s->b.n, s->no);
    }
    rcount = 0;
    if (only_matching) {
        if (invert) return;
        uint32_t from = 0, ms, me;
        while (from <= len && re_find(&re, line, len, from, &ms, &me)) {
            if (me == ms) {
                from = me + 1;
                continue;
            }
            prefix(f, f->lineno, ':');
            emit(line + ms, me - ms);
            emit("\n", 1);
            from = me;
        }
        printed_any = 1;
        return;
    }
    gap_mark(f, f->lineno);
    prefix(f, f->lineno, ':');
    emit(line, len);
    emit("\n", 1);
    f->last_printed = f->lineno;
    printed_any = 1;
    f->after = ctx_after;
}

static void do_line(file_t* f, buf_t* l) {
    f->lineno++;
    int hit = !no_patterns && re_test(&re, l->s, l->n);
    if (hit != invert && f->count < max_count) {
        f->count++;
        if (!list_without) selected_any = 1;
        if (quiet) {
            flush();
            sys_exit(0);
        }
        if (list_with || list_without) f->stop = 1;
        else if (!count_only) print_selected(f, l->s, l->n);
        if (f->count == max_count && !f->after) f->stop = 1;
    } else if (f->after) {
        // Trailing context, also after -m has run out.
        print_context(f, l->s, l->n, f->lineno);
        f->after--;
        if (f->count >= max_count && !f->after) f->stop = 1;
    } else if (f->count >= max_count) {
        f->stop = 1;
    } else if (context_on()) {
        ring_push(l->s, l->n, f->lineno);
    }
}

static void scan(int fd, const char* name) {
    static buf_t line;
    file_t f = { 0, 0, 0, 0, 0 };
    in_fd = fd;
    ilen = ipos = read_err = binary = 0;
    rcount = 0;
    cur_name = name;
    while (!f.stop && read_line(&line)) do_line(&f, &line);
    if (read_err) {
        flush();
        if (!hush || read_err != -EISDIR) dprintf(STDERR_FILENO, "grep: %s: %s\n", name, strerror(read_err));
        bad = 1;
    }
    if (count_only && !list_with && !list_without) {
        if (show_name) {
            emit_str(name);
            emit(":", 1);
        }
        emit_uint(f.count);
        emit("\n", 1);
    }
    if ((list_with && f.count) || (list_without && !f.count)) {
        emit_str(name);
        emit("\n", 1);
        if (list_without) selected_any = 1;
    }
}

// ---- walking the operands ----


static void walk(const char* path, const char* shown, int top) {
    dirent_t d;
    int r = sys_stat(path, &d);
    if (r < 0) {
        flush();
        if (!hush) dprintf(STDERR_FILENO, "grep: %s: %s\n", shown, strerror(r));
        bad = 1;
        return;
    }
    if (d.flags & DIRENT_DIR) {
        if (!recursive) {
            flush();
            if (!hush) dprintf(STDERR_FILENO, "grep: %s: %s\n", shown, strerror(-EISDIR));
            bad = 1;
            return;
        }
        if (top && !name_forced) show_name = 1;
        uint32_t n = 0;
        int err = 0;
        dirent_t* ents = read_dir(path, &n, &err);
        if (!ents) {
            flush();
            if (!hush) dprintf(STDERR_FILENO, "grep: %s: %s\n", shown, strerror(err));
            bad = 1;
            return;
        }
        sort_dirents(ents, n);  // so the output does not depend on the disk
        for (uint32_t i = 0; i < n; i++) {
            char cp[PATH_MAX], cs[PATH_MAX];
            if (join_path(cp, sizeof cp, path, ents[i].name) < 0 ||
                (shown[0] ? join_path(cs, sizeof cs, shown, ents[i].name) : (strlcpy(cs, ents[i].name, sizeof cs) >= sizeof cs ? -1 : 0)) < 0) {
                flush();
                if (!hush) dprintf(STDERR_FILENO, "grep: %s/%s: %s\n", shown, ents[i].name, strerror(-ENAMETOOLONG));
                bad = 1;
                continue;
            }
            walk(cp, cs, 0);
        }
        free(ents);
        return;
    }
    int fd = sys_open(path, O_RDONLY);
    if (fd < 0) {
        flush();
        if (!hush) dprintf(STDERR_FILENO, "grep: %s: %s\n", shown, strerror(fd));
        bad = 1;
        return;
    }
    scan(fd, shown);
    sys_close(fd);
}

// ---- the command line ----

static buf_t pats;      // every pattern, newline-separated
static int have_pats;   // -e or -f seen

static void add_pattern(const char* p, uint32_t n) {
    if (have_pats) buf_put(&pats, "\n", 1);
    buf_put(&pats, p, n);
    have_pats = 1;
}

// -f FILE: one pattern a line; an empty file has no patterns at all.
static int add_pattern_file(const char* path) {
    int fd = strcmp(path, "-") ? sys_open(path, O_RDONLY) : STDIN_FILENO;
    if (fd < 0) {
        dprintf(STDERR_FILENO, "grep: %s: %s\n", path, strerror(fd));
        return -1;
    }
    buf_t all = { 0, 0, 0 };
    char tmp[512];
    int n;
    while ((n = sys_read(fd, tmp, sizeof tmp)) > 0) buf_put(&all, tmp, (uint32_t)n);
    if (fd != STDIN_FILENO) sys_close(fd);
    if (n < 0) {
        dprintf(STDERR_FILENO, "grep: %s: %s\n", path, strerror(n));
        return -1;
    }
    uint32_t start = 0;
    for (uint32_t i = 0; i < all.n; i++) {
        if (all.s[i] != '\n') continue;
        add_pattern(all.s + start, i - start);
        start = i + 1;
    }
    if (start < all.n) add_pattern(all.s + start, all.n - start);
    free(all.s);
    return 0;
}

static int bad_number(const char* what, const char* v) {
    dprintf(STDERR_FILENO, "grep: %s: invalid %s\n", v, what);
    return 2;
}

int main(int argc, char** argv) {
    unsigned reflags = 0;
    int pattern_files = 0;
    const char* ops[64];
    int nops = 0;
    for (int a = 1; a < argc; a++) {
        const char* s = argv[a];
        if (!strcmp(s, "--")) {
            for (a++; a < argc; a++) {
                if (nops == 64) return usage();
                ops[nops++] = argv[a];
            }
            break;
        }
        if (s[0] != '-' || !s[1]) {
            if (nops == 64) return usage();
            ops[nops++] = s;
            continue;
        }
        for (const char* o = s + 1; *o; o++) {
            char c = *o;
            if (c >= '0' && c <= '9') {  // -3: context both ways
                uint32_t v;
                if (parse_uint(o, &v)) return bad_number("context length", o);
                ctx_after = ctx_before = v;
                break;
            }
            if (strchr("efmABC", c)) {
                const char* v = o + 1;
                if (!*v) {
                    if (++a >= argc) {
                        dprintf(STDERR_FILENO, "grep: option requires an argument -- '%c'\n", c);
                        return usage();
                    }
                    v = argv[a];
                }
                if (c == 'e') {
                    add_pattern(v, (uint32_t)strlen(v));
                } else if (c == 'f') {
                    if (add_pattern_file(v)) return 2;
                    pattern_files++;
                } else {
                    uint32_t n;
                    if (parse_uint(v, &n)) return bad_number(c == 'm' ? "max count" : "context length", v);
                    if (c == 'm') max_count = n;
                    else if (c == 'A') ctx_after = n;
                    else if (c == 'B') ctx_before = n;
                    else ctx_after = ctx_before = n;
                }
                break;
            }
            switch (c) {
            case 'E': reflags = (reflags & ~(unsigned)RE_FIXED) | RE_ERE; break;
            case 'F': reflags = (reflags & ~(unsigned)RE_ERE) | RE_FIXED; break;
            case 'G': reflags &= ~(unsigned)(RE_ERE | RE_FIXED); break;
            case 'i': case 'y': reflags |= RE_ICASE; break;
            case 'w': reflags |= RE_WORD; break;
            case 'x': reflags |= RE_LINE; break;
            case 'v': invert = 1; break;
            case 'o': only_matching = 1; break;
            case 'c': count_only = 1; break;
            case 'l': list_with = 1; break;
            case 'L': list_without = 1; break;
            case 'q': quiet = 1; break;
            case 's': hush = 1; break;
            case 'n': with_number = 1; break;
            case 'H': show_name = name_forced = 1; break;
            case 'h': show_name = 0; name_forced = 1; break;
            case 'r': case 'R': recursive = 1; break;
            case 'a': as_text = 1; break;
            default:
                dprintf(STDERR_FILENO, "grep: invalid option -- '%c'\n", c);
                return usage();
            }
        }
    }
    int first = 0;
    if (!have_pats && !pattern_files) {
        if (!nops) return usage();
        add_pattern(ops[0], (uint32_t)strlen(ops[0]));
        first = 1;
    }
    no_patterns = pattern_files && pats.n == 0 && !have_pats;
    // "-f empty" leaves have_pats clear: nothing to compile.
    if (!no_patterns) {
        const char* err = re_compile(&re, pats.s ? pats.s : "", pats.n, reflags);
        if (err) {
            dprintf(STDERR_FILENO, "grep: %s\n", err);
            return 2;
        }
    }
    if (max_count == 0) return 1;

    int nfiles = nops - first;
    if (!name_forced && nfiles > 1) show_name = 1;
    if (nfiles == 0) {
        if (recursive) {
            if (!name_forced) show_name = 1;
            walk(".", "", 1);
        } else {
            scan(STDIN_FILENO, "(standard input)");
        }
    }
    for (int i = first; i < nops; i++) {
        if (!strcmp(ops[i], "-")) scan(STDIN_FILENO, "(standard input)");
        else walk(ops[i], ops[i], 1);
    }
    flush();
    if (quiet && selected_any) return 0;
    return bad ? 2 : selected_any ? 0 : 1;
}
