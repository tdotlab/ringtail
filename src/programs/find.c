// find [PATH...] [EXPRESSION]    walk directory trees and act on what matches
//
// Each PATH (default ".") is visited, then everything under it, and the
// EXPRESSION is evaluated for every entry. Paths come out as they were
// given with the entry's name added: `find docs` prints "docs/a.txt",
// `find` alone prints "./a.txt". Entries come in the order the disk
// keeps them, which is not sorted.
//
// Tests (true or false for an entry):
//   -name PAT     the entry's own name matches the glob PAT (* ? [a-z] [!x])
//   -iname PAT    the same. RingTail's disk ignores case, so -name does too
//   -path PAT     the whole path matches PAT; here * also matches "/"
//   -type f|d     a file, or a directory
//   -size [+-]N[bckMG]
//                 size in units, rounded up (b = 512 bytes, the default);
//                 +N is more than N units, -N fewer, N exactly
//   -empty        a file of no bytes, or a directory with nothing in it
// Options (always true, may stand anywhere):
//   -maxdepth N   do not go below N levels (0 = just the PATHs)
//   -mindepth N   do not test or act above level N
//   -depth        look into a directory before testing it
// Actions:
//   -print        write the path and a newline (the default, if there is
//                 no action at all)
//   -print0       write the path and a NUL
//   -exec CMD ... ;   run CMD with each {} replaced by the path; true if it
//                 exits 0. CMD must be a program: built-ins of the shell
//                 such as cd and pwd are not programs
//   -delete       remove a file or an empty directory; implies -depth
// Operators: ! or -not, -a or -and (also implied between tests), -o or
// -or, and ( ... ), which may be written \( ... \) as well.
//
// Directories are read in full before anything in them is touched, so
// -delete and -exec may remove or add files without confusing the walk.
// The exit status is 0, or 1 if a PATH could not be read or something
// could not be deleted, or 2 for a bad expression.
#include "ulib.h"

#define MAX_NODES 64
#define MAX_EXEC 16

enum { N_NAME, N_PATH, N_TYPE, N_SIZE, N_EMPTY, N_TRUE, N_PRINT, N_PRINT0, N_EXEC, N_DELETE, N_NOT, N_AND, N_OR };

typedef struct {
    int kind;
    int a, b;          // children; for N_EXEC the first argument and how many; N_TYPE the letter
    const char* s;     // the pattern
    uint32_t n, unit;  // N_SIZE: the number, and what one unit is in bytes
    int cmp;           // N_SIZE: -1 fewer, 0 exactly, 1 more
} node_t;

static node_t nodes[MAX_NODES];
static int nn, root, has_action;
static char** av;
static int ac, pos;
static uint32_t maxdepth = 0xFFFFFFFFu, mindepth;
static int depth_first;

static void die(const char* fmt, const char* arg) __attribute__((noreturn));
static void die(const char* fmt, const char* arg) {
    dprintf(STDERR_FILENO, "find: ");
    dprintf(STDERR_FILENO, fmt, arg);
    dprintf(STDERR_FILENO, "\n");
    sys_exit(2);
}

// ---- output ----

static char ob[1024];
static uint32_t on;

static void flush(void) {
    const char* p = ob;
    while (on) {
        int w = sys_write(STDOUT_FILENO, p, on);
        if (w <= 0) {
            if (w != -EPIPE) dprintf(STDERR_FILENO, "find: write error: %s\n", w ? strerror(w) : "no progress");
            sys_exit(1);
        }
        p += w;
        on -= (uint32_t)w;
    }
}

static void emit(const char* s, uint32_t n) {
    while (n) {
        uint32_t take = sizeof ob - on < n ? (uint32_t)sizeof ob - on : n;
        memcpy(ob + on, s, take);
        on += take;
        s += take;
        n -= take;
        if (on == sizeof ob) flush();
    }
}

// ---- globs, ignoring case ----

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int glob(const char* p, const char* s) {
    while (*p) {
        if (*p == '*') {
            while (*p == '*') p++;
            if (!*p) return 1;
            for (;; s++) {
                if (glob(p, s)) return 1;
                if (!*s) return 0;
            }
        }
        if (*p == '?') {
            if (!*s) return 0;
            p++;
            s++;
        } else if (*p == '[') {
            const char* q = p + 1;
            int neg = *q == '!' || *q == '^', match = 0, first = 1;
            if (neg) q++;
            while (*q && (*q != ']' || first)) {
                first = 0;
                int lo = (unsigned char)*q, hi = lo;
                if (q[1] == '-' && q[2] && q[2] != ']') {
                    hi = (unsigned char)q[2];
                    q += 3;
                } else {
                    q++;
                }
                int c = lower((unsigned char)*s);
                if (*s && c >= lower(lo) && c <= lower(hi)) match = 1;
            }
            if (*q == ']') {
                if (!*s || match == neg) return 0;
                p = q + 1;
                s++;
            } else {  // no closing bracket: a plain [
                if (lower((unsigned char)*s) != '[') return 0;
                p++;
                s++;
            }
        } else {
            if (*p == '\\' && p[1]) p++;
            if (lower((unsigned char)*p) != lower((unsigned char)*s)) return 0;
            p++;
            s++;
        }
    }
    return !*s;
}

// ---- the expression ----

static int new_node(int kind) {
    if (nn == MAX_NODES) die("the expression is too long%s", "");
    nodes[nn].kind = kind;
    return nn++;
}

static int binary(int kind, int l, int r) {
    int n = new_node(kind);
    nodes[n].a = l;
    nodes[n].b = r;
    return n;
}

static int is_open(const char* s) { return !strcmp(s, "(") || !strcmp(s, "\\("); }
static int is_close(const char* s) { return !strcmp(s, ")") || !strcmp(s, "\\)"); }
static int is_or(const char* s) { return !strcmp(s, "-o") || !strcmp(s, "-or"); }
static int is_and(const char* s) { return !strcmp(s, "-a") || !strcmp(s, "-and"); }

static const char* value(const char* what) {
    if (pos >= ac) die("missing argument to '%s'", what);
    return av[pos++];
}

static uint32_t number(const char* what) {
    const char* v = value(what);
    uint32_t n;
    if (parse_uint(v, &n)) {
        dprintf(STDERR_FILENO, "find: invalid argument '%s' to '%s'\n", v, what);
        sys_exit(2);
    }
    return n;
}

static void parse_size(node_t* n, const char* v) {
    const char* p = v;
    n->cmp = 0;
    if (*p == '+') { n->cmp = 1; p++; }
    else if (*p == '-') { n->cmp = -1; p++; }
    uint32_t num = 0;
    int digits = 0;
    for (; *p >= '0' && *p <= '9'; p++, digits++) {
        if (num > (0xFFFFFFFFu - 9) / 10) die("size '%s' is too big", v);
        num = num * 10 + (uint32_t)(*p - '0');
    }
    n->unit = 512;
    if (*p == 'c') n->unit = 1;
    else if (*p == 'k') n->unit = 1024;
    else if (*p == 'M') n->unit = 1024 * 1024;
    else if (*p == 'G') n->unit = 1024 * 1024 * 1024;
    else if (*p == 'b' || !*p) n->unit = 512;
    else digits = 0;
    if (!digits || (*p && p[1])) die("invalid argument '%s' to '-size'", v);
    n->n = num;
}

static int parse_or(void);

static int primary(void) {
    if (pos >= ac) die("the expression ends where a test was expected%s", "");
    const char* s = av[pos++];
    if (is_open(s)) {
        int e = parse_or();
        if (pos >= ac || !is_close(av[pos])) die("a '(' has no matching ')'%s", "");
        pos++;
        return e;
    }
    if (is_close(s)) die("unexpected '%s'", s);
    if (!strcmp(s, "!") || !strcmp(s, "-not")) {
        int c = primary();
        int n = new_node(N_NOT);
        nodes[n].a = c;
        return n;
    }
    if (!strcmp(s, "-name") || !strcmp(s, "-iname") || !strcmp(s, "-path") || !strcmp(s, "-ipath")) {
        int n = new_node(s[1] == 'p' || s[2] == 'p' ? N_PATH : N_NAME);
        nodes[n].s = value(s);
        return n;
    }
    if (!strcmp(s, "-type")) {
        const char* v = value(s);
        if ((v[0] != 'f' && v[0] != 'd') || v[1]) die("unknown argument '%s' to '-type' (only f and d exist here)", v);
        int n = new_node(N_TYPE);
        nodes[n].a = v[0];
        return n;
    }
    if (!strcmp(s, "-size")) {
        int n = new_node(N_SIZE);
        parse_size(&nodes[n], value(s));
        return n;
    }
    if (!strcmp(s, "-empty")) return new_node(N_EMPTY);
    if (!strcmp(s, "-maxdepth")) { maxdepth = number(s); return new_node(N_TRUE); }
    if (!strcmp(s, "-mindepth")) { mindepth = number(s); return new_node(N_TRUE); }
    if (!strcmp(s, "-depth")) { depth_first = 1; return new_node(N_TRUE); }
    if (!strcmp(s, "-print")) { has_action = 1; return new_node(N_PRINT); }
    if (!strcmp(s, "-print0")) { has_action = 1; return new_node(N_PRINT0); }
    if (!strcmp(s, "-delete")) { has_action = depth_first = 1; return new_node(N_DELETE); }
    if (!strcmp(s, "-exec")) {
        int first = pos;
        while (pos < ac && strcmp(av[pos], ";") && strcmp(av[pos], "\\;")) pos++;
        if (pos >= ac || pos == first) die("missing argument to '-exec' (it ends with ;)%s", "");
        if (pos - first > MAX_EXEC) die("-exec has too many arguments%s", "");
        int n = new_node(N_EXEC);
        nodes[n].a = first;
        nodes[n].b = pos - first;
        pos++;
        has_action = 1;
        return n;
    }
    die("unknown predicate '%s'", s);
}

static int parse_and(void) {
    int l = primary();
    while (pos < ac && !is_close(av[pos]) && !is_or(av[pos])) {
        if (is_and(av[pos])) pos++;
        l = binary(N_AND, l, primary());
    }
    return l;
}

static int parse_or(void) {
    int l = parse_and();
    while (pos < ac && is_or(av[pos])) {
        pos++;
        l = binary(N_OR, l, parse_and());
    }
    return l;
}

// ---- the walk ----

typedef struct {
    const char* path;
    const char* base;  // the entry's own name
    const dirent_t* d;
} ent_t;

static char path[PATH_MAX];
static int status;

static int do_exec(const node_t* n, const ent_t* e) {
    static char buf[MAX_EXEC][2 * PATH_MAX];
    const char* args[MAX_EXEC + 1];
    uint32_t plen = (uint32_t)strlen(e->path);
    for (int i = 0; i < n->b; i++) {
        const char* t = av[n->a + i];
        uint32_t o = 0;
        while (*t) {
            const char* piece = t;
            uint32_t len = 1;
            if (t[0] == '{' && t[1] == '}') { piece = e->path; len = plen; t += 2; }
            else t++;
            if (o + len >= sizeof buf[i]) {
                flush();
                dprintf(STDERR_FILENO, "find: an -exec argument for %s is too long\n", e->path);
                status = 1;
                return 0;
            }
            memcpy(buf[i] + o, piece, len);
            o += len;
        }
        buf[i][o] = 0;
        args[i] = buf[i];
    }
    args[n->b] = 0;
    flush();
    int id = sys_spawn_args(args[0], args);
    if (id < 0) {
        dprintf(STDERR_FILENO, "find: %s: %s%s\n", args[0], strerror(id),
                id == -ENOENT ? " (-exec runs programs; shell built-ins like cd and pwd are not)" : "");
        status = 1;
        return 0;
    }
    return sys_join(id) == 0;
}

static int eval(int i, const ent_t* e) {
    const node_t* n = &nodes[i];
    int dir = (e->d->flags & DIRENT_DIR) != 0;
    switch (n->kind) {
    case N_NAME: return glob(n->s, e->base);
    case N_PATH: return glob(n->s, e->path);
    case N_TYPE: return n->a == 'd' ? dir : !dir;
    case N_SIZE: {
        uint32_t units = n->unit == 1 ? e->d->size : (uint32_t)(((uint64_t)e->d->size + n->unit - 1) / n->unit);
        return n->cmp < 0 ? units < n->n : n->cmp > 0 ? units > n->n : units == n->n;
    }
    case N_EMPTY: {
        if (!dir) return e->d->size == 0;
        int cursor = 0;
        dirent_t x;
        return sys_readdir(e->path, &cursor, &x) == 0;
    }
    case N_TRUE: return 1;
    case N_PRINT:
        emit(e->path, (uint32_t)strlen(e->path));
        emit("\n", 1);
        return 1;
    case N_PRINT0:
        emit(e->path, (uint32_t)strlen(e->path) + 1);
        return 1;
    case N_EXEC: return do_exec(n, e);
    case N_DELETE: {
        int r = dir ? sys_rmdir(e->path) : sys_delete(e->path);
        if (r < 0) {
            flush();
            dprintf(STDERR_FILENO, "find: cannot delete %s: %s\n", e->path, strerror(r));
            status = 1;
            return 0;
        }
        return 1;
    }
    case N_NOT: return !eval(n->a, e);
    case N_AND: return eval(n->a, e) && eval(n->b, e);
    case N_OR: return eval(n->a, e) || eval(n->b, e);
    }
    return 0;
}

static void visit(uint32_t plen, const char* base, const dirent_t* d, uint32_t depth);

// The entries of path (its length is plen), all read before the first
// is visited.
static void descend(uint32_t plen, uint32_t depth) {
    dirent_t* list = 0;
    uint32_t n = 0, cap = 0;
    int cursor = 0, r;
    for (;;) {
        if (n == cap) {
            uint32_t want = cap ? cap * 2 : 32;
            dirent_t* p = realloc(list, want * sizeof *list);
            if (!p) {
                flush();
                dprintf(STDERR_FILENO, "find: %s: out of memory\n", path);
                status = 1;
                break;
            }
            list = p;
            cap = want;
        }
        r = sys_readdir(path, &cursor, &list[n]);
        if (r <= 0) break;
        if (strcmp(list[n].name, ".") && strcmp(list[n].name, "..")) n++;
    }
    if (r < 0) {
        flush();
        dprintf(STDERR_FILENO, "find: %s: %s\n", path, strerror(r));
        status = 1;
    }
    for (uint32_t i = 0; i < n; i++) {
        uint32_t at = plen;
        if (at && path[at - 1] != '/') path[at++] = '/';
        uint32_t len = (uint32_t)strlen(list[i].name);
        if (at + len >= sizeof path) {
            path[plen] = 0;
            flush();
            dprintf(STDERR_FILENO, "find: %s/%s: the path is too long\n", path, list[i].name);
            status = 1;
            continue;
        }
        memcpy(path + at, list[i].name, len + 1);
        visit(at + len, path + at, &list[i], depth + 1);
        path[plen] = 0;
    }
    free(list);
}

static void visit(uint32_t plen, const char* base, const dirent_t* d, uint32_t depth) {
    ent_t e = { path, base, d };
    int dir = (d->flags & DIRENT_DIR) != 0;
    if (!depth_first && depth >= mindepth) eval(root, &e);
    if (dir && depth < maxdepth) descend(plen, depth);
    if (depth_first && depth >= mindepth) eval(root, &e);
}

int main(int argc, char** argv) {
    av = argv;
    ac = argc;
    int first = 1;
    while (first < argc && argv[first][0] != '-' && strcmp(argv[first], "!") && !is_open(argv[first])) first++;
    pos = first;
    if (pos < ac) {
        root = parse_or();
        if (pos < ac) die("unexpected '%s'", av[pos]);
    } else {
        root = new_node(N_TRUE);
    }
    if (!has_action) {
        int p = new_node(N_PRINT);
        root = binary(N_AND, root, p);
    }

    int starts = first > 1 ? first - 1 : 1;
    for (int i = 0; i < starts; i++) {
        const char* s = first > 1 ? argv[1 + i] : ".";
        uint32_t len = (uint32_t)strlen(s);
        if (!len || len >= sizeof path) {
            dprintf(STDERR_FILENO, "find: '%s': %s\n", s, len ? "the path is too long" : "empty path");
            status = 1;
            continue;
        }
        memcpy(path, s, len + 1);
        dirent_t d;
        int r = sys_stat(path, &d);
        if (r < 0) {
            dprintf(STDERR_FILENO, "find: %s: %s\n", s, strerror(r));
            status = 1;
            continue;
        }
        // The name of the starting point: its last component, without any
        // trailing slashes ("/" stays as it is).
        char base[PATH_MAX];
        uint32_t end = len;
        while (end > 1 && path[end - 1] == '/') end--;
        uint32_t from = end;
        while (from > 0 && path[from - 1] != '/') from--;
        if (end == 1 && path[0] == '/') from = 0;
        memcpy(base, path + from, end - from);
        base[end - from] = 0;
        visit(len, base, &d, 0);
    }
    flush();
    return status;
}
