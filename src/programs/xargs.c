// xargs [-0rt] [-n N] [-I STR] [COMMAND [ARGS...]]
//
// Reads items from standard input and runs COMMAND with them added to
// the end of its arguments (echo, if there is no COMMAND). Items are
// split on blanks and newlines; a quote ('...' or "...") or a backslash
// keeps what follows as one piece, and "" is an empty item. There is no
// -L, -s, -P, -p, -d, -E, -x, -o or -a.
//   -0  items end at a NUL byte and nothing else is special (for find -print0)
//   -n  at most N items to a run
//   -I  run COMMAND once per line, with STR in its arguments (not the
//       command's own name) replaced by the whole line: blanks inside it
//       stay, leading blanks go, empty lines are skipped. It cancels -n,
//       and the later of the two wins, with a warning, as in GNU xargs.
//   -t  print each command line to standard error before it runs
//   -r  with no input at all, run nothing (GNU runs COMMAND once, with
//       no items; -I never does)
//
// There is no fork, so each run is a spawn, which takes 64 words and
// 3072 bytes of them, NULs counted, COMMAND and its arguments among
// them. A run is cut short at either limit and the rest goes to the next
// one, so the runs are not the ones GNU xargs makes on long input (its
// room is far larger). An item that cannot go in any run is an error.
// The children read an empty standard input (GNU uses /dev/null) and
// share xargs's output.
//
// Exit status: 0; 123 if a run exited with a nonzero status; 124 if one
// exited with 255, which stops xargs; 125 if one was killed or crashed,
// which stops it too; 126 if COMMAND could not be started, 127 if it was
// not found (both stop it); 1 for a bad quote or an unreadable input;
// 2 for a bad command line.
#include "ulib.h"

#define MAX_ARGS 64     // ARGV_MAX in the kernel
#define MAX_BYTES 3072  // SPAWN_ARGS_BYTES in syscall.c

static int nul_sep, no_empty, verbose;
static uint32_t per_run;  // -n; 0 means as many as fit
static const char* repl;  // -I
static const char* cmdv[MAX_ARGS + 1];
static int cmdc;
static uint32_t cmd_bytes;  // the command's own words, NULs counted
static int failed, read_failed;
static int null_in = -1;  // read end of a pipe nobody writes: a child's stdin

// The words of the next run: the command, then the items.
static const char* av[MAX_ARGS + 1];
static char pool[MAX_BYTES];  // the items' text
static uint32_t used;
static int count;

static char tok[MAX_BYTES];  // the item being read
static uint32_t toklen;

static int usage(void) {
    dprintf(STDERR_FILENO, "usage: xargs [-0rt] [-n N] [-I STR] [COMMAND [ARGS...]]\n");
    return 2;
}

static void too_long(void) {
    dprintf(STDERR_FILENO, "xargs: argument line too long\n");
    sys_exit(1);
}

// ---- reading -------------------------------------------------------

static unsigned char ib[1024];
static int ipos, ilen, eof;

static int getb(void) {
    if (ipos == ilen) {
        if (eof) return -1;
        int n = sys_read(STDIN_FILENO, ib, sizeof ib);
        if (n < 0) {
            dprintf(STDERR_FILENO, "xargs: standard input: %s\n", strerror(n));
            read_failed = 1;
        }
        if (n <= 0) { eof = 1; return -1; }
        ilen = n;
        ipos = 0;
    }
    return ib[ipos++];
}

static void put(int c) {
    if (toklen >= MAX_BYTES - 1) too_long();
    tok[toklen++] = (char)c;
}

static int bad_quote;  // which quote read_item found open

// 1 with the next item in tok, 0 at the end of the input, -1 for a
// quote that is never closed.
static int read_item(void) {
    int c, have = 0;
    toklen = 0;
    if (nul_sep) {
        while ((c = getb()) > 0) { put(c); have = 1; }
        return c == 0 || have;
    }
    for (;;) {
        c = getb();
        if (c < 0) return toklen > 0;  // as in GNU, an empty item at the very end is dropped
        if (c == '\n' || (!repl && (c == ' ' || c == '\t'))) {
            if (have) return 1;
            continue;
        }
        if (repl && !have && (c == ' ' || c == '\t')) continue;  // a line's leading blanks
        have = 1;
        if (c == '\\') {
            c = getb();
            if (c >= 0) put(c);  // a backslash at the very end is dropped
        } else if (c == '\'' || c == '"') {
            int q = c;
            while ((c = getb()) != q) {
                if (c < 0 && !toklen) return 0;  // GNU: a quote opened at the very end is no error
                if (c < 0 || c == '\n') { bad_quote = q; return -1; }
                put(c);
            }
        } else {
            put(c);
        }
    }
}

// ---- running -------------------------------------------------------

static char eb[256];
static int en;

static void eflush(void) {
    if (en) sys_write(STDERR_FILENO, eb, (uint32_t)en);
    en = 0;
}

static void ec(int c) {
    if (en == sizeof eb) eflush();
    eb[en++] = (char)c;
}

static int ctl(int c) { return c < 0x20 || c >= 0x7f; }

// Writes s as a shell would want it, the way GNU xargs -t does: bare
// if it is safe, 'in single quotes', "in double quotes" if all that
// is wrong with it is a ', or in pieces with $'\n' for odd bytes. (A '
// next to an odd byte comes out in a form GNU spells a little differently.)
static void quote(const char* s) {
    int safe = *s && *s != '~' && *s != '#', odd = 0, sq = 0, dq = 0;
    for (const char* p = s; *p; p++) {
        int c = (unsigned char)*p;
        if (ctl(c)) odd = 1;
        if (c == '\'') sq = 1;
        if (c == '"' || c == '$' || c == '`' || c == '\\') dq = 1;
        if (ctl(c) || strchr(" !\"$&'()*;<=>?[\\^`|", c)) safe = 0;
    }
    if (safe) {
        while (*s) ec(*s++);
    } else if (!odd && sq && !dq) {
        ec('"');
        while (*s) ec(*s++);
        ec('"');
    } else {
        int in = 0;  // 0 outside, 1 in '...', 2 in $'...'
        for (; *s; s++) {
            int c = (unsigned char)*s;
            if (ctl(c)) {
                if (in != 2) {
                    if (in == 1) ec('\'');
                    else ec('\''), ec('\'');
                    ec('$');
                    ec('\'');
                    in = 2;
                }
                static const char esc[] = "\a\b\t\n\v\f\r", name[] = "abtnvfr";
                const char* e = c && c < 0x20 ? strchr(esc, c) : 0;
                ec('\\');
                if (e) ec(name[e - esc]);
                else ec('0' + (c >> 6)), ec('0' + ((c >> 3) & 7)), ec('0' + (c & 7));
            } else {
                if (in != 1) {
                    if (in == 2) ec('\'');
                    ec('\'');
                    in = 1;
                }
                if (c == '\'') { ec('\''); ec('\\'); ec('\''); ec('\''); }
                else ec(c);
            }
        }
        if (!in) ec('\'');  // nothing was written: the empty string
        ec('\'');
    }
}

// What to say just before the next run starts (see main).
static const char* pending_err;

// Runs av (NULL-terminated) and waits. Returns if it exited 0 or with
// a status that merely counts as a failure.
static void run(void) {
    if (verbose) {
        for (int i = 0; av[i]; i++) {
            if (i) ec(' ');
            quote(av[i]);
        }
        ec('\n');
        eflush();
    }
    if (pending_err) {
        dprintf(STDERR_FILENO, "%s", pending_err);
        pending_err = 0;
    }
    spawn_opts_t o = { 0, 0, { null_in, -1, -1 } };
    int id = sys_spawn_opts(av[0], av, &o);
    if (id < 0) {
        dprintf(STDERR_FILENO, "xargs: %s: %s\n", av[0], strerror(id));
        sys_exit(id == -ENOENT ? 127 : 126);
    }
    int st = sys_join(id);
    if (st < 0) {
        dprintf(STDERR_FILENO, "xargs: %s: %s\n", av[0], st == EXIT_KILLED ? "killed" : "terminated by a fault");
        sys_exit(125);
    }
    if (st == 255) {
        dprintf(STDERR_FILENO, "xargs: %s: exited with status 255; aborting\n", av[0]);
        sys_exit(124);
    }
    if (st) failed = 1;
    // A child has its own copy of a redirected output file, and its own
    // offset in it; ours has not moved. Go to the end so the next run
    // writes after this one's output.
    for (int fd = STDOUT_FILENO; fd <= STDERR_FILENO; fd++)
        if (sys_fdtype(fd) == FDTYPE_FILE) sys_seek(fd, 0, SEEK_END);
}

static void run_batch(void) {
    av[cmdc + count] = 0;
    run();
    count = 0;
    used = 0;
}

static void add_item(void) {
    uint32_t need = toklen + 1, room = MAX_BYTES - cmd_bytes;
    int most = MAX_ARGS - cmdc;
    if (per_run && per_run < (uint32_t)most) most = (int)per_run;
    // A full run goes when the next item turns up, not before: a bad
    // quote after it then stops the input first, as in GNU xargs.
    if (count && (count >= most || used + need > room)) run_batch();
    if (need > room) too_long();
    memcpy(pool + used, tok, toklen);
    pool[used + toklen] = 0;
    av[cmdc + count++] = pool + used;
    used += need;
}

// -I: the command once, with repl in its words (but the first) replaced by tok.
static void run_repl(void) {
    uint32_t rl = (uint32_t)strlen(repl), u = 0, room = MAX_BYTES - (uint32_t)strlen(cmdv[0]) - 1;
    for (int i = 1; i < cmdc; i++) {
        av[i] = pool + u;
        for (const char* t = cmdv[i]; *t;) {
            const char* piece = t;
            uint32_t len = 1;
            if (!strncmp(t, repl, rl)) { piece = tok; len = toklen; t += rl; }
            else t++;
            if (u + len + 1 > room) too_long();
            memcpy(pool + u, piece, len);
            u += len;
        }
        if (u + 1 > room) too_long();
        pool[u++] = 0;
    }
    av[cmdc] = 0;
    run();
}

// ---- command line --------------------------------------------------

static int set_n(const char* s) {
    uint32_t v = 0;
    int ok = *s != 0;
    for (const char* p = s; *p; p++) {
        if (*p < '0' || *p > '9') { ok = 0; break; }
        if (v < 0x7fffffff / 10) v = v * 10 + (uint32_t)(*p - '0');
        else v = 0x7fffffff;
    }
    if (!ok) { dprintf(STDERR_FILENO, "xargs: invalid number \"%s\" for -n option\n", s); return -1; }
    if (!v) { dprintf(STDERR_FILENO, "xargs: value 0 for -n option should be >= 1\n"); return -1; }
    if (repl) {
        dprintf(STDERR_FILENO, "xargs: warning: options --replace and --max-args/-n are mutually exclusive, ignoring previous --replace value\n");
        repl = 0;
    }
    per_run = v;
    return 0;
}

static int set_repl(const char* s) {
    if (!*s) return -1;
    if (per_run) {
        dprintf(STDERR_FILENO, "xargs: warning: options --max-args and --replace/-I/-i are mutually exclusive, ignoring previous --max-args value\n");
        per_run = 0;
    }
    repl = s;
    return 0;
}

int main(int argc, char** argv) {
    int a = 1;
    for (; a < argc && argv[a][0] == '-' && argv[a][1]; a++) {
        const char* s = argv[a];
        if (s[1] == '-') {
            if (!s[2]) { a++; break; }
            const char* eq = strchr(s, '=');
            uint32_t nl = eq ? (uint32_t)(eq - s) : (uint32_t)strlen(s);
            if (!strcmp(s, "--null")) nul_sep = 1;
            else if (!strcmp(s, "--no-run-if-empty")) no_empty = 1;
            else if (!strcmp(s, "--verbose")) verbose = 1;
            else if (nl == 10 && !strncmp(s, "--max-args", nl)) {
                if (!eq && ++a == argc) return usage();
                if (set_n(eq ? eq + 1 : argv[a])) return usage();
            } else if (nl == 9 && !strncmp(s, "--replace", nl)) {
                if (set_repl(eq ? eq + 1 : "{}")) return usage();
            } else {
                return usage();
            }
            continue;
        }
        for (const char* o = s + 1; *o; o++) {
            if (*o == '0') nul_sep = 1;
            else if (*o == 'r') no_empty = 1;
            else if (*o == 't') verbose = 1;
            else if (*o == 'i') {  // the old spelling: the text may follow, or it is {}
                if (set_repl(o[1] ? o + 1 : "{}")) return usage();
                break;
            } else if (*o == 'n' || *o == 'I') {
                const char* v = o + 1;
                if (!*v && ++a == argc) return usage();
                if (!*v) v = argv[a];
                if (*o == 'n' ? set_n(v) : set_repl(v)) return usage();
                break;
            } else {
                return usage();
            }
        }
    }
    if (a == argc) {
        cmdv[cmdc++] = "echo";
    } else {
        // Spawn gave xargs itself at most 64 words and 3072 bytes, so
        // the command is never too long to leave room for an item.
        for (; a < argc; a++) cmdv[cmdc++] = argv[a];
    }
    for (int i = 0; i < cmdc; i++) {
        cmd_bytes += (uint32_t)strlen(cmdv[i]) + 1;
        av[i] = cmdv[i];
    }

    int p[2];
    if (sys_pipe(p) == 0) {
        sys_close(p[1]);
        null_in = p[0];
    }

    int r;
    while ((r = read_item()) > 0) {
        if (repl) run_repl();
        else add_item();
    }
    if (r < 0) {
        // The items before the bad quote still run, after the complaint
        // (and after the -t line, as in GNU xargs).
        pending_err = bad_quote == '\'' ?
            "xargs: unmatched single quote; by default quotes are special to xargs unless you use the -0 option\n" :
            "xargs: unmatched double quote; by default quotes are special to xargs unless you use the -0 option\n";
        if (count) run_batch();
        if (pending_err) dprintf(STDERR_FILENO, "%s", pending_err);
        return 1;
    }
    if (!repl && (count || !no_empty)) run_batch();  // no items: the command once, unless -r
    return failed ? 123 : read_failed;
}
