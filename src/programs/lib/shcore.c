// The shell language (sh.h): reading commands, expanding words and running
// them. Nothing here knows which system it runs on; that is shos.h.
//
// A command is parsed into a tree of nodes in an arena that is let go once
// the command has run. Words stay as the text they were typed as, and are
// expanded only when their command runs. A function keeps its body as text
// and parses it again at each call.
//
// Expansion builds a word a character at a time, each with a mark that says
// where it came from (typed, quoted, or the result of an expansion): field
// splitting looks only at the results of expansions, and wildcards only at
// what was not quoted.
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include "sh.h"
#include "shos.h"
#include "pfcore.h"

// The C library: RingTail's (lib.h) or the host's.
void* memcpy(void* dst, const void* src, size_t n);
void* memmove(void* dst, const void* src, size_t n);
void* memset(void* dst, int c, size_t n);
int memcmp(const void* a, const void* b, size_t n);
size_t strlen(const char* s);
int strcmp(const char* a, const char* b);
int strncmp(const char* a, const char* b, size_t n);
char* strchr(const char* s, int c);
void* malloc(size_t n);
void* realloc(void* p, size_t n);
void free(void* p);
int vsnprintf(char* buf, size_t size, const char* fmt, va_list ap);
int snprintf(char* buf, size_t size, const char* fmt, ...) __attribute__((format(printf, 3, 4)));

// errno and signal numbers: RingTail's are Linux's.
#define E_NOENT  2
#define E_INTR   4
#define E_NOEXEC 8
#define E_INVAL  22
#define SIG_INT  2

#define MAX_DEPTH  200   // how deeply commands may nest in the text
#define MAX_CALLS  100   // how deeply functions may call each other
#define HEREDOC_MAX 4096 // a here-document goes through a pipe this big
#define MAX_BG     32    // background jobs `wait` remembers

volatile int sh_interrupted;

// ---- Memory ----

static __attribute__((noreturn)) void oom(void) {
    static const char m[] = "sh: out of memory\n";
    shos_write(2, m, sizeof m - 1);
    shos_exit(2);
}

static void* xmalloc(size_t n) {
    void* p = malloc(n ? n : 1);
    if (!p) oom();
    return p;
}

static void* xrealloc(void* p, size_t n) {
    p = realloc(p, n ? n : 1);
    if (!p) oom();
    return p;
}

static char* xstrndup(const char* s, size_t n) {
    char* d = xmalloc(n + 1);
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}

static char* xstrdup(const char* s) { return xstrndup(s, strlen(s)); }

// The arena: what one command needs while it is parsed and run.
typedef struct ablock {
    struct ablock* prev;
    size_t used, size, pad;  // (pad: data starts 16-aligned)
    char data[];
} ablock_t;
static ablock_t* arena;
typedef struct { ablock_t* b; size_t used; } amark_t;

static void* aalloc(size_t n) {
    n = (n + 15) & ~(size_t)15;
    if (!arena || arena->used + n > arena->size) {
        size_t size = 8192 - sizeof(ablock_t);
        if (n > size) size = n;
        ablock_t* b = xmalloc(sizeof *b + size);
        b->prev = arena;
        b->used = 0;
        b->size = size;
        arena = b;
    }
    void* p = arena->data + arena->used;
    arena->used += n;
    memset(p, 0, n);
    return p;
}

static amark_t amark(void) {
    amark_t m = { arena, arena ? arena->used : 0 };
    return m;
}

static void arelease(amark_t m) {
    while (arena != m.b) {
        ablock_t* p = arena->prev;
        free(arena);
        arena = p;
    }
    if (arena) arena->used = m.used;
}

static char* adup(const char* s, int n) {
    char* d = aalloc((size_t)n + 1);
    memcpy(d, s, (size_t)n);
    return d;
}

// Text that grows (malloc'd).
typedef struct { char* s; int n, cap; } str_t;

static void sput(str_t* b, const char* s, int n) {
    if (b->n + n + 1 > b->cap) {
        b->cap = (b->n + n + 1) * 2 + 32;
        b->s = xrealloc(b->s, (size_t)b->cap);
    }
    memcpy(b->s + b->n, s, (size_t)n);
    b->n += n;
    b->s[b->n] = 0;
}

static void sputc(str_t* b, char c) { sput(b, &c, 1); }
static void sputs(str_t* b, const char* s) { sput(b, s, (int)strlen(s)); }

// A list of strings, NULL-terminated (the array is malloc'd).
typedef struct { char** v; int n, cap; } argv_t;

static void av_add(argv_t* a, char* s) {
    if (a->n + 2 > a->cap) {
        a->cap = a->cap * 2 + 8;
        a->v = xrealloc(a->v, (size_t)a->cap * sizeof *a->v);
    }
    a->v[a->n++] = s;
    a->v[a->n] = 0;
}

// ---- Small helpers ----

static int is_digit(int c) { return c >= '0' && c <= '9'; }
static int is_alpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static int is_name1(int c) { return is_alpha(c) || c == '_'; }
static int is_namec(int c) { return is_name1(c) || is_digit(c); }

// Whether s[0..n) is a variable name.
static int is_name(const char* s, int n) {
    if (n <= 0 || !is_name1((unsigned char)s[0])) return 0;
    for (int i = 1; i < n; i++)
        if (!is_namec((unsigned char)s[i])) return 0;
    return 1;
}

static char* itoa64(int64_t v, char* buf) {  // buf: 24 bytes
    char t[24];
    int n = 0;
    uint64_t u = v < 0 ? 0 - (uint64_t)v : (uint64_t)v;
    do t[n++] = (char)('0' + u % 10); while (u /= 10);
    int k = 0;
    if (v < 0) buf[k++] = '-';
    while (n) buf[k++] = t[--n];
    buf[k] = 0;
    return buf;
}

// A decimal number, all of s; 0 on success.
static int parse_int(const char* s, int64_t* out) {
    int neg = 0;
    uint64_t v = 0;
    if (*s == '-' || *s == '+') neg = *s++ == '-';
    if (!*s) return -1;
    for (; *s; s++) {
        if (!is_digit((unsigned char)*s)) return -1;
        if (v > (UINT64_C(1) << 63) / 10) return -1;
        v = v * 10 + (uint64_t)(*s - '0');
        if (v > (UINT64_C(1) << 63)) return -1;
    }
    if (!neg && v == (UINT64_C(1) << 63)) return -1;
    *out = neg ? (int64_t)(0 - v) : (int64_t)v;
    return 0;
}

// ---- The shell's state ----

enum { CTL_NONE, CTL_BREAK, CTL_CONTINUE, CTL_RETURN, CTL_EXIT, CTL_ABORT, CTL_INTR };

typedef struct var {
    struct var* next;
    char* name, *val;
    int exported;
} var_t;

typedef struct func {
    struct func* next;
    char* name, *body;
    int len;
} func_t;

// A variable's value before a function changed it (local), or before a
// command's own assignments (x=1 cmd), to be put back.
typedef struct save {
    struct save* next;
    char* name, *val;  // val 0: it was unset
} save_t;

static struct {
    const sh_front_t* front;
    int interactive;  // the prompt's own shell
    int ui;           // messages as at the prompt (also in its child shells)
    char* arg0;
    char** pv;        // the positional parameters (malloc'd)
    int pc;
    int last;         // $?
    int ctl, ctln;    // control flow under way, and break/continue's count
    int exit_status, abort_status, retval;
    int opt_e, opt_u, opt_x, opt_f;
    int noerrexit;    // > 0 where set -e does not apply
    int loops, fdepth, dotdepth;
    int pid, bg;      // $$ and $!
    int bgids[MAX_BG], nbg;
    int subst_status, did_subst;
    str_t* cap;       // where a builtin's output goes inside $( )
    save_t** frame;   // the running function's saved variables
    int slots;        // the descriptor slots in use for saving
    int sig_reported;
    int wfail;  // a builtin could not write its output
    int exit_asked;
    int feed_skip;
} S;

#define NBUCKETS 64
static var_t* vars[NBUCKETS];
static func_t* funcs;

static unsigned vhash(const char* s, int n) {
    unsigned h = 5381;
    for (int i = 0; i < n; i++) h = h * 33 + (unsigned char)s[i];
    return h % NBUCKETS;
}

static var_t* vfind(const char* name, int n) {
    for (var_t* v = vars[vhash(name, n)]; v; v = v->next)
        if ((int)strlen(v->name) == n && memcmp(v->name, name, (size_t)n) == 0) return v;
    return 0;
}

static const char* vget(const char* name) {
    var_t* v = vfind(name, (int)strlen(name));
    return v ? v->val : 0;
}

static void vsetn(const char* name, int n, const char* val) {
    var_t* v = vfind(name, n);
    char* copy = xstrdup(val);
    if (v) {
        free(v->val);
        v->val = copy;
        return;
    }
    v = xmalloc(sizeof *v);
    v->name = xstrndup(name, (size_t)n);
    v->val = copy;
    v->exported = 0;
    unsigned h = vhash(name, n);
    v->next = vars[h];
    vars[h] = v;
}

static void vset(const char* name, const char* val) { vsetn(name, (int)strlen(name), val); }

static void vunset(const char* name) {
    int n = (int)strlen(name);
    for (var_t** p = &vars[vhash(name, n)]; *p; p = &(*p)->next) {
        if (strcmp((*p)->name, name) == 0) {
            var_t* v = *p;
            *p = v->next;
            free(v->name);
            free(v->val);
            free(v);
            return;
        }
    }
}

static func_t* ffind(const char* name) {
    for (func_t* f = funcs; f; f = f->next)
        if (strcmp(f->name, name) == 0) return f;
    return 0;
}

static void fdefine(const char* name, int n, char* body, int len) {
    func_t* f;
    for (f = funcs; f; f = f->next)
        if ((int)strlen(f->name) == n && memcmp(f->name, name, (size_t)n) == 0) break;
    if (!f) {
        f = xmalloc(sizeof *f);
        f->name = xstrndup(name, (size_t)n);
        f->next = funcs;
        funcs = f;
    } else {
        free(f->body);
    }
    f->body = body;
    f->len = len;
}

static void fremove(const char* name) {
    for (func_t** p = &funcs; *p; p = &(*p)->next) {
        if (strcmp((*p)->name, name) == 0) {
            func_t* f = *p;
            *p = f->next;
            free(f->name);
            free(f->body);
            free(f);
            return;
        }
    }
}

// Remembers name's value in *list, unless it is there already.
static void save_var(save_t** list, const char* name, int n) {
    for (save_t* s = *list; s; s = s->next)
        if ((int)strlen(s->name) == n && memcmp(s->name, name, (size_t)n) == 0) return;
    save_t* s = xmalloc(sizeof *s);
    s->name = xstrndup(name, (size_t)n);
    var_t* v = vfind(name, n);
    s->val = v ? xstrdup(v->val) : 0;
    s->next = *list;
    *list = s;
}

// Puts back what list saved, newest first.
static void restore_vars(save_t* list) {
    while (list) {
        save_t* s = list;
        list = s->next;
        if (s->val) vset(s->name, s->val);
        else vunset(s->name);
        free(s->name);
        free(s->val);
        free(s);
    }
}

static void set_params(char** v, int n) {
    for (int i = 0; i < S.pc; i++) free(S.pv[i]);
    free(S.pv);
    S.pv = xmalloc((size_t)(n + 1) * sizeof *S.pv);
    for (int i = 0; i < n; i++) S.pv[i] = xstrdup(v[i]);
    S.pv[n] = 0;
    S.pc = n;
}

static const char* ifs(void) {
    const char* v = vget("IFS");
    return v ? v : " \t\n";
}

// ---- Messages ----

// Where messages go when not to the shell's own standard error: a
// command's 2> while the shell speaks for it (it could not be started).
static int say_fd = 2;

static void say(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char* fmt, ...) {
    char buf[600];
    int k = 0;
    if (!S.ui) {
        k = snprintf(buf, sizeof buf, "%s: ", S.arg0 ? S.arg0 : "sh");
        if (k < 0 || k > 200) k = 0;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf + k, sizeof buf - (size_t)k - 1, fmt, ap);
    va_end(ap);
    size_t n = strlen(buf);
    buf[n++] = '\n';
    buf[n] = 0;
    if (say_fd != 2) shos_write(say_fd, buf, n);
    else if (S.front && S.front->message) S.front->message(buf);
    else shos_write(2, buf, n);
}

// An error that stops the command (and a script).
static void sh_abort(int status) {
    if (!S.ctl) {
        S.ctl = CTL_ABORT;
        S.abort_status = status;
    }
}

// ---- Reading commands: tokens ----

enum {
    T_EOF, T_NL, T_WORD, T_IONUM, T_SEMI, T_AMP, T_PIPE, T_AND, T_OR, T_DSEMI, T_LPAR, T_RPAR,
    T_LESS, T_GREAT, T_DGREAT, T_LESSAND, T_GREATAND, T_LESSGREAT, T_DLESS, T_DLESSDASH, T_CLOBBER,
};
enum { P_OK, P_MORE, P_BAD };

// Where a here-document's text is in the source, and where its << is.
typedef struct hdrec {
    struct hdrec* next;
    int op_at, bs, be;  // be: past the line with the delimiter
} hdrec_t;

typedef struct {
    const char* base;
    int len;
    hdrec_t* hdocs, **hdtail;
} src_t;

typedef struct word {
    struct word* next;
    const char* p;
    int n;
} word_t;

typedef struct redir {
    struct redir* next;
    int fd, op, op_at;
    const char* p;       // the word, as written
    int n;
    const char* dp;      // a here-document's delimiter, quotes removed
    int dn;
    const char* body;    // its text
    int blen, quoted, strip;
} redir_t;

enum { N_LIST, N_AND, N_OR, N_PIPE, N_SIMPLE, N_IF, N_WHILE, N_UNTIL, N_FOR, N_CASE, N_BRACE, N_SUBSHELL, N_FUNC };

typedef struct citem {
    struct citem* next;
    word_t* pats;
    struct node* body;  // 0: nothing to run
} citem_t;

typedef struct node {
    int kind, s, e;      // s..e: its text in src
    src_t* src;
    struct node* next;   // in a list or pipeline
    int bg;              // in a list: ended by &
    redir_t* redirs;
    word_t* assigns, *words;  // N_SIMPLE; N_FOR's list; N_CASE's subject
    struct node* kids;   // N_LIST, N_PIPE
    int bang;
    struct node* a, *b, *c;  // if: condition, then, else; loops: condition, body; and/or: both sides
    const char* name;    // N_FOR, N_FUNC
    int nlen, has_in;
    citem_t* items;
} node_t;

typedef struct {
    src_t* src;
    const char* s;
    int n, i;
    int eof_ok;          // the end of the text ends a command
    int have, tok, ts, te;
    int pe;              // the end of the last token taken
    int err;
    char emsg[160];
    redir_t* pend[16];   // here-documents waiting for their line to end
    int npend;
    int depth;
} lex_t;

static void lx_init(lex_t* L, src_t* src, int eof_ok) {
    memset(L, 0, sizeof *L);
    L->src = src;
    L->s = src->base;
    L->n = src->len;
    L->eof_ok = eof_ok;
}

static void lx_bad(lex_t* L, const char* msg) {
    if (L->err) return;
    L->err = P_BAD;
    snprintf(L->emsg, sizeof L->emsg, "%s", msg);
}

// The text ended in the middle of something.
static void lx_eof(lex_t* L) {
    if (L->err) return;
    if (!L->eof_ok) L->err = P_MORE;
    else lx_bad(L, "syntax error: unexpected end of file");
}

static void scan_dollar(lex_t* L, int dq);
static node_t* p_clist(lex_t* L, int empty_ok);
static int tok(lex_t* L);

static void scan_bq(lex_t* L) {  // at the opening backquote
    const char* s = L->s;
    int i = L->i + 1;
    for (;;) {
        if (i >= L->n) { L->i = L->n; lx_eof(L); return; }
        if (s[i] == '\\') { i += 2; continue; }
        if (s[i] == '`') { L->i = i + 1; return; }
        i++;
    }
}

static void scan_dq(lex_t* L) {  // after the opening quote
    const char* s = L->s;
    while (!L->err) {
        if (L->i >= L->n) { lx_eof(L); return; }
        char c = s[L->i];
        if (c == '"') { L->i++; return; }
        if (c == '\\') {
            if (L->i + 1 >= L->n) { L->i = L->n; lx_eof(L); return; }
            L->i += 2;
        } else if (c == '`') {
            scan_bq(L);
        } else if (c == '$') {
            scan_dollar(L, 1);
        } else {
            L->i++;
        }
    }
}

static void scan_arith(lex_t* L) {  // at $((
    const char* s = L->s;
    int i = L->i + 3, depth = 0;
    for (;;) {
        if (i >= L->n) { L->i = L->n; lx_eof(L); return; }
        char c = s[i];
        if (c == '\\') { i += 2; continue; }
        if (c == '(') depth++;
        if (c == ')') {
            if (depth == 0) {
                if (i + 1 >= L->n) { L->i = L->n; lx_eof(L); return; }
                if (s[i + 1] != ')') { lx_bad(L, "syntax error: unbalanced ( ) in $(( ))"); return; }
                L->i = i + 2;
                return;
            }
            depth--;
        }
        i++;
    }
}

// $( ): the commands inside are parsed to find its end.
static void scan_cmdsub(lex_t* L) {
    if (L->depth >= MAX_DEPTH) { lx_bad(L, "commands nested too deeply"); return; }
    lex_t save = *L;
    L->i += 2;
    L->have = 0;
    L->npend = 0;
    L->depth++;
    p_clist(L, 1);
    int end = L->n;
    if (!L->err) {
        if (tok(L) == T_RPAR) end = L->te;
        else if (tok(L) == T_EOF) lx_eof(L);
        else lx_bad(L, "syntax error: no ) for $(");
    }
    int err = L->err;
    char msg[sizeof L->emsg];
    memcpy(msg, L->emsg, sizeof msg);
    *L = save;
    L->i = end;
    if (err) {
        L->err = err;
        memcpy(L->emsg, msg, sizeof msg);
    }
}

static void scan_brace(lex_t* L, int dq) {  // at ${
    const char* s = L->s;
    L->i += 2;
    while (!L->err) {
        if (L->i >= L->n) { lx_eof(L); return; }
        char c = s[L->i];
        if (c == '}') { L->i++; return; }
        if (c == '\\') {
            if (L->i + 1 >= L->n) { L->i = L->n; lx_eof(L); return; }
            L->i += 2;
        } else if (c == '\'' && !dq) {
            int j = L->i + 1;
            while (j < L->n && s[j] != '\'') j++;
            if (j >= L->n) { L->i = L->n; lx_eof(L); return; }
            L->i = j + 1;
        } else if (c == '"') {
            L->i++;
            scan_dq(L);
        } else if (c == '`') {
            scan_bq(L);
        } else if (c == '$') {
            scan_dollar(L, dq);
        } else {
            L->i++;
        }
    }
}

static void scan_dollar(lex_t* L, int dq) {  // at $
    const char* s = L->s;
    if (L->i + 1 < L->n && s[L->i + 1] == '(') {
        if (L->i + 2 < L->n && s[L->i + 2] == '(') scan_arith(L);
        else scan_cmdsub(L);
    } else if (L->i + 1 < L->n && s[L->i + 1] == '{') {
        scan_brace(L, dq);
    } else {
        L->i++;
    }
}

static int is_meta(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == ';' || c == '&' || c == '|' || c == '<' || c == '>' || c == '(' || c == ')';
}

static void scan_word(lex_t* L) {
    const char* s = L->s;
    while (L->i < L->n && !L->err) {
        char c = s[L->i];
        if (is_meta(c)) break;
        if (c == '\\') {
            if (L->i + 2 == L->n && s[L->i + 1] == '\n' && !L->eof_ok) {  // the line goes on
                L->i = L->n;
                lx_eof(L);
            } else if (L->i + 1 < L->n) L->i += 2;
            else if (L->eof_ok) L->i++;
            else lx_eof(L);
        } else if (c == '\'') {
            int j = L->i + 1;
            while (j < L->n && s[j] != '\'') j++;
            if (j >= L->n) { L->i = L->n; lx_eof(L); return; }
            L->i = j + 1;
        } else if (c == '"') {
            L->i++;
            scan_dq(L);
        } else if (c == '`') {
            scan_bq(L);
        } else if (c == '$') {
            scan_dollar(L, 0);
        } else {
            L->i++;
        }
    }
}

// The here-documents whose line just ended.
static void lx_heredocs(lex_t* L) {
    const char* s = L->s;
    for (int k = 0; k < L->npend; k++) {
        redir_t* r = L->pend[k];
        int bs = L->i, i = L->i;
        for (;;) {
            int le = i;
            while (le < L->n && s[le] != '\n') le++;
            if (le >= L->n && !L->eof_ok) { L->err = P_MORE; return; }
            int ls = i;
            if (r->strip)
                while (ls < le && s[ls] == '\t') ls++;
            if (le - ls == r->dn && memcmp(s + ls, r->dp, (size_t)r->dn) == 0) {
                r->body = s + bs;
                r->blen = i - bs;
                i = le < L->n ? le + 1 : L->n;
                break;
            }
            if (le >= L->n) {  // the end of the text ends it too
                r->body = s + bs;
                r->blen = L->n - bs;
                i = L->n;
                break;
            }
            i = le + 1;
        }
        hdrec_t* h = aalloc(sizeof *h);
        h->op_at = r->op_at;
        h->bs = bs;
        h->be = i;
        *L->src->hdtail = h;
        L->src->hdtail = &h->next;
        L->i = i;
    }
    L->npend = 0;
}

static void lx_scan(lex_t* L) {
    const char* s = L->s;
    int n = L->n;
    if (L->err) {
        L->tok = T_EOF;
        L->ts = L->te = L->i;
        return;
    }
    for (;;) {
        while (L->i < n && (s[L->i] == ' ' || s[L->i] == '\t')) L->i++;
        if (L->i + 1 < n && s[L->i] == '\\' && s[L->i + 1] == '\n') {
            L->i += 2;
            if (L->i >= n && !L->eof_ok) {
                L->err = P_MORE;
                L->tok = T_EOF;
                L->ts = L->te = L->i;
                return;
            }
            continue;
        }
        if (L->i < n && s[L->i] == '#') {
            while (L->i < n && s[L->i] != '\n') L->i++;
            continue;
        }
        break;
    }
    L->ts = L->i;
    if (L->i >= n) {
        if (L->npend) {
            if (!L->eof_ok) L->err = P_MORE;
            for (int k = 0; k < L->npend; k++) {
                L->pend[k]->body = s + n;
                L->pend[k]->blen = 0;
            }
            L->npend = 0;
        }
        L->tok = T_EOF;
        L->te = L->i;
        return;
    }
    char c = s[L->i];
    char d = L->i + 1 < n ? s[L->i + 1] : 0;
    char e = L->i + 2 < n ? s[L->i + 2] : 0;
    int t = -1, len = 1;
    switch (c) {
    case '\n': t = T_NL; break;
    case ';': if (d == ';') t = T_DSEMI, len = 2; else t = T_SEMI; break;
    case '&': if (d == '&') t = T_AND, len = 2; else t = T_AMP; break;
    case '|': if (d == '|') t = T_OR, len = 2; else t = T_PIPE; break;
    case '(': t = T_LPAR; break;
    case ')': t = T_RPAR; break;
    case '<':
        if (d == '<' && e == '-') t = T_DLESSDASH, len = 3;
        else if (d == '<') t = T_DLESS, len = 2;
        else if (d == '&') t = T_LESSAND, len = 2;
        else if (d == '>') t = T_LESSGREAT, len = 2;
        else t = T_LESS;
        break;
    case '>':
        if (d == '>') t = T_DGREAT, len = 2;
        else if (d == '&') t = T_GREATAND, len = 2;
        else if (d == '|') t = T_CLOBBER, len = 2;
        else t = T_GREAT;
        break;
    }
    if (t >= 0) {
        L->i += len;
        L->tok = t;
        L->te = L->i;
        if (t == T_NL && L->npend) lx_heredocs(L);
        return;
    }
    scan_word(L);
    L->te = L->i;
    L->tok = T_WORD;
    int digits = 1;
    for (int k = L->ts; k < L->te; k++) digits &= is_digit((unsigned char)s[k]);
    if (digits && L->i < n && (s[L->i] == '<' || s[L->i] == '>')) L->tok = T_IONUM;
    if (L->err) L->tok = T_EOF;
}

static int tok(lex_t* L) {
    if (!L->have) {
        lx_scan(L);
        L->have = 1;
    }
    return L->tok;
}

static void adv(lex_t* L) {
    tok(L);
    if (L->tok != T_NL && L->tok != T_EOF) L->pe = L->te;
    L->have = 0;
}

// ---- Reading commands: the grammar ----

static node_t* nnew(lex_t* L, int kind, int s) {
    node_t* x = aalloc(sizeof *x);
    x->kind = kind;
    x->s = x->e = s;
    x->src = L->src;
    return x;
}

static int is_word(lex_t* L, const char* w) {
    return tok(L) == T_WORD && L->te - L->ts == (int)strlen(w) && memcmp(L->s + L->ts, w, (size_t)(L->te - L->ts)) == 0;
}

static int is_term(lex_t* L) {
    int t = tok(L);
    if (t == T_RPAR || t == T_DSEMI || t == T_EOF) return 1;
    static const char* const words[] = { "then", "else", "elif", "fi", "do", "done", "esac", "}" };
    for (size_t i = 0; i < sizeof words / sizeof *words; i++)
        if (is_word(L, words[i])) return 1;
    return 0;
}

// A syntax error at the token in hand.
static void near(lex_t* L) {
    if (L->err) return;
    if (tok(L) == T_EOF) { lx_eof(L); return; }
    char t[48];
    if (L->tok == T_NL) {
        snprintf(t, sizeof t, "newline");
    } else {
        int n = L->te - L->ts;
        if (n > 40) n = 40;
        memcpy(t, L->s + L->ts, (size_t)n);
        t[n] = 0;
    }
    char m[100];
    snprintf(m, sizeof m, "syntax error near unexpected token '%s'", t);
    lx_bad(L, m);
}

static void skip_nl(lex_t* L) {
    while (tok(L) == T_NL) adv(L);
}

static int expect(lex_t* L, const char* w) {
    if (is_word(L, w)) {
        adv(L);
        return 1;
    }
    near(L);
    return 0;
}

static int is_redir(int t) { return t == T_IONUM || (t >= T_LESS && t <= T_CLOBBER); }

static word_t* wnew(lex_t* L) {
    word_t* w = aalloc(sizeof *w);
    w->p = L->s + L->ts;
    w->n = L->te - L->ts;
    return w;
}

// A here-document's delimiter, with its quotes taken off.
static void hd_delim(redir_t* r) {
    char* d = aalloc((size_t)r->n + 1);
    int k = 0;
    for (int i = 0; i < r->n; i++) {
        char c = r->p[i];
        if (c == '\\' && i + 1 < r->n) {
            d[k++] = r->p[++i];
            r->quoted = 1;
        } else if (c == '\'') {
            r->quoted = 1;
            while (++i < r->n && r->p[i] != '\'') d[k++] = r->p[i];
        } else if (c == '"') {
            r->quoted = 1;
            while (++i < r->n && r->p[i] != '"') {
                if (r->p[i] == '\\' && i + 1 < r->n && strchr("$`\"\\", r->p[i + 1])) i++;
                d[k++] = r->p[i];
            }
        } else {
            d[k++] = c;
        }
    }
    r->dp = d;
    r->dn = k;
}

static int p_redirect(lex_t* L, redir_t*** tail) {
    int fd = -1, op_at = (tok(L), L->ts);
    if (L->tok == T_IONUM) {
        if (L->te - L->ts > 1) { lx_bad(L, "only descriptors 0 to 9 can be redirected"); return 0; }
        fd = L->s[L->ts] - '0';
        adv(L);
    }
    int op = tok(L);
    adv(L);
    if (tok(L) != T_WORD) { near(L); return 0; }
    redir_t* r = aalloc(sizeof *r);
    r->op = op;
    r->op_at = op_at;
    r->fd = fd >= 0 ? fd : (op == T_LESS || op == T_LESSAND || op == T_LESSGREAT || op == T_DLESS || op == T_DLESSDASH) ? 0 : 1;
    r->p = L->s + L->ts;
    r->n = L->te - L->ts;
    adv(L);
    if (op == T_DLESS || op == T_DLESSDASH) {
        r->strip = op == T_DLESSDASH;
        hd_delim(r);
        if (L->npend == (int)(sizeof L->pend / sizeof *L->pend)) { lx_bad(L, "too many here-documents on one line"); return 0; }
        L->pend[L->npend++] = r;
    }
    **tail = r;
    *tail = &r->next;
    return 1;
}

static node_t* p_andor(lex_t* L);
static node_t* p_command(lex_t* L);

// A list of commands ended by a word like "fi" or "done", by ) or ;; or the end.
static node_t* p_clist(lex_t* L, int empty_ok) {
    skip_nl(L);
    node_t* list = nnew(L, N_LIST, L->ts);
    node_t** tail = &list->kids;
    while (!L->err && !is_term(L)) {
        node_t* a = p_andor(L);
        if (!a) return 0;
        if (!list->kids) list->s = a->s;
        *tail = a;
        tail = &a->next;
        list->e = a->e;
        int t = tok(L);
        if (t == T_SEMI || t == T_AMP) {
            a->bg = t == T_AMP;
            adv(L);
            list->e = L->pe;
        } else if (t != T_NL && !is_term(L)) {
            near(L);
            return 0;
        }
        skip_nl(L);
    }
    if (L->err) return 0;
    if (!list->kids && !empty_ok) {
        near(L);
        return 0;
    }
    return list;
}

// One line's commands at the top: 0 at the end of the text (or an error).
static node_t* p_complete(lex_t* L) {
    skip_nl(L);
    if (tok(L) == T_EOF) return 0;
    node_t* list = nnew(L, N_LIST, L->ts);
    node_t** tail = &list->kids;
    for (;;) {
        node_t* a = p_andor(L);
        if (!a) return 0;
        *tail = a;
        tail = &a->next;
        list->e = a->e;
        int t = tok(L);
        if (t == T_SEMI || t == T_AMP) {
            a->bg = t == T_AMP;
            adv(L);
            list->e = L->pe;
            t = tok(L);
            if (t == T_NL) { adv(L); break; }
            if (t == T_EOF) break;
            continue;
        }
        if (t == T_NL) { adv(L); break; }
        if (t == T_EOF) break;
        near(L);
        return 0;
    }
    return L->err ? 0 : list;
}

static node_t* p_pipeline(lex_t* L) {
    int s = (tok(L), L->ts), bang = 0;
    if (is_word(L, "!")) {
        bang = 1;
        adv(L);
    }
    node_t* c = p_command(L);
    if (!c) return 0;
    if (tok(L) != T_PIPE && !bang) return c;
    node_t* p = nnew(L, N_PIPE, s);
    p->bang = bang;
    p->kids = c;
    node_t** tail = &c->next;
    while (tok(L) == T_PIPE) {
        adv(L);
        skip_nl(L);
        c = p_command(L);
        if (!c) return 0;
        *tail = c;
        tail = &c->next;
    }
    p->e = c->e;
    return p;
}

static node_t* p_andor(lex_t* L) {
    node_t* a = p_pipeline(L);
    if (!a) return 0;
    for (;;) {
        int t = tok(L);
        if (t != T_AND && t != T_OR) return a;
        adv(L);
        skip_nl(L);
        node_t* b = p_pipeline(L);
        if (!b) return 0;
        node_t* x = nnew(L, t == T_AND ? N_AND : N_OR, a->s);
        x->a = a;
        x->b = b;
        x->e = b->e;
        a = x;
    }
}

static node_t* p_if(lex_t* L) {  // at "if" or "elif"
    node_t* x = nnew(L, N_IF, L->ts);
    adv(L);
    if (!(x->a = p_clist(L, 0))) return 0;
    if (!expect(L, "then")) return 0;
    if (!(x->b = p_clist(L, 0))) return 0;
    if (is_word(L, "elif")) {
        if (!(x->c = p_if(L))) return 0;
        x->e = x->c->e;
        return x;
    }
    if (is_word(L, "else")) {
        adv(L);
        if (!(x->c = p_clist(L, 0))) return 0;
    }
    if (!expect(L, "fi")) return 0;
    x->e = L->pe;
    return x;
}

static node_t* p_for(lex_t* L) {
    node_t* x = nnew(L, N_FOR, L->ts);
    adv(L);
    if (tok(L) != T_WORD || !is_name(L->s + L->ts, L->te - L->ts)) { near(L); return 0; }
    x->name = L->s + L->ts;
    x->nlen = L->te - L->ts;
    adv(L);
    skip_nl(L);
    if (is_word(L, "in")) {
        adv(L);
        x->has_in = 1;
        word_t** tail = &x->words;
        while (tok(L) == T_WORD) {
            *tail = wnew(L);
            tail = &(*tail)->next;
            adv(L);
        }
        if (tok(L) == T_SEMI) adv(L);
        else if (tok(L) != T_NL) { near(L); return 0; }
    } else if (tok(L) == T_SEMI) {
        adv(L);
    }
    skip_nl(L);
    if (!expect(L, "do")) return 0;
    if (!(x->b = p_clist(L, 0))) return 0;
    if (!expect(L, "done")) return 0;
    x->e = L->pe;
    return x;
}

static node_t* p_case(lex_t* L) {
    node_t* x = nnew(L, N_CASE, L->ts);
    adv(L);
    if (tok(L) != T_WORD) { near(L); return 0; }
    x->words = wnew(L);
    adv(L);
    skip_nl(L);
    if (!expect(L, "in")) return 0;
    skip_nl(L);
    citem_t** tail = &x->items;
    while (!is_word(L, "esac")) {
        if (L->err) return 0;
        citem_t* it = aalloc(sizeof *it);
        if (tok(L) == T_LPAR) adv(L);
        word_t** pt = &it->pats;
        for (;;) {
            if (tok(L) != T_WORD) { near(L); return 0; }
            *pt = wnew(L);
            pt = &(*pt)->next;
            adv(L);
            if (tok(L) != T_PIPE) break;
            adv(L);
        }
        if (tok(L) != T_RPAR) { near(L); return 0; }
        adv(L);
        skip_nl(L);
        if (tok(L) != T_DSEMI && !is_word(L, "esac") && !(it->body = p_clist(L, 0))) return 0;
        *tail = it;
        tail = &it->next;
        if (tok(L) == T_DSEMI) {
            adv(L);
            skip_nl(L);
        } else if (!is_word(L, "esac")) {
            near(L);
            return 0;
        }
    }
    adv(L);
    x->e = L->pe;
    return x;
}

static int is_funcname(const char* s, int n) {
    int digits = 1;
    for (int i = 0; i < n; i++) {
        int c = (unsigned char)s[i];
        if (!is_namec(c) && c != '-' && c != '.') return 0;
        digits &= is_digit(c);
    }
    return n > 0 && !digits;
}

static node_t* p_simple(lex_t* L) {
    node_t* x = nnew(L, N_SIMPLE, (tok(L), L->ts));
    word_t** at = &x->assigns, **wt = &x->words;
    redir_t** rt = &x->redirs;
    for (;;) {
        int t = tok(L);
        if (is_redir(t)) {
            if (!p_redirect(L, &rt)) return 0;
            continue;
        }
        if (t != T_WORD) break;
        word_t* w = wnew(L);
        int eq = 0;
        while (eq < w->n && w->p[eq] != '=') eq++;
        if (!x->words && eq < w->n && is_name(w->p, eq)) {
            *at = w;
            at = &w->next;
        } else {
            *wt = w;
            wt = &w->next;
        }
        adv(L);
    }
    if (!x->words && !x->assigns && !x->redirs) {
        near(L);
        return 0;
    }
    x->e = L->pe;
    return x;
}

static node_t* p_command(lex_t* L) {
    if (L->depth >= MAX_DEPTH) { lx_bad(L, "commands nested too deeply"); return 0; }
    L->depth++;
    int t = tok(L), s = L->ts;
    node_t* x = 0;
    if (t == T_LPAR) {
        adv(L);
        x = nnew(L, N_SUBSHELL, s);
        if (!(x->a = p_clist(L, 0))) goto fail;
        if (tok(L) != T_RPAR) { near(L); goto fail; }
        adv(L);
    } else if (is_word(L, "{")) {
        adv(L);
        x = nnew(L, N_BRACE, s);
        if (!(x->a = p_clist(L, 0))) goto fail;
        if (!expect(L, "}")) goto fail;
    } else if (is_word(L, "if")) {
        if (!(x = p_if(L))) goto fail;
    } else if (is_word(L, "while") || is_word(L, "until")) {
        x = nnew(L, is_word(L, "while") ? N_WHILE : N_UNTIL, s);
        adv(L);
        if (!(x->a = p_clist(L, 0))) goto fail;
        if (!expect(L, "do")) goto fail;
        if (!(x->b = p_clist(L, 0))) goto fail;
        if (!expect(L, "done")) goto fail;
    } else if (is_word(L, "for")) {
        if (!(x = p_for(L))) goto fail;
    } else if (is_word(L, "case")) {
        if (!(x = p_case(L))) goto fail;
    } else if (is_term(L) && t == T_WORD) {
        near(L);
        goto fail;
    } else {
        int j = L->i;
        if (t == T_WORD) {
            while (j < L->n && (L->s[j] == ' ' || L->s[j] == '\t')) j++;
        }
        if (t == T_WORD && j < L->n && L->s[j] == '(' && is_funcname(L->s + L->ts, L->te - L->ts)) {
            x = nnew(L, N_FUNC, s);
            x->name = L->s + L->ts;
            x->nlen = L->te - L->ts;
            adv(L);
            adv(L);  // (
            if (tok(L) != T_RPAR) { near(L); goto fail; }
            adv(L);
            skip_nl(L);
            int bt = tok(L);
            if (!(bt == T_LPAR || is_word(L, "{") || is_word(L, "if") || is_word(L, "while") || is_word(L, "until") ||
                  is_word(L, "for") || is_word(L, "case"))) {
                near(L);
                goto fail;
            }
            if (!(x->a = p_command(L))) goto fail;
            x->e = x->a->e;
            L->depth--;
            return x;
        }
        L->depth--;
        return p_simple(L);
    }
    {
        redir_t** rt = &x->redirs;
        while (is_redir(tok(L)))
            if (!p_redirect(L, &rt)) goto fail;
    }
    x->e = L->pe;
    L->depth--;
    return x;
fail:
    L->depth--;
    return 0;
}

// The text of x for a shell of its own: its span, with the
// here-documents it uses (whose text may follow the line it ends on),
// and without those of commands around it.
static char* rebuild(node_t* x, int* len) {
    src_t* src = x->src;
    const char* b = src->base;
    str_t out = { 0, 0, 0 };
    int s = x->s, e = x->e, i = s;
    for (hdrec_t* h = src->hdocs; h; h = h->next) {
        if (h->op_at >= s && h->op_at < e) continue;
        if (h->be <= i || h->bs >= e) continue;
        if (h->bs > i) sput(&out, b + i, h->bs - i);
        i = h->be;
    }
    if (i < e) sput(&out, b + i, e - i);
    int nl = 0;
    for (hdrec_t* h = src->hdocs; h; h = h->next) {
        if (h->op_at < s || h->op_at >= e || h->bs < e) continue;
        if (!nl) sputc(&out, '\n');
        nl = 1;
        sput(&out, b + h->bs, h->be - h->bs);
    }
    sputc(&out, '\n');
    *len = out.n;
    return out.s;
}

// ---- Expansion ----

// Where each character of an expanded word came from.
enum {
    M_LIT,     // typed, not quoted
    M_Q,       // quoted (or from an expansion in quotes)
    M_EXP,     // from an expansion outside quotes: split, and wildcards
    M_BRK,     // (no character) a break between fields: "$@"
    M_SOFT,    // (no character) a break if the field has something: $@
    M_EMPTYQ,  // (no character) quotes were here: the field is there even if empty
};
typedef struct {
    char* c;
    unsigned char* m;
    int n, cap;
    int atzero;  // a "$@" with no parameters was here
} xbuf_t;

static void xput(xbuf_t* x, char c, int m) {
    if (x->n + 1 > x->cap) {
        x->cap = x->cap * 2 + 64;
        x->c = xrealloc(x->c, (size_t)x->cap);
        x->m = xrealloc(x->m, (size_t)x->cap);
    }
    x->c[x->n] = c;
    x->m[x->n++] = (unsigned char)m;
}

static void xputs(xbuf_t* x, const char* s, int n, int m) {
    for (int i = 0; i < n; i++) xput(x, s[i], m);
}

static void xfree(xbuf_t* x) {
    free(x->c);
    free(x->m);
    memset(x, 0, sizeof *x);
}

// The word as one string (in the arena): no splitting, no wildcards.
static char* xflat(xbuf_t* x, int* len) {
    char* d = aalloc((size_t)x->n + 1);
    int k = 0;
    for (int i = 0; i < x->n; i++) {
        int m = x->m[i];
        if (m <= M_EXP) d[k++] = x->c[i];
        else if (m == M_BRK || m == M_SOFT) d[k++] = ' ';
    }
    d[k] = 0;
    if (len) *len = k;
    return d;
}

// How the text being expanded is quoted.
enum {
    C_UNQ,    // a word
    C_DQ,     // inside "..."
    C_HD,     // a here-document: like "..." but a " is just a "
    C_ARITH,  // inside $(( )): like a here-document, and quotes go
    C_DQW,    // the word of ${x-word} inside "...": quotes inside it quote
};

static void ex_seq(xbuf_t* x, const char* p, int n, int ctx);
static int ex_dollar(xbuf_t* x, const char* p, int i, int n, int q);
static int ex_bq(xbuf_t* x, const char* p, int i, int n, int q);
static void run_capture(const char* text, int n, str_t* out);
static int arith_eval(const char* s, int64_t* out);

// The end of the $..., ${...}, $(...) or $((...)) at p[i].
static int raw_end(const char* p, int i, int n, int dq) {
    src_t src = { p, n, 0, 0 };
    src.hdtail = &src.hdocs;
    lex_t L;
    lx_init(&L, &src, 1);
    L.i = i;
    amark_t m = amark();
    scan_dollar(&L, dq);
    arelease(m);
    return L.err ? n : L.i;
}

static int ex_dq(xbuf_t* x, const char* p, int i, int n) {  // after the opening quote
    int start = x->n, az = x->atzero;
    x->atzero = 0;
    while (i < n && p[i] != '"' && !S.ctl) {
        char c = p[i];
        if (c == '\\' && i + 1 < n) {
            char d = p[i + 1];
            if (d == '\n') { i += 2; continue; }
            if (d == '$' || d == '`' || d == '\\' || d == '"') {
                xput(x, d, M_Q);
                i += 2;
                continue;
            }
            xput(x, c, M_Q);
            i++;
        } else if (c == '$') {
            i = ex_dollar(x, p, i, n, 1);
        } else if (c == '`') {
            i = ex_bq(x, p, i, n, 1);
        } else {
            xput(x, c, M_Q);
            i++;
        }
    }
    if (x->n == start && !x->atzero) xput(x, 0, M_EMPTYQ);
    x->atzero = az;
    return i < n ? i + 1 : n;
}

static void ex_seq(xbuf_t* x, const char* p, int n, int ctx) {
    int i = 0;
    int vm = ctx == C_UNQ ? M_LIT : M_Q;
    while (i < n && !S.ctl) {
        char c = p[i];
        if (c == '\\') {
            if (i + 1 >= n) { xput(x, c, vm); i++; continue; }
            char d = p[i + 1];
            if (d == '\n') { i += 2; continue; }
            if (ctx == C_UNQ || d == '$' || d == '`' || d == '\\' || (d == '"' && ctx == C_DQW)) {
                xput(x, d, M_Q);
                i += 2;
                continue;
            }
            xput(x, c, M_Q);
            i++;
        } else if (c == '\'' && ctx == C_UNQ) {
            int j = i + 1;
            while (j < n && p[j] != '\'') j++;
            if (j == i + 1) xput(x, 0, M_EMPTYQ);
            else xputs(x, p + i + 1, j - i - 1, M_Q);
            i = j < n ? j + 1 : n;
        } else if (c == '"' && (ctx == C_UNQ || ctx == C_DQW)) {
            i = ex_dq(x, p, i + 1, n);
        } else if (c == '"' && ctx == C_ARITH) {
            i++;
        } else if (c == '$') {
            i = ex_dollar(x, p, i, n, ctx != C_UNQ);
        } else if (c == '`') {
            i = ex_bq(x, p, i, n, ctx != C_UNQ);
        } else {
            xput(x, c, vm);
            i++;
        }
    }
}

// A word expanded as one string, in the arena: no splitting or wildcards.
static char* expand_str(const char* p, int n, int ctx, int* len) {
    xbuf_t x = { 0 };
    ex_seq(&x, p, n, ctx);
    char* s = xflat(&x, len);
    xfree(&x);
    return s;
}

static void put_val(xbuf_t* x, const char* s, int n, int q) { xputs(x, s, n, q ? M_Q : M_EXP); }

// $- : the options that are set.
static char* opts_string(char* buf) {
    int k = 0;
    if (S.opt_e) buf[k++] = 'e';
    if (S.opt_f) buf[k++] = 'f';
    if (S.interactive) buf[k++] = 'i';
    if (S.opt_u) buf[k++] = 'u';
    if (S.opt_x) buf[k++] = 'x';
    buf[k] = 0;
    return buf;
}

// The value of parameter name[0..n): a variable, a positional parameter
// or one of the special ones (not @ or *). 0 if it is not set.
static const char* getparam(const char* name, int n, char* buf) {
    if (is_digit((unsigned char)name[0])) {
        int64_t k = 0;
        for (int i = 0; i < n; i++) {
            k = k * 10 + (name[i] - '0');
            if (k > 1000000) return 0;
        }
        if (k == 0) return S.arg0 ? S.arg0 : "sh";
        return k <= S.pc ? S.pv[k - 1] : 0;
    }
    if (n == 1) {
        switch (name[0]) {
        case '#': return itoa64(S.pc, buf);
        case '?': return itoa64(S.last, buf);
        case '$': return itoa64(S.pid, buf);
        case '!': return S.bg ? itoa64(S.bg, buf) : 0;
        case '-': return opts_string(buf);
        }
    }
    var_t* v = vfind(name, n);
    return v ? v->val : 0;
}

static void unset_error(const char* name, int n) {
    char nb[64];
    if (n > 60) n = 60;
    memcpy(nb, name, (size_t)n);
    nb[n] = 0;
    say("%s: parameter not set", nb);
    sh_abort(2);
}

static void ex_at(xbuf_t* x, char which, int q) {
    if (q && which == '@') {
        if (S.pc == 0) x->atzero = 1;
        for (int k = 0; k < S.pc; k++) {
            xput(x, 0, k ? M_BRK : M_EMPTYQ);
            xputs(x, S.pv[k], (int)strlen(S.pv[k]), M_Q);
        }
    } else if (q) {
        const char* f = ifs();
        for (int k = 0; k < S.pc; k++) {
            if (k && *f) xput(x, *f, M_Q);
            xputs(x, S.pv[k], (int)strlen(S.pv[k]), M_Q);
        }
    } else {
        for (int k = 0; k < S.pc; k++) {
            if (k) xput(x, 0, M_SOFT);
            xputs(x, S.pv[k], (int)strlen(S.pv[k]), M_EXP);
        }
    }
}

static void ex_param(xbuf_t* x, const char* name, int n, int q) {
    if (n == 1 && (name[0] == '@' || name[0] == '*')) {
        ex_at(x, name[0], q);
        return;
    }
    char buf[24];
    const char* v = getparam(name, n, buf);
    if (!v) {
        if (S.opt_u) unset_error(name, n);
        return;
    }
    put_val(x, v, (int)strlen(v), q);
}

// The positional parameters joined with spaces (for ${@...}), in the arena.
static char* joined_params(void) {
    str_t b = { 0, 0, 0 };
    sput(&b, "", 0);
    for (int k = 0; k < S.pc; k++) {
        if (k) sputc(&b, ' ');
        sputs(&b, S.pv[k]);
    }
    char* s = adup(b.s, b.n);
    free(b.s);
    return s;
}

static int pmatch(const char* p, const unsigned char* lit, int pn, const char* s, int sn, int fold);

// A pattern expanded: its characters, and which are just themselves.
static void ex_pattern(const char* p, int n, char** pc, unsigned char** pl, int* pn) {
    xbuf_t x = { 0 };
    ex_seq(&x, p, n, C_UNQ);
    char* c = aalloc((size_t)x.n + 1);
    unsigned char* l = aalloc((size_t)x.n + 1);
    int k = 0;
    for (int i = 0; i < x.n; i++) {
        int m = x.m[i];
        if (m == M_EMPTYQ) continue;
        if (m == M_BRK || m == M_SOFT) { c[k] = ' '; l[k++] = 1; continue; }
        c[k] = x.c[i];
        l[k++] = m == M_Q;
    }
    xfree(&x);
    *pc = c;
    *pl = l;
    *pn = k;
}

static void ex_brace(xbuf_t* x, const char* p, int n, int q) {  // the text inside ${ }
    int i = 0, length = 0;
    if (n > 1 && p[0] == '#') {
        int j = 1;
        if (is_name1((unsigned char)p[1])) {
            while (j < n && is_namec((unsigned char)p[j])) j++;
        } else if (is_digit((unsigned char)p[1])) {
            while (j < n && is_digit((unsigned char)p[j])) j++;
        } else if (strchr("@*#?-$!", p[1])) {
            j = 2;
        }
        if (j == n) {
            length = 1;
            i = 1;
        }
    }
    int ns = i;
    if (i < n && is_name1((unsigned char)p[i])) {
        while (i < n && is_namec((unsigned char)p[i])) i++;
    } else if (i < n && is_digit((unsigned char)p[i])) {
        while (i < n && is_digit((unsigned char)p[i])) i++;
    } else if (i < n && strchr("@*#?-$!", p[i])) {
        i++;
    }
    int nl = i - ns;
    const char* name = p + ns;
    if (nl == 0) goto bad;
    int at = nl == 1 && (name[0] == '@' || name[0] == '*');
    char buf[24];
    if (length) {
        if (at) {
            { itoa64(S.pc, buf); put_val(x, buf, (int)strlen(buf), q); }
            return;
        }
        const char* v = getparam(name, nl, buf);
        if (!v && S.opt_u) { unset_error(name, nl); return; }
        char nb[24];
        itoa64(v ? (int64_t)strlen(v) : 0, nb);
        put_val(x, nb, (int)strlen(nb), q);
        return;
    }
    if (i == n) {
        ex_param(x, name, nl, q);
        return;
    }
    int colon = 0;
    if (p[i] == ':') {
        colon = 1;
        i++;
        if (i >= n || !strchr("-=?+", p[i])) goto bad;
    }
    char op = p[i];
    int twice = 0;
    if (op == '%' || op == '#') {
        if (i + 1 < n && p[i + 1] == op) twice = 1;
        i += 1 + twice;
    } else if (op == '-' || op == '=' || op == '?' || op == '+') {
        i++;
    } else {
        goto bad;
    }
    const char* w = p + i;
    int wn = n - i;
    const char* v = at ? (S.pc ? joined_params() : 0) : getparam(name, nl, buf);
    int use_word = colon ? (!v || !*v) : !v;
    int wctx = q ? C_DQW : C_UNQ;
    switch (op) {
    case '-':
        if (use_word) ex_seq(x, w, wn, wctx);
        else if (at) ex_at(x, name[0], q);
        else put_val(x, v, (int)strlen(v), q);
        return;
    case '+':
        if (!use_word) ex_seq(x, w, wn, wctx);
        return;
    case '=':
        if (use_word) {
            if (!is_name(name, nl)) {
                char nb[64];
                int k = nl > 60 ? 60 : nl;
                memcpy(nb, name, (size_t)k);
                nb[k] = 0;
                say("%s: cannot assign in this way", nb);
                sh_abort(2);
                return;
            }
            int len;
            char* s = expand_str(w, wn, wctx, &len);
            vsetn(name, nl, s);
            put_val(x, s, len, q);
        } else if (at) {
            ex_at(x, name[0], q);
        } else {
            put_val(x, v, (int)strlen(v), q);
        }
        return;
    case '?':
        if (use_word) {
            char nb[64];
            int k = nl > 60 ? 60 : nl;
            memcpy(nb, name, (size_t)k);
            nb[k] = 0;
            const char* msg = wn ? expand_str(w, wn, wctx, 0) : colon ? "parameter null or not set" : "parameter not set";
            if (!S.ctl) say("%s: %s", nb, msg);
            sh_abort(2);
        } else if (at) {
            ex_at(x, name[0], q);
        } else {
            put_val(x, v, (int)strlen(v), q);
        }
        return;
    }
    // % %% # ##: the value without the shortest or longest match at its end or start
    if (!v) {
        if (S.opt_u && !at) { unset_error(name, nl); return; }
        v = "";
    }
    char* pc;
    unsigned char* pl;
    int pn;
    v = adup(v, strlen(v));  // the pattern may change the variable
    ex_pattern(w, wn, &pc, &pl, &pn);
    int vn = (int)strlen(v), from = 0, to = vn;
    if (op == '#') {
        for (int k = 0; k <= vn; k++) {
            int len = twice ? vn - k : k;
            if (pmatch(pc, pl, pn, v, len, 0)) { from = len; break; }
        }
    } else {
        for (int k = 0; k <= vn; k++) {
            int start = twice ? k : vn - k;
            if (pmatch(pc, pl, pn, v + start, vn - start, 0)) { to = start; break; }
        }
    }
    put_val(x, v + from, to - from, q);
    return;
bad:
    say("${%s}: bad substitution", adup(p, n));
    sh_abort(2);
}

// A command substitution's output, trailing newlines off, NULs dropped.
static void cmdsub(xbuf_t* x, const char* t, int n, int q, int bq) {
    char* text;
    if (bq) {  // a backslash before $ ` \ (and " in quotes) only quoted it
        text = aalloc((size_t)n + 1);
        int k = 0;
        for (int i = 0; i < n; i++) {
            if (t[i] == '\\' && i + 1 < n && (t[i + 1] == '$' || t[i + 1] == '`' || t[i + 1] == '\\' || (q && t[i + 1] == '"'))) i++;
            text[k++] = t[i];
        }
        n = k;
    } else {
        text = adup(t, n);
    }
    str_t out = { 0, 0, 0 };
    run_capture(text, n, &out);
    int len = out.n;
    while (len > 0 && out.s[len - 1] == '\n') len--;
    for (int i = 0; i < len; i++)
        if (out.s[i]) xput(x, out.s[i], q ? M_Q : M_EXP);
    free(out.s);
}

static int ex_bq(xbuf_t* x, const char* p, int i, int n, int q) {  // at the backquote
    int j = i + 1;
    while (j < n && p[j] != '`') j += p[j] == '\\' ? 2 : 1;
    if (j > n) j = n;
    cmdsub(x, p + i + 1, j - i - 1, q, 1);
    return j < n ? j + 1 : n;
}

static int ex_dollar(xbuf_t* x, const char* p, int i, int n, int q) {  // at the $
    if (i + 1 >= n) {
        xput(x, '$', q ? M_Q : M_LIT);
        return i + 1;
    }
    char c = p[i + 1];
    if (c == '(' || c == '{') {
        int end = raw_end(p, i, n, q);
        if (c == '{') {
            ex_brace(x, p + i + 2, end - 1 - (i + 2), q);
        } else if (i + 2 < n && p[i + 2] == '(') {
            int len;
            char* e = expand_str(p + i + 3, end - 2 - (i + 3), C_ARITH, &len);
            int64_t v;
            if (!S.ctl && arith_eval(e, &v) == 0) {
                char nb[24];
                { itoa64(v, nb); put_val(x, nb, (int)strlen(nb), q); }
            }
        } else {
            cmdsub(x, p + i + 2, end - 1 - (i + 2), q, 0);
        }
        return end;
    }
    if (is_name1((unsigned char)c)) {
        int j = i + 1;
        while (j < n && is_namec((unsigned char)p[j])) j++;
        ex_param(x, p + i + 1, j - i - 1, q);
        return j;
    }
    if (is_digit((unsigned char)c) || strchr("@*#?-$!", c)) {
        ex_param(x, p + i + 1, 1, q);
        return i + 2;
    }
    xput(x, '$', q ? M_Q : M_LIT);
    return i + 1;
}

// ---- Patterns ----

static int fold_c(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int in_class(const char* name, int n, int c) {
#define IS(s) (n == (int)sizeof(s) - 1 && memcmp(name, s, (size_t)n) == 0)
    if (IS("alpha")) return is_alpha(c);
    if (IS("digit")) return is_digit(c);
    if (IS("alnum")) return is_alpha(c) || is_digit(c);
    if (IS("upper")) return c >= 'A' && c <= 'Z';
    if (IS("lower")) return c >= 'a' && c <= 'z';
    if (IS("space")) return c == ' ' || (c >= '\t' && c <= '\r');
    if (IS("blank")) return c == ' ' || c == '\t';
    if (IS("punct")) return c > 32 && c < 127 && !is_alpha(c) && !is_digit(c);
    if (IS("print")) return c >= 32 && c < 127;
    if (IS("graph")) return c > 32 && c < 127;
    if (IS("cntrl")) return c < 32 || c == 127;
    if (IS("xdigit")) return is_digit(c) || (fold_c(c) >= 'a' && fold_c(c) <= 'f');
#undef IS
    return 0;
}

// The [...] at p: 1 if c is in it, 0 if not, -1 if it never closes
// (then the [ is just a [). *used: its length.
static int bracket(const char* p, const unsigned char* lit, int pn, int c, int* used, int fold) {
    int i = 1, neg = 0, hit = 0;
    if (i < pn && !lit[i] && (p[i] == '!' || p[i] == '^')) {
        neg = 1;
        i++;
    }
    int first = 1;
    while (i < pn) {
        if (p[i] == ']' && !lit[i] && !first) {
            *used = i + 1;
            return hit != neg;
        }
        first = 0;
        if (p[i] == '[' && !lit[i] && i + 1 < pn && p[i + 1] == ':') {
            int j = i + 2;
            while (j + 1 < pn && !(p[j] == ':' && p[j + 1] == ']')) j++;
            if (j + 1 < pn) {
                if (in_class(p + i + 2, j - i - 2, c)) hit = 1;
                i = j + 2;
                continue;
            }
        }
        int lo = (unsigned char)p[i], hi = lo;
        if (i + 2 < pn && p[i + 1] == '-' && !lit[i + 1] && !(p[i + 2] == ']' && !lit[i + 2])) {
            hi = (unsigned char)p[i + 2];
            i += 3;
        } else {
            i++;
        }
        if (c >= lo && c <= hi) hit = 1;
        if (fold && fold_c(c) >= fold_c(lo) && fold_c(c) <= fold_c(hi)) hit = 1;
    }
    return -1;
}

// Whether s[0..sn) matches the pattern p[0..pn); lit[i] says p[i] is just
// itself. fold: letters match in either case (as file names do here).
static int pmatch(const char* p, const unsigned char* lit, int pn, const char* s, int sn, int fold) {
    int pi = 0, si = 0, star_p = -1, star_s = 0;
    while (si < sn) {
        int ok = 0;
        if (pi < pn) {
            char c = p[pi];
            int sc = (unsigned char)s[si];
            if (!lit[pi] && c == '*') {
                star_p = ++pi;
                star_s = si;
                continue;
            }
            if (!lit[pi] && c == '?') {
                ok = 1;
                pi++;
            } else {
                int used = 0, r = -1;
                if (!lit[pi] && c == '[') r = bracket(p + pi, lit + pi, pn - pi, sc, &used, fold);
                if (r >= 0) {
                    ok = r;
                    pi += used;
                } else if ((unsigned char)c == sc || (fold && fold_c((unsigned char)c) == fold_c(sc))) {
                    ok = 1;
                    pi++;
                }
            }
        }
        if (ok) {
            si++;
            continue;
        }
        if (star_p < 0) return 0;
        pi = star_p;
        si = ++star_s;
    }
    while (pi < pn && !lit[pi] && p[pi] == '*') pi++;
    return pi == pn;
}

// ---- Fields and wildcards ----

#define PBUF 512

static int path_cmp(const char* a, const char* b) {
    for (;; a++, b++) {
        int x = (unsigned char)*a, y = (unsigned char)*b;
        if (shos_fold_names) x = fold_c(x), y = fold_c(y);
        if (x != y) return x - y;
        if (!*a) break;
    }
    return strcmp(a, b);
}

static void gwalk(char* path, int plen, const char* p, const unsigned char* lit, int pn, argv_t* out) {
    if (pn == 0) {
        av_add(out, adup(path, plen));
        return;
    }
    int k = 0, wild = 0;
    while (k < pn && p[k] != '/') {
        if (!lit[k] && (p[k] == '*' || p[k] == '?' || p[k] == '[')) wild = 1;
        k++;
    }
    int last = k == pn;
    if (plen + k + 2 >= PBUF) return;
    if (!wild) {
        memcpy(path + plen, p, (size_t)k);
        path[plen + k] = 0;
        if (last) {
            long size;
            if (shos_stat(path, &size) != SHS_NONE) av_add(out, adup(path, plen + k));
        } else {
            path[plen + k] = '/';
            gwalk(path, plen + k + 1, p + k + 1, lit + k + 1, pn - k - 1, out);
        }
        return;
    }
    char dir[PBUF];
    if (plen == 0) {
        dir[0] = '.';
        dir[1] = 0;
    } else {
        memcpy(dir, path, (size_t)plen);
        dir[plen > 1 ? plen - 1 : plen] = 0;
    }
    int cursor = 0, isdir;
    char name[PBUF];
    while (shos_readdir(dir, &cursor, name, sizeof name, &isdir) > 0) {
        int nl = (int)strlen(name);
        if (name[0] == '.' && (nl == 1 || (nl == 2 && name[1] == '.'))) continue;
        if (name[0] == '.' && p[0] != '.') continue;
        if (!pmatch(p, lit, k, name, nl, shos_fold_names)) continue;
        if (!last && !isdir) continue;
        if (plen + nl + 2 >= PBUF) continue;
        memcpy(path + plen, name, (size_t)nl);
        if (last) {
            av_add(out, adup(path, plen + nl));
        } else {
            path[plen + nl] = '/';
            gwalk(path, plen + nl + 1, p + k + 1, lit + k + 1, pn - k - 1, out);
        }
    }
}

// One field done: its wildcards matched against file names, or as it is.
static void emit(xbuf_t* f, argv_t* out) {
    int wild = 0;
    if (!S.opt_f)
        for (int i = 0; i < f->n; i++)
            if (f->m[i] != M_Q && (f->c[i] == '*' || f->c[i] == '?' || f->c[i] == '[')) wild = 1;
    if (wild) {
        unsigned char* lit = aalloc((size_t)f->n + 1);
        for (int i = 0; i < f->n; i++) lit[i] = f->m[i] == M_Q;
        char path[PBUF];
        int start = out->n;
        gwalk(path, 0, f->c, lit, f->n, out);
        if (out->n > start) {
            for (int i = start + 1; i < out->n; i++) {
                char* t = out->v[i];
                int j = i;
                while (j > start && path_cmp(out->v[j - 1], t) > 0) {
                    out->v[j] = out->v[j - 1];
                    j--;
                }
                out->v[j] = t;
            }
            f->n = 0;
            return;
        }
    }
    av_add(out, adup(f->c ? f->c : "", f->n));
    f->n = 0;
}

static int is_ifs_ws(char c) { return c == ' ' || c == '\t' || c == '\n'; }

// Splits an expanded word into fields at the characters of IFS that
// expansions gave.
static void x_fields(xbuf_t* x, argv_t* out) {
    const char* sep = ifs();
    xbuf_t f = { 0 };
    int have = 0, wsend = 0;
    for (int k = 0; k < x->n; k++) {
        char c = x->c[k];
        int m = x->m[k];
        if (m == M_EXP && c && strchr(sep, c)) {
            if (is_ifs_ws(c)) {
                if (have) {
                    emit(&f, out);
                    have = 0;
                    wsend = 1;
                }
            } else {
                if (have || !wsend) emit(&f, out);
                have = wsend = 0;
            }
            continue;
        }
        if (m == M_BRK) {
            emit(&f, out);
            have = 1;
            wsend = 0;
            continue;
        }
        if (m == M_SOFT) {
            if (have) {
                emit(&f, out);
                have = 0;
                wsend = 1;
            }
            continue;
        }
        if (m == M_EMPTYQ) {
            have = 1;
            wsend = 0;
            continue;
        }
        xput(&f, c, m);
        have = 1;
        wsend = 0;
    }
    if (have) emit(&f, out);
    xfree(&f);
}

// Nothing in the word needs expanding.
static int plain(const char* p, int n) {
    for (int i = 0; i < n; i++)
        if (strchr("\\'\"$`*?[", p[i])) return 0;
    return 1;
}

// The words expanded into fields. -1 if an error stopped it.
static int expand_words(word_t* w, argv_t* out) {
    for (; w; w = w->next) {
        if (plain(w->p, w->n)) {
            av_add(out, adup(w->p, w->n));
            continue;
        }
        xbuf_t x = { 0 };
        ex_seq(&x, w->p, w->n, C_UNQ);
        if (S.ctl) {
            xfree(&x);
            return -1;
        }
        x_fields(&x, out);
        xfree(&x);
    }
    return S.ctl ? -1 : 0;
}

// ---- Arithmetic ----

typedef struct {
    const char* s;
    int i, err, noeval, depth;
} ar_t;

static int64_t ar_assign(ar_t* a);

static void ar_ws(ar_t* a) {
    while (a->s[a->i] == ' ' || a->s[a->i] == '\t' || a->s[a->i] == '\n') a->i++;
}

static void ar_error(ar_t* a, const char* what) {
    if (a->err) return;
    a->err = 1;
    say("%s: %s", a->s, what);
}

// A number in C's way of writing them: 0x1f, 017, 15.
static int64_t ar_number(ar_t* a, const char* s, int* used) {
    uint64_t v = 0;
    int i = 0, base = 10;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        i = 2;
    } else if (s[0] == '0') {
        base = 8;
    }
    int start = i;
    for (;; i++) {
        int c = (unsigned char)s[i], d;
        if (is_digit(c)) d = c - '0';
        else if (fold_c(c) >= 'a' && fold_c(c) <= 'z') d = fold_c(c) - 'a' + 10;
        else if (c == '_') d = 99;
        else break;
        if (d >= base) {
            ar_error(a, "value too great for base");
            break;
        }
        v = v * (uint64_t)base + (uint64_t)d;
    }
    if (base == 16 && i == start) ar_error(a, "invalid number");
    *used = i;
    return (int64_t)v;
}

// A variable's value as a number (its value may itself be an expression).
static int64_t ar_var(ar_t* a, const char* name, int n) {
    var_t* v = vfind(name, n);
    if (!v || !*v->val) {
        if (!v && S.opt_u && !a->noeval) {
            unset_error(name, n);
            a->err = 1;
        }
        return 0;
    }
    if (a->depth > 32) {
        ar_error(a, "expression recursion level exceeded");
        return 0;
    }
    ar_t b = { v->val, 0, 0, a->noeval, a->depth + 1 };
    int64_t r = ar_assign(&b);
    ar_ws(&b);
    if (!b.err && b.s[b.i]) ar_error(&b, "arithmetic syntax error");
    if (b.err) a->err = 1;
    return r;
}

static void ar_store(ar_t* a, const char* name, int n, int64_t v) {
    if (a->noeval || a->err) return;
    char nb[24];
    vsetn(name, n, itoa64(v, nb));
}

static int64_t ar_unary(ar_t* a) {
    ar_ws(a);
    const char* s = a->s + a->i;
    if (a->err) return 0;
    if ((s[0] == '+' || s[0] == '-') && s[1] == s[0]) {  // ++x --x
        a->i += 2;
        ar_ws(a);
        int j = a->i;
        while (is_namec((unsigned char)a->s[j])) j++;
        if (j == a->i || is_digit((unsigned char)a->s[a->i])) {
            ar_error(a, "arithmetic syntax error");
            return 0;
        }
        int64_t v = ar_var(a, a->s + a->i, j - a->i);
        v = (int64_t)((uint64_t)v + (s[0] == '+' ? 1u : (uint64_t)-1));
        ar_store(a, a->s + a->i, j - a->i, v);
        a->i = j;
        return v;
    }
    switch (s[0]) {
    case '+': a->i++; return ar_unary(a);
    case '-': a->i++; return (int64_t)(0 - (uint64_t)ar_unary(a));
    case '!': a->i++; return !ar_unary(a);
    case '~': a->i++; return ~ar_unary(a);
    case '(': {
        a->i++;
        int64_t v = ar_assign(a);
        while (!a->err) {  // the comma operator
            ar_ws(a);
            if (a->s[a->i] != ',') break;
            a->i++;
            v = ar_assign(a);
        }
        ar_ws(a);
        if (a->s[a->i] != ')') {
            ar_error(a, "missing ')'");
            return 0;
        }
        a->i++;
        return v;
    }
    }
    if (is_digit((unsigned char)s[0])) {
        int used;
        int64_t v = ar_number(a, s, &used);
        a->i += used;
        return v;
    }
    if (is_name1((unsigned char)s[0])) {
        int j = a->i;
        while (is_namec((unsigned char)a->s[j])) j++;
        const char* name = a->s + a->i;
        int n = j - a->i;
        a->i = j;
        int64_t v = ar_var(a, name, n);
        ar_ws(a);
        const char* t = a->s + a->i;
        if ((t[0] == '+' || t[0] == '-') && t[1] == t[0]) {  // x++ x--
            a->i += 2;
            ar_store(a, name, n, (int64_t)((uint64_t)v + (t[0] == '+' ? 1u : (uint64_t)-1)));
        }
        return v;
    }
    ar_error(a, s[0] ? "arithmetic syntax error" : "operand expected");
    return 0;
}

enum { B_NONE, B_MUL, B_DIV, B_MOD, B_ADD, B_SUB, B_SHL, B_SHR, B_LT, B_LE, B_GT, B_GE, B_EQ, B_NE, B_AND, B_XOR, B_OR, B_LAND, B_LOR };

// The binary operator at a->i: its kind, precedence and length.
static int ar_binop(ar_t* a, int* prec, int* len) {
    ar_ws(a);
    const char* s = a->s + a->i;
    char c = s[0], d = s[0] ? s[1] : 0, e = d ? s[2] : 0;
    *len = 2;
    switch (c) {
    case '*': if (d == '=') return B_NONE; *prec = 10; *len = 1; return B_MUL;
    case '/': if (d == '=') return B_NONE; *prec = 10; *len = 1; return B_DIV;
    case '%': if (d == '=') return B_NONE; *prec = 10; *len = 1; return B_MOD;
    case '+': if (d == '=' || d == '+') return B_NONE; *prec = 9; *len = 1; return B_ADD;
    case '-': if (d == '=' || d == '-') return B_NONE; *prec = 9; *len = 1; return B_SUB;
    case '<':
        if (d == '<') { if (e == '=') return B_NONE; *prec = 8; return B_SHL; }
        *prec = 7;
        if (d == '=') return B_LE;
        *len = 1;
        return B_LT;
    case '>':
        if (d == '>') { if (e == '=') return B_NONE; *prec = 8; return B_SHR; }
        *prec = 7;
        if (d == '=') return B_GE;
        *len = 1;
        return B_GT;
    case '=': if (d != '=') return B_NONE; *prec = 6; return B_EQ;
    case '!': if (d != '=') return B_NONE; *prec = 6; return B_NE;
    case '&':
        if (d == '&') { *prec = 2; return B_LAND; }
        if (d == '=') return B_NONE;
        *prec = 5; *len = 1; return B_AND;
    case '^': if (d == '=') return B_NONE; *prec = 4; *len = 1; return B_XOR;
    case '|':
        if (d == '|') { *prec = 1; return B_LOR; }
        if (d == '=') return B_NONE;
        *prec = 3; *len = 1; return B_OR;
    }
    return B_NONE;
}

static int64_t ar_apply(ar_t* a, int op, int64_t l, int64_t r) {
    uint64_t ul = (uint64_t)l, ur = (uint64_t)r;
    switch (op) {
    case B_MUL: return (int64_t)(ul * ur);
    case B_DIV:
    case B_MOD:
        if (r == 0) {
            if (!a->noeval) ar_error(a, "division by zero");
            return 0;
        }
        if (l == INT64_MIN && r == -1) return op == B_DIV ? INT64_MIN : 0;
        return op == B_DIV ? l / r : l % r;
    case B_ADD: return (int64_t)(ul + ur);
    case B_SUB: return (int64_t)(ul - ur);
    case B_SHL: return (int64_t)(ul << (ur & 63));
    case B_SHR: return l >> (ur & 63);
    case B_LT: return l < r;
    case B_LE: return l <= r;
    case B_GT: return l > r;
    case B_GE: return l >= r;
    case B_EQ: return l == r;
    case B_NE: return l != r;
    case B_AND: return l & r;
    case B_XOR: return l ^ r;
    case B_OR: return l | r;
    }
    return 0;
}

static int64_t ar_bin(ar_t* a, int minprec) {
    int64_t l = ar_unary(a);
    while (!a->err) {
        int prec, len, op = ar_binop(a, &prec, &len);
        if (op == B_NONE || prec < minprec) break;
        a->i += len;
        if (op == B_LAND || op == B_LOR) {
            int skip = op == B_LAND ? !l : l != 0;
            a->noeval += skip;
            int64_t r = ar_bin(a, prec + 1);
            a->noeval -= skip;
            l = op == B_LAND ? (l && r) : (l || r);
        } else {
            int64_t r = ar_bin(a, prec + 1);
            l = ar_apply(a, op, l, r);
        }
    }
    return l;
}

static int64_t ar_cond(ar_t* a) {
    int64_t c = ar_bin(a, 1);
    ar_ws(a);
    if (a->err || a->s[a->i] != '?') return c;
    a->i++;
    a->noeval += !c;
    int64_t t = ar_assign(a);
    a->noeval -= !c;
    ar_ws(a);
    if (a->s[a->i] != ':') {
        ar_error(a, "expected ':'");
        return 0;
    }
    a->i++;
    a->noeval += c != 0;
    int64_t f = ar_cond(a);
    a->noeval -= c != 0;
    return c ? t : f;
}

static int64_t ar_assign(ar_t* a) {
    ar_ws(a);
    int j = a->i;
    if (is_name1((unsigned char)a->s[j])) {
        while (is_namec((unsigned char)a->s[j])) j++;
        int k = j;
        while (a->s[k] == ' ' || a->s[k] == '\t') k++;
        const char* o = a->s + k;
        int op = -1, len = 0;
        if (o[0] == '=' && o[1] != '=') { op = B_NONE; len = 1; }
        else if (o[0] && o[1] == '=' && strchr("*/%+-&^|", o[0])) {
            static const char ops[] = "*/%+-&^|";
            static const int kinds[] = { B_MUL, B_DIV, B_MOD, B_ADD, B_SUB, B_AND, B_XOR, B_OR };
            op = kinds[strchr(ops, o[0]) - ops];
            len = 2;
        } else if ((o[0] == '<' || o[0] == '>') && o[1] == o[0] && o[2] == '=') {
            op = o[0] == '<' ? B_SHL : B_SHR;
            len = 3;
        }
        if (op >= 0) {
            const char* name = a->s + a->i;
            int n = j - a->i;
            a->i = k + len;
            int64_t r = ar_assign(a);
            if (op != B_NONE) r = ar_apply(a, op, ar_var(a, name, n), r);
            ar_store(a, name, n, r);
            return r;
        }
    }
    return ar_cond(a);
}

// Works out $(( s )): 0, or -1 when an error was said (and stops the command).
static int arith_eval(const char* s, int64_t* out) {
    ar_t a = { s, 0, 0, 0, 0 };
    int64_t v = ar_assign(&a);
    while (!a.err) {
        ar_ws(&a);
        if (s[a.i] != ',') break;
        a.i++;
        v = ar_assign(&a);
    }
    ar_ws(&a);
    if (!a.err && s[a.i]) ar_error(&a, "arithmetic syntax error");
    if (a.err) {
        sh_abort(2);
        return -1;
    }
    *out = v;
    return 0;
}

// ---- Running commands ----

#define E_BADF 9
#define E_PIPE 32
#define SIG_PIPE 13
#define MAX_RSAVE  10  // descriptors one command's redirections may move
#define MAX_OPEN   10  // files one program's redirections may open
#define MAX_STAGES 32  // commands in a pipeline

static int run(node_t* x);
static int run_text(const char* text, int n, int eof_ok, int* more_at, int top);
static const sh_builtin_t* core_find(const char* name);
static const sh_builtin_t* ext_find(const char* name);
static int b_exec(int argc, char** argv);

static int in_stage;  // a builtin is writing into a pipeline in place of a program

// Stops at a Ctrl+C at the prompt. Whether to stop.
static int stop(void) {
    if (sh_interrupted && !S.ctl) S.ctl = CTL_INTR;
    return S.ctl;
}

static void write_all(int fd, const char* s, int n) {
    while (n > 0) {
        int w = shos_write(fd, s, (size_t)n);
        if (w == -E_INTR) continue;
        if (w <= 0) return;
        s += w;
        n -= w;
    }
}

// What the shell's own commands write. Inside $( ) standard output is
// collected; a reader that has gone ends a shell that is not the prompt's,
// as the signal would.
static void bout(int fd, const char* s, int n) {
    if (fd == 1 && S.cap) {
        sput(S.cap, s, n);
        return;
    }
    while (n > 0) {
        int w = shos_write(fd, s, (size_t)n);
        if (w == -E_INTR) continue;
        if (w <= 0) {
            if (w == -E_PIPE && !in_stage && !S.interactive && !S.ctl) {
                S.ctl = CTL_EXIT;
                S.exit_status = 128 + SIG_PIPE;
            } else if (w < 0 && w != -E_PIPE && !S.wfail) {
                S.wfail = 1;
                say("write error: %s", shos_strerror("", -w));
            }
            return;
        }
        s += w;
        n -= w;
    }
}

static void put_quoted(str_t* b, const char* s) {
    sputc(b, '\'');
    for (; *s; s++) {
        if (*s == '\'') sputs(b, "'\\''");
        else sputc(b, *s);
    }
    sputc(b, '\'');
}

static int needs_quote(const char* s) {
    if (!*s) return 1;
    for (; *s; s++)
        if (!is_namec((unsigned char)*s) && !strchr("./=:,+-@%^", *s)) return 1;
    return 0;
}

// set -x: the command about to run.
static void trace(char** v, int n) {
    str_t t = { 0, 0, 0 };
    sputc(&t, '+');
    for (int i = 0; i < n; i++) {
        sputc(&t, ' ');
        if (needs_quote(v[i])) put_quoted(&t, v[i]);
        else sputs(&t, v[i]);
    }
    sputc(&t, '\n');
    bout(2, t.s, t.n);
    free(t.s);
}

static void close_all(int* fds, int n) {
    for (int i = 0; i < n; i++) shos_close(fds[i]);
}

// ---- Redirections ----

// A here-document's text: tabs taken off with <<-, expanded unless its
// delimiter was quoted.
static char* hd_text(redir_t* r, int* len) {
    const char* b = r->body;
    int n = r->blen;
    if (r->strip) {
        char* t = aalloc((size_t)n + 1);
        int k = 0, bol = 1;
        for (int i = 0; i < n; i++) {
            if (bol && b[i] == '\t') continue;
            bol = b[i] == '\n';
            t[k++] = b[i];
        }
        b = t;
        n = k;
    }
    if (r->quoted) {
        *len = n;
        return adup(b, n);
    }
    return expand_str(b, n, C_HD, len);
}

// A pipe holding a here-document's text: its reading end, or -1 (said).
static void rec(str_t* b, char type, const char* s, int n);
static int spawn_records(str_t* b, const int fds[3], int how, int lead);

// A here-document as the read end of a pipe. One bigger than a pipe holds
// is written by a shell of its own (`sh -S` with a w record), since its
// reader has not started yet.
static int hd_pipe(redir_t* r) {
    int len;
    char* t = hd_text(r, &len);
    if (S.ctl) return -1;
    int p[2], e = shos_pipe(p);
    if (e < 0) {
        say("pipe: %s", shos_strerror("pipe", -e));
        return -1;
    }
    if (len <= HEREDOC_MAX) {
        write_all(p[1], t, len);
    } else {
        str_t b = { 0, 0, 0 };
        rec(&b, 'w', t, len);
        int fds[3] = { 0, p[1], 2 };
        int id = spawn_records(&b, fds, SHJ_SAME, 0);
        free(b.s);
        if (id < 0) {
            shos_close(p[0]);
            shos_close(p[1]);
            return -1;
        }
    }
    shos_close(p[1]);
    return p[0];
}

static int open_redir(redir_t* r, const char* path) {
    int how = r->op == T_LESS ? SHO_READ : r->op == T_DGREAT ? SHO_APPEND : r->op == T_LESSGREAT ? SHO_RDWR : SHO_WRITE;
    int fd = shos_open(path, how);
    if (fd < 0) say("%s: %s", path, shos_strerror(path, -fd));
    return fd;
}

// The word of <& or >&: a descriptor 0-9, -1 for -, or -2.
static int dup_target(const char* w) {
    if (w[0] == '-' && !w[1]) return -1;
    if (is_digit((unsigned char)w[0]) && !w[1]) return w[0] - '0';
    return -2;
}

// Descriptors moved out of the way while a command's redirections are in
// force, to be put back after.
typedef struct {
    struct { int fd, slot; } v[MAX_RSAVE];  // slot -1: fd was not open
    int n;
} rsaves_t;

static int save_fd(rsaves_t* sv, int fd, int avoid) {
    for (int i = 0; i < sv->n; i++)
        if (sv->v[i].fd == fd) return 0;
    int s = -1;
    for (int k = SHOS_SAVE_FIRST; k < SHOS_SAVE_END; k++) {
        if (!(S.slots & (1 << k)) && k != avoid) {
            s = k;
            break;
        }
    }
    if (sv->n == MAX_RSAVE || s < 0) {
        say("too many redirections at once");
        return -1;
    }
    if (shos_dup2(fd, s) < 0) s = -1;
    else S.slots |= 1 << s;
    sv->v[sv->n].fd = fd;
    sv->v[sv->n++].slot = s;
    return 0;
}

static void restore_redirs(rsaves_t* sv) {
    while (sv->n) {
        sv->n--;
        int fd = sv->v[sv->n].fd, s = sv->v[sv->n].slot;
        if (s < 0) {
            shos_close(fd);
        } else {
            shos_dup2(s, fd);
            shos_close(s);
            S.slots &= ~(1 << s);
        }
    }
}

// exec with no program: the redirections stay.
static void keep_redirs(rsaves_t* sv) {
    for (int i = 0; i < sv->n; i++) {
        int s = sv->v[i].slot;
        if (s >= 0) {
            shos_close(s);
            S.slots &= ~(1 << s);
        }
    }
    sv->n = 0;
}

// Redirections for something that runs inside the shell. 0, or -1 when
// one failed (said, and what was done is undone).
static int apply_redirs(redir_t* r, rsaves_t* sv) {
    amark_t m = amark();
    for (; r; r = r->next) {
        int src;
        if (r->op == T_DLESS || r->op == T_DLESSDASH) {
            src = hd_pipe(r);
        } else {
            char* w = expand_str(r->p, r->n, C_UNQ, 0);
            if (S.ctl) goto fail;
            if (r->op == T_LESSAND || r->op == T_GREATAND) {
                int d = dup_target(w);
                if (d == -2) {
                    say("%s: bad file descriptor", w);
                    goto fail;
                }
                if (save_fd(sv, r->fd, -1) < 0) goto fail;
                if (d < 0) {
                    shos_close(r->fd);
                } else if (d != r->fd && shos_dup2(d, r->fd) < 0) {
                    say("%d: bad file descriptor", d);
                    goto fail;
                }
                continue;
            }
            src = open_redir(r, w);
        }
        if (src < 0) goto fail;
        if (save_fd(sv, r->fd, src) < 0) {
            shos_close(src);
            goto fail;
        }
        if (src != r->fd) {
            shos_dup2(src, r->fd);
            shos_close(src);
        }
    }
    arelease(m);
    return 0;
fail:
    arelease(m);
    restore_redirs(sv);
    return -1;
}

// Redirections for a program: fds (the shell's descriptors that become
// its 0, 1 and 2) and fd3 are changed; the files opened for it go in
// opened[], to be closed once it has started. 0, or -1 (said).
static int prog_fds(redir_t* r, int fds[3], int* fd3, int* opened, int* nopen) {
    for (; r; r = r->next) {
        int fd;
        if (r->op == T_DLESS || r->op == T_DLESSDASH) {
            fd = hd_pipe(r);
        } else {
            char* w = expand_str(r->p, r->n, C_UNQ, 0);
            if (S.ctl) goto fail;
            if (r->op == T_LESSAND || r->op == T_GREATAND) {
                int d = dup_target(w);
                if (d == -2) {
                    say("%s: bad file descriptor", w);
                    goto fail;
                }
                int v = d < 0 ? -1 : d <= 2 ? fds[d] : d == 3 && *fd3 >= 0 ? *fd3 : d;
                if (r->fd <= 2) fds[r->fd] = v;
                else if (r->fd == 3) *fd3 = v;
                continue;
            }
            fd = open_redir(r, w);
        }
        if (fd < 0) goto fail;
        if (*nopen == MAX_OPEN) {
            shos_close(fd);
            say("too many redirections at once");
            goto fail;
        }
        opened[(*nopen)++] = fd;
        if (r->fd <= 2) fds[r->fd] = fd;
        else if (r->fd == 3) *fd3 = fd;
        // other descriptors: the file is made, and that is all
    }
    return 0;
fail:
    close_all(opened, *nopen);
    *nopen = 0;
    return -1;
}

// ---- Programs and shells of their own ----

// Waits for a task: its status, 128 + the signal if one ended it.
// stoppable: a Ctrl+C at the prompt stops the waiting (wait).
static int wait_task(int id, int report, int stoppable) {
    int sig = 0, st;
    while ((st = shos_wait(id, &sig)) == -E_INTR)
        if (stoppable && sh_interrupted) return 128 + SIG_INT;
    if (st < 0) return 127;
    if (!sig) return st;
    if (report && S.interactive && !S.sig_reported) {
        S.sig_reported = 1;
        if (S.front && S.front->report_signal) S.front->report_signal(sig);
    }
    if (sig == SIG_INT && S.interactive && !S.ctl) S.ctl = CTL_INTR;
    return 128 + sig;
}

// Starts a program: its task id, or -1 having said why (*st: the status).
// A file that is not a program is read by the shell, as a script.
static int spawn_prog(char** argv, const int fds[3], int fd3, int how, int lead, int* st, int explicit_run) {
    const char* name = argv[0];
    int id = shos_spawn(name, argv, fds, fd3, how, lead);
    if (id == -E_NOEXEC) {
        argv_t a = { 0, 0, 0 };
        static char sh[] = "sh";
        av_add(&a, sh);
        char* path = (char*)name;
        if (!strchr(name, '/')) {  // a bare name is a program in the root
            size_t n = strlen(name);
            path = aalloc(n + 2);
            path[0] = '/';
            memcpy(path + 1, name, n);
        }
        av_add(&a, path);
        for (int k = 1; argv[k]; k++) av_add(&a, argv[k]);
        id = shos_spawn(SHOS_SELF, a.v, fds, fd3, how, lead);
        free(a.v);
    }
    if (id >= 0) return id;
    int keep = say_fd;
    if (fds[2] >= 0) say_fd = fds[2];
    int bare = !strchr(name, '/') && !strchr(name, '.');
    if ((id == -E_NOENT || id == -E_INVAL) && bare) {
        if (S.ui && !explicit_run) say("Unknown command '%s'; try 'help'", name);
        else say("%s: not found", name);
        *st = 127;
    } else {
        say("%s: %s", name, shos_strerror(name, -id));
        *st = id == -E_NOENT ? 127 : 126;
    }
    say_fd = keep;
    return -1;
}

static void rec(str_t* b, char type, const char* s, int n) {
    char nb[24];
    sputc(b, type);
    sputs(b, itoa64(n, nb));
    sputc(b, ':');
    sput(b, s, n);
}

static void rec_kv(str_t* b, char type, const char* k, const char* v, int vn) {
    char nb[24];
    int kn = (int)strlen(k);
    sputc(b, type);
    sputs(b, itoa64(kn + 1 + vn, nb));
    sputc(b, ':');
    sput(b, k, kn);
    sputc(b, '=');
    sput(b, v, vn);
}

static void rec_num(str_t* b, char type, int64_t v) {
    char nb[24];
    itoa64(v, nb);
    rec(b, type, nb, (int)strlen(nb));
}

// Starts a shell of its own (sh -S) to run text, with everything it needs
// to know sent over its descriptor 3. Its task id, or -1 (said).
// Starts `sh -S` and hands it the records in b: its task id, or -1 (said).
static int spawn_records(str_t* b, const int fds[3], int how, int lead) {
    int p[2], e = shos_pipe(p);
    if (e < 0) {
        say("pipe: %s", shos_strerror("pipe", -e));
        return -1;
    }
    static char a0[] = "sh", a1[] = "-S";
    char* argv[] = { a0, a1, 0 };
    int id = shos_spawn(SHOS_SELF, argv, fds, p[0], how, lead);
    shos_close(p[0]);
    if (id < 0) {
        shos_close(p[1]);
        say("sh: %s", shos_strerror("sh", -id));
        return -1;
    }
    write_all(p[1], b->s, b->n);
    shos_close(p[1]);
    return id;
}

static int spawn_shell(const char* text, int n, const int fds[3], int how, int lead) {
    str_t b = { 0, 0, 0 };
    for (int h = 0; h < NBUCKETS; h++)
        for (var_t* v = vars[h]; v; v = v->next) rec_kv(&b, 'v', v->name, v->val, (int)strlen(v->val));
    for (func_t* f = funcs; f; f = f->next) rec_kv(&b, 'f', f->name, f->body, f->len);
    const char* a = S.arg0 ? S.arg0 : "sh";
    rec(&b, '0', a, (int)strlen(a));
    for (int k = 0; k < S.pc; k++) rec(&b, 'p', S.pv[k], (int)strlen(S.pv[k]));
    char o[16];
    int k = 0;
    if (S.opt_e) o[k++] = 'e';
    if (S.opt_u) o[k++] = 'u';
    if (S.opt_x) o[k++] = 'x';
    if (S.opt_f) o[k++] = 'f';
    if (S.noerrexit) o[k++] = 'N';
    if (S.ui) o[k++] = 'i';
    if (S.fdepth || S.dotdepth) o[k++] = 'F';
    rec(&b, 'o', o, k);
    rec_num(&b, '?', S.last);
    rec_num(&b, '$', S.pid);
    rec_num(&b, '!', S.bg);
    rec(&b, 'c', text, n);
    int id = spawn_records(&b, fds, how, lead);
    free(b.s);
    return id;
}

// x runs a program that can be started as it is: its first word is typed
// plainly and is not a function or one of the shell's commands.
// 1: x runs a program. 2: it is `run PROG ...`, which starts PROG the
// same way, so that a job in the background or a pipeline's stage is
// the program itself (what tasks shows, and kill and Ctrl+C reach)
// rather than a shell waiting for it. 0: anything else.
static int prog_cmd(node_t* x) {
    if (x->kind != N_SIMPLE || !x->words || x->assigns) return 0;
    word_t* w = x->words;
    if (!plain(w->p, w->n)) return 0;
    amark_t m = amark();
    char* name = adup(w->p, w->n);
    int r = ffind(name) ? 0 : core_find(name) ? (!strcmp(name, "run") && w->next ? 2 : 0) : !ext_find(name);
    arelease(m);
    return r;
}

// Starts x apart from the shell, with fds as its 0, 1 and 2: a program as
// it is, anything else in a shell of its own. The task id, or -1 (said;
// *st: the status).
static int spawn_apart(node_t* x, const int fds_in[3], int how, int lead, int* st) {
    *st = 2;
    int kind = prog_cmd(x);
    if (kind) {
        amark_t m = amark();
        argv_t av = { 0, 0, 0 };
        int id = -1, skip = kind - 1;  // past `run`
        if (expand_words(x->words, &av) == 0 && av.n > skip) {
            int fds[3] = { fds_in[0], fds_in[1], fds_in[2] }, fd3 = -1, opened[MAX_OPEN], nopen = 0;
            *st = 1;
            if (prog_fds(x->redirs, fds, &fd3, opened, &nopen) == 0) {
                if (S.opt_x) trace(av.v, av.n);
                id = spawn_prog(av.v + skip, fds, fd3, how, lead, st, skip);
                close_all(opened, nopen);
            }
        } else if (skip && !S.ctl && av.n == 1) {
            say("run: needs a program");
        }
        free(av.v);
        arelease(m);
        return id;
    }
    int len;
    char* text = rebuild(x, &len);
    int id = spawn_shell(text, len, fds_in, how, lead);
    free(text);
    return id;
}

static int run_prog(node_t* x, argv_t* av) {
    for (word_t* w = x->assigns; w; w = w->next) {  // there is no environment: only what they do
        expand_str(w->p, w->n, C_UNQ, 0);
        if (S.ctl) return 1;
    }
    int fds[3] = { 0, 1, 2 }, fd3 = -1, opened[MAX_OPEN], nopen = 0, st;
    if (prog_fds(x->redirs, fds, &fd3, opened, &nopen) < 0) return 1;
    int how = S.interactive ? SHJ_NEW_FG : SHJ_SAME;
    int id = spawn_prog(av->v, fds, fd3, how, 0, &st, 0);
    close_all(opened, nopen);
    if (id < 0) return st;
    st = wait_task(id, 1, 0);
    if (how == SHJ_NEW_FG) shos_job_done(id);
    return st;
}

// A program run as asked (run, exec), with the shell's 0, 1 and 2.
static int run_argv(char** argv) {
    int fds[3] = { 0, 1, 2 }, st;
    int how = S.interactive ? SHJ_NEW_FG : SHJ_SAME;
    int id = spawn_prog(argv, fds, -1, how, 0, &st, 1);
    if (id < 0) return st;
    st = wait_task(id, 1, 0);
    if (how == SHJ_NEW_FG) shos_job_done(id);
    return st;
}

// ---- $( ) ----

static void read_all(int fd, str_t* out) {
    char buf[512];
    for (;;) {
        int r = shos_read(fd, buf, sizeof buf);
        if (r == -E_INTR) continue;
        if (r <= 0) break;
        sput(out, buf, r);
    }
}

// Runs text and collects what it writes. A command of the shell's own
// that only writes runs inside it, a program is started as it is, and
// anything else runs in a shell of its own.
static void run_capture(const char* text, int n, str_t* out) {
    amark_t m = amark();
    src_t src = { text, n, 0, 0 };
    src.hdtail = &src.hdocs;
    lex_t L;
    lx_init(&L, &src, 1);
    node_t* x = p_clist(&L, 1);
    if (x && tok(&L) != T_EOF) near(&L);
    S.did_subst = 1;
    if (L.err) {
        say("%s", L.emsg);
        sh_abort(2);
        S.subst_status = 2;
        arelease(m);
        return;
    }
    node_t* c = x->kids;
    if (!c) {
        S.subst_status = 0;
        arelease(m);
        return;
    }
    int st = 2, id = -1, p[2] = { -1, -1 };
    if (!c->next && !c->bg && c->kind == N_SIMPLE && !c->assigns && !c->redirs && c->words &&
        plain(c->words->p, c->words->n)) {
        char* name = adup(c->words->p, c->words->n);
        const sh_builtin_t* b = ffind(name) ? 0 : core_find(name);
        if (b && (b->flags & SHB_PURE)) {
            argv_t av = { 0, 0, 0 };
            if (expand_words(c->words, &av) == 0) {
                str_t* oc = S.cap;
                S.cap = out;
                if (S.opt_x) trace(av.v, av.n);
                st = b->fn(av.n, av.v);
                S.cap = oc;
            }
            free(av.v);
            S.subst_status = st;
            S.did_subst = 1;
            arelease(m);
            return;
        }
        if (!b && !ffind(name) && !ext_find(name)) {
            argv_t av = { 0, 0, 0 };
            if (expand_words(c->words, &av) == 0) {
                int e = shos_pipe(p);
                if (e < 0) {
                    say("pipe: %s", shos_strerror("pipe", -e));
                } else {
                    int fds[3] = { 0, p[1], 2 };
                    if (S.opt_x) trace(av.v, av.n);
                    id = spawn_prog(av.v, fds, -1, SHJ_SAME, 0, &st, 0);
                }
            }
            free(av.v);
            goto collect;
        }
    }
    {
        int e = shos_pipe(p);
        if (e < 0) {
            say("pipe: %s", shos_strerror("pipe", -e));
        } else {
            int fds[3] = { 0, p[1], 2 };
            id = spawn_shell(text, n, fds, SHJ_SAME, 0);
        }
    }
collect:
    if (p[1] >= 0) shos_close(p[1]);
    if (p[0] >= 0) {
        read_all(p[0], out);
        shos_close(p[0]);
    }
    if (id >= 0) st = wait_task(id, 0, 0);
    S.subst_status = st;
    S.did_subst = 1;
    arelease(m);
}

// ---- Commands inside the shell ----

static int assign_word(word_t* w, save_t** save, str_t* tr) {
    int eq = 0;
    while (w->p[eq] != '=') eq++;
    int len;
    char* v = expand_str(w->p + eq + 1, w->n - eq - 1, C_UNQ, &len);
    if (S.ctl) return -1;
    if (save) save_var(save, w->p, eq);
    vsetn(w->p, eq, v);
    if (tr) {
        sputc(tr, ' ');
        sput(tr, w->p, eq + 1);
        if (needs_quote(v)) put_quoted(tr, v);
        else sputs(tr, v);
    }
    return 0;
}

static int run_builtin(const sh_builtin_t* b, node_t* x, argv_t* av) {
    int special = b->flags & 0x100;
    save_t* saved = 0;
    for (word_t* w = x->assigns; w; w = w->next) {
        if (assign_word(w, special ? 0 : &saved, 0) < 0) {
            restore_vars(saved);
            return 1;
        }
    }
    rsaves_t sv;
    sv.n = 0;
    if (apply_redirs(x->redirs, &sv) < 0) {
        restore_vars(saved);
        if (special) sh_abort(2);
        return 1;
    }
    int st;
    if (b->fn == b_exec && av->n == 1) {
        keep_redirs(&sv);
        st = 0;
    } else {
        int wf = S.wfail;
        S.wfail = 0;
        st = b->fn(av->n, av->v);
        if (S.wfail && !st) st = 1;
        S.wfail = wf;
        restore_redirs(&sv);
    }
    restore_vars(saved);
    return st;
}

static void free_params(void) {
    for (int i = 0; i < S.pc; i++) free(S.pv[i]);
    free(S.pv);
    S.pv = 0;
    S.pc = 0;
}

static int call_func(func_t* f, node_t* x, argv_t* av) {
    if (S.fdepth >= MAX_CALLS) {
        say("%s: functions call each other too deeply", f->name);
        sh_abort(2);
        return 2;
    }
    save_t* frame = 0;
    for (word_t* w = x->assigns; w; w = w->next) {
        if (assign_word(w, &frame, 0) < 0) {
            restore_vars(frame);
            return 1;
        }
    }
    rsaves_t sv;
    sv.n = 0;
    if (apply_redirs(x->redirs, &sv) < 0) {
        restore_vars(frame);
        return 1;
    }
    char** opv = S.pv;
    int opc = S.pc;
    S.pv = 0;
    S.pc = 0;
    set_params(av->v + 1, av->n - 1);
    save_t** oframe = S.frame;
    S.frame = &frame;
    S.fdepth++;
    char* body = adup(f->body, f->len);  // the function may be changed while it runs
    S.last = 0;
    run_text(body, f->len, 1, 0, 0);
    int st = S.last;
    S.fdepth--;
    if (S.ctl == CTL_RETURN) {
        S.ctl = CTL_NONE;
        st = S.retval;
    }
    free_params();
    S.pv = opv;
    S.pc = opc;
    S.frame = oframe;
    restore_redirs(&sv);
    restore_vars(frame);
    return st;
}

static int run_simple(node_t* x) {
    amark_t m = amark();
    argv_t av = { 0, 0, 0 };
    int st = 0;
    S.did_subst = 0;
    S.subst_status = 0;
    if (expand_words(x->words, &av) < 0) goto done;
    if (av.n == 0) {
        str_t tr = { 0, 0, 0 };
        sputc(&tr, '+');
        for (word_t* w = x->assigns; w; w = w->next)
            if (assign_word(w, 0, S.opt_x ? &tr : 0) < 0) break;
        if (S.opt_x && x->assigns && !S.ctl) {
            sputc(&tr, '\n');
            bout(2, tr.s, tr.n);
        }
        free(tr.s);
        if (S.ctl) goto done;
        st = S.did_subst ? S.subst_status : 0;
        if (x->redirs) {
            rsaves_t sv;
            sv.n = 0;
            if (apply_redirs(x->redirs, &sv) < 0) st = 1;
            else restore_redirs(&sv);
        }
        goto done;
    }
    if (S.opt_x) trace(av.v, av.n);
    const char* name = av.v[0];
    func_t* f = ffind(name);
    const sh_builtin_t* b;
    if (f) st = call_func(f, x, &av);
    else if ((b = core_find(name)) || (b = ext_find(name))) st = run_builtin(b, x, &av);
    else st = run_prog(x, &av);
done:
    if (S.ctl == CTL_ABORT && st == 0) st = S.abort_status;
    free(av.v);
    arelease(m);
    return st;
}

// ---- Pipelines, lists, compound commands ----

// x can run inside the shell as the first command of a pipeline: one of
// its commands that only writes.
static int pure_stage(node_t* x) {
    if (x->kind != N_SIMPLE || !x->words || x->assigns || x->redirs) return 0;
    word_t* w = x->words;
    if (!plain(w->p, w->n)) return 0;
    amark_t m = amark();
    char* name = adup(w->p, w->n);
    const sh_builtin_t* b = ffind(name) ? 0 : core_find(name);
    if (!b && !ffind(name)) b = ext_find(name);
    arelease(m);
    return b && (b->flags & SHB_PURE);
}

static int run_stages(node_t* x) {
    int ids[MAX_STAGES], n = 0, k = 0, lead = 0, prev = -1, st = 0, def_fd = -1;
    node_t* def = 0;
    for (node_t* c = x->kids; c; c = c->next) n++;
    if (n > MAX_STAGES) {
        say("a pipeline can have at most %d commands", MAX_STAGES);
        return 2;
    }
    int fg = S.interactive;
    for (node_t* c = x->kids; c; c = c->next, k++) {
        int p[2] = { -1, -1 };
        ids[k] = -1;
        if (c->next) {
            int e = shos_pipe(p);
            if (e < 0) {
                say("pipe: %s", shos_strerror("pipe", -e));
                st = 2;
                for (int j = k; j < n; j++) ids[j] = -1;
                break;
            }
        }
        if (k == 0 && c->next && pure_stage(c)) {
            def = c;  // it runs once the rest have started, to have a reader
            def_fd = p[1];
        } else {
            int fds[3] = { prev >= 0 ? prev : 0, c->next ? p[1] : 1, 2 };
            int how = lead ? (fg ? SHJ_JOIN : SHJ_SAME) : (fg ? SHJ_NEW_FG : SHJ_SAME);
            int sst;
            ids[k] = spawn_apart(c, fds, how, lead, &sst);
            if (ids[k] < 0 && !c->next) st = sst;
            if (ids[k] >= 0 && !lead) lead = ids[k];
            if (p[1] >= 0) shos_close(p[1]);
        }
        if (prev >= 0) shos_close(prev);
        prev = p[0];
        if (S.ctl) {
            for (int j = k + 1; j < n; j++) ids[j] = -1;
            break;
        }
    }
    if (prev >= 0) shos_close(prev);
    if (def) {
        if (!S.ctl) {
            rsaves_t sv;
            sv.n = 0;
            if (save_fd(&sv, 1, def_fd) == 0) {
                shos_dup2(def_fd, 1);
                in_stage++;
                run(def);
                in_stage--;
                restore_redirs(&sv);
            }
        }
        shos_close(def_fd);
    }
    for (k = 0; k < n; k++) {
        if (ids[k] < 0) continue;
        int s = wait_task(ids[k], 1, 0);
        if (k == n - 1) st = s;
    }
    if (fg && lead) shos_job_done(lead);
    return st;
}

static int pipe_status(node_t* x) {  // N_PIPE
    if (x->bang) S.noerrexit++;
    int st = x->kids->next ? run_stages(x) : run(x->kids);
    if (x->bang) {
        S.noerrexit--;
        st = !st;
    }
    return st;
}

// A pipeline as an item of a list, or the last of && and ||: set -e
// looks at it.
static int run_pl(node_t* x) {
    int st = x->kind == N_PIPE ? pipe_status(x) : run(x);
    S.last = st;
    if (st && S.opt_e && !S.noerrexit && !S.ctl && !(x->kind == N_PIPE && x->bang)) {
        S.ctl = CTL_EXIT;
        S.exit_status = st;
    }
    return st;
}

static int run_andor(node_t* x) {
    node_t* a = x->a;
    S.noerrexit++;
    int st = a->kind == N_AND || a->kind == N_OR ? run_andor(a) : run_pl(a);
    S.noerrexit--;
    S.last = st;
    if (S.ctl) return st;
    if ((x->kind == N_AND) == (st == 0)) st = run_pl(x->b);
    return st;
}

static int run_bg(node_t* c) {
    int fds[3] = { 0, 1, 2 }, st;
    int how = S.interactive ? SHJ_NEW_BG : SHJ_SAME;
    int id = spawn_apart(c, fds, how, 0, &st);
    if (id < 0) return S.ctl ? st : 0;
    S.bg = id;
    if (S.nbg == MAX_BG) {
        memmove(S.bgids, S.bgids + 1, (MAX_BG - 1) * sizeof *S.bgids);
        S.nbg--;
    }
    S.bgids[S.nbg++] = id;
    if (S.interactive) {
        char b[64];
        snprintf(b, sizeof b, "[%d] started in the background\n", id);
        shos_write(2, b, strlen(b));
    }
    return 0;
}

static int run_list(node_t* x) {
    int st = 0;
    for (node_t* c = x->kids; c && !stop(); c = c->next) {
        if (c->bg) st = run_bg(c);
        else if (c->kind == N_AND || c->kind == N_OR) st = run_andor(c);
        else st = run_pl(c);
        S.last = st;
    }
    return st;
}

// After a part of a loop ran: 1 to leave the loop.
static int loop_ctl(void) {
    if (S.ctl == CTL_BREAK) {
        if (--S.ctln <= 0) S.ctl = CTL_NONE;
        return 1;
    }
    if (S.ctl == CTL_CONTINUE) {
        if (--S.ctln > 0) return 1;
        S.ctl = CTL_NONE;
        return 0;
    }
    return S.ctl != CTL_NONE;
}

static int run_while(node_t* x) {
    int st = 0;
    S.loops++;
    for (;;) {
        S.noerrexit++;
        int c = run(x->a);
        S.noerrexit--;
        if (S.ctl) {
            if (loop_ctl()) break;
            continue;
        }
        if ((c == 0) != (x->kind == N_WHILE)) break;
        st = run(x->b);
        if (S.ctl && loop_ctl()) break;
        if (stop()) break;
    }
    S.loops--;
    return st;
}

static int run_for(node_t* x) {
    amark_t m = amark();
    argv_t av = { 0, 0, 0 };
    int st = 0;
    if (!x->has_in) {
        for (int k = 0; k < S.pc; k++) av_add(&av, adup(S.pv[k], (int)strlen(S.pv[k])));
    } else if (expand_words(x->words, &av) < 0) {
        free(av.v);
        arelease(m);
        return 1;
    }
    S.loops++;
    for (int i = 0; i < av.n; i++) {
        vsetn(x->name, x->nlen, av.v[i]);
        st = run(x->b);
        if (S.ctl && loop_ctl()) break;
        if (stop()) break;
    }
    S.loops--;
    free(av.v);
    arelease(m);
    return st;
}

static int run_case(node_t* x) {
    amark_t m = amark();
    int sn;
    char* subj = expand_str(x->words->p, x->words->n, C_UNQ, &sn);
    if (S.ctl) {
        arelease(m);
        return 1;
    }
    for (citem_t* it = x->items; it; it = it->next) {
        for (word_t* p = it->pats; p; p = p->next) {
            char* pc;
            unsigned char* pl;
            int pn;
            ex_pattern(p->p, p->n, &pc, &pl, &pn);
            if (S.ctl) {
                arelease(m);
                return 1;
            }
            if (pmatch(pc, pl, pn, subj, sn, 0)) {
                arelease(m);
                return it->body ? run(it->body) : 0;
            }
        }
    }
    arelease(m);
    return 0;
}

static int run_subshell(node_t* x) {
    amark_t m = amark();
    int fds[3] = { 0, 1, 2 }, fd3 = -1, opened[MAX_OPEN], nopen = 0;
    int r = prog_fds(x->redirs, fds, &fd3, opened, &nopen);
    arelease(m);
    if (r < 0) return 1;
    int len;
    char* text = rebuild(x->a, &len);
    int how = S.interactive ? SHJ_NEW_FG : SHJ_SAME;
    int id = spawn_shell(text, len, fds, how, 0);
    free(text);
    close_all(opened, nopen);
    if (id < 0) return 2;
    int st = wait_task(id, 1, 0);
    if (how == SHJ_NEW_FG) shos_job_done(id);
    return st;
}

static int run_node(node_t* x) {
    switch (x->kind) {
    case N_LIST: return run_list(x);
    case N_AND:
    case N_OR: return run_andor(x);
    case N_PIPE: return pipe_status(x);
    case N_SIMPLE: return run_simple(x);
    case N_IF: {
        S.noerrexit++;
        int c = run(x->a);
        S.noerrexit--;
        if (S.ctl) return c;
        if (c == 0) return run(x->b);
        return x->c ? run(x->c) : 0;
    }
    case N_WHILE:
    case N_UNTIL: return run_while(x);
    case N_FOR: return run_for(x);
    case N_CASE: return run_case(x);
    case N_BRACE: return run(x->a);
    case N_SUBSHELL: return run_subshell(x);
    case N_FUNC: {
        int len;
        char* body = rebuild(x->a, &len);
        fdefine(x->name, x->nlen, body, len);
        return 0;
    }
    }
    return 0;
}

static int run(node_t* x) {
    if (stop()) return S.last;
    int st;
    if (x->redirs && x->kind != N_SIMPLE && x->kind != N_SUBSHELL) {
        rsaves_t sv;
        sv.n = 0;
        if (apply_redirs(x->redirs, &sv) < 0) {
            S.last = 1;
            return 1;
        }
        st = run_node(x);
        restore_redirs(&sv);
    } else {
        st = run_node(x);
    }
    S.last = st;
    return st;
}

// Runs the commands in text, one complete command at a time. eof_ok: the
// text is all there is; if not, P_MORE says the last command is not
// finished, and *more_at where it starts. top: at the prompt.
static int run_text(const char* text, int n, int eof_ok, int* more_at, int top) {
    src_t src = { text, n, 0, 0 };
    src.hdtail = &src.hdocs;
    lex_t L;
    lx_init(&L, &src, eof_ok);
    int r = P_OK;
    while (!stop()) {
        amark_t m = amark();
        int start = L.i;
        node_t* x = p_complete(&L);
        if (L.err) {
            if (L.err == P_MORE) {
                if (more_at) *more_at = start;
                r = P_MORE;
            } else {
                say("%s", L.emsg);
                S.last = 2;
                sh_abort(2);
                r = P_BAD;
            }
            arelease(m);
            break;
        }
        if (!x) {
            arelease(m);
            break;
        }
        if (top) S.sig_reported = 0;
        run(x);
        if (top && S.interactive && S.last && !S.sig_reported && S.ctl != CTL_EXIT && S.front && S.front->report_status)
            S.front->report_status(S.last);
        arelease(m);
        src.hdocs = 0;
        src.hdtail = &src.hdocs;
    }
    return r;
}

// ---- The shell's own commands ----

#define SHB_SPECIAL 0x100  // assignments before it stay; its errors stop a script

static int b_true(int argc, char** argv) { (void)argc; (void)argv; return 0; }
static int b_false(int argc, char** argv) { (void)argc; (void)argv; return 1; }

static int hexval(int c) {
    if (is_digit(c)) return c - '0';
    c = fold_c(c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

// echo -e's escapes. 1 if \c asked to stop.
static int echo_esc(str_t* o, const char* s) {
    for (; *s; s++) {
        if (*s != '\\' || !s[1]) {
            sputc(o, *s);
            continue;
        }
        s++;
        switch (*s) {
        case 'n': sputc(o, '\n'); break;
        case 't': sputc(o, '\t'); break;
        case 'r': sputc(o, '\r'); break;
        case 'a': sputc(o, '\a'); break;
        case 'b': sputc(o, '\b'); break;
        case 'e': sputc(o, 27); break;
        case 'f': sputc(o, '\f'); break;
        case 'v': sputc(o, '\v'); break;
        case '\\': sputc(o, '\\'); break;
        case 'c': return 1;
        case '0': {
            int v = 0, n = 0;
            while (n < 3 && s[1] >= '0' && s[1] <= '7') v = v * 8 + (*++s - '0'), n++;
            sputc(o, (char)v);
            break;
        }
        case 'x': {
            int v = 0, n = 0;
            while (n < 2 && hexval((unsigned char)s[1]) >= 0) v = v * 16 + hexval((unsigned char)*++s), n++;
            if (n) sputc(o, (char)v);
            else sputs(o, "\\x");
            break;
        }
        default:
            sputc(o, '\\');
            sputc(o, *s);
            break;
        }
    }
    return 0;
}

static int b_echo(int argc, char** argv) {
    int a = 1, nl = 1, esc = 0;
    for (; a < argc; a++) {
        const char* s = argv[a];
        if (s[0] != '-' || !s[1]) break;
        int ok = 1;
        for (int i = 1; s[i]; i++)
            if (s[i] != 'n' && s[i] != 'e' && s[i] != 'E') ok = 0;
        if (!ok) break;
        for (int i = 1; s[i]; i++) {
            if (s[i] == 'n') nl = 0;
            else esc = s[i] == 'e';
        }
    }
    str_t o = { 0, 0, 0 };
    sput(&o, "", 0);
    int stopped = 0;
    for (int first = a; a < argc && !stopped; a++) {
        if (a > first) sputc(&o, ' ');
        if (esc) stopped = echo_esc(&o, argv[a]);
        else sputs(&o, argv[a]);
    }
    if (nl && !stopped) sputc(&o, '\n');
    bout(1, o.s, o.n);
    free(o.s);
    return 0;
}

static void pf_out(void* ctx, const char* s, uint32_t n) { sput(ctx, s, (int)n); }

static void pf_err(void* ctx, const char* s, uint32_t n) {
    str_t* o = ctx;
    if (o->n) {
        bout(1, o->s, o->n);
        o->n = 0;
    }
    bout(2, s, (int)n);
}

static int b_printf(int argc, char** argv) {
    if (argc < 2) {
        say("printf: missing operand");
        return 1;
    }
    if (!strcmp(argv[1], "--")) {
        argv++;
        if (--argc < 2) return 0;
    }
    str_t o = { 0, 0, 0 };
    pf_io_t io = { pf_out, pf_err, &o };
    int st = pf_run(&io, argv[1], argc - 2, argv + 2);
    if (o.n) bout(1, o.s, o.n);
    free(o.s);
    return st;
}

static int b_pwd(int argc, char** argv) {
    (void)argc;
    (void)argv;
    char b[PBUF + 1];
    int r = shos_getcwd(b, PBUF);
    if (r < 0) {
        say("pwd: %s", shos_strerror(".", -r));
        return 1;
    }
    size_t n = strlen(b);
    b[n] = '\n';
    bout(1, b, (int)n + 1);
    return 0;
}

// test and [

static char** t_v;
static int t_n, t_i, t_bad;

static int t_isunary(const char* s) { return s[0] == '-' && s[1] && !s[2] && strchr("efdsrwxnzhLtbcpS", s[1]); }

static int t_isbinary(const char* s) {
    static const char* const ops[] = { "=", "==", "!=", "<", ">", "-eq", "-ne", "-lt", "-le", "-gt", "-ge" };
    for (size_t i = 0; i < sizeof ops / sizeof *ops; i++)
        if (strcmp(s, ops[i]) == 0) return 1;
    return 0;
}

static int t_num(const char* s, int64_t* v) {
    const char* p = s;
    while (*p == ' ' || *p == '\t') p++;
    int n = (int)strlen(p);
    while (n && (p[n - 1] == ' ' || p[n - 1] == '\t')) n--;
    char buf[32];
    if (n > 0 && n < (int)sizeof buf) {
        memcpy(buf, p, (size_t)n);
        buf[n] = 0;
        if (parse_int(buf, v) == 0) return 1;
    }
    if (!t_bad) say("test: %s: integer expected", s);
    t_bad = 1;
    return 0;
}

static int t_unary(char op, const char* a) {
    long size = 0;
    switch (op) {
    case 'n': return a[0] != 0;
    case 'z': return a[0] == 0;
    case 'e':
    case 'r':
    case 'w':
    case 'x': return shos_stat(a, &size) != SHS_NONE;
    case 'f': return shos_stat(a, &size) == SHS_FILE;
    case 'd': return shos_stat(a, &size) == SHS_DIR;
    case 's': return shos_stat(a, &size) != SHS_NONE && size > 0;
    case 't': {
        int64_t fd;
        return t_num(a, &fd) && fd >= 0 && fd < 1024 && shos_isatty((int)fd);
    }
    }
    return 0;  // no links, devices, pipes or sockets have names here
}

static int t_binary(const char* a, const char* op, const char* b) {
    if (!strcmp(op, "=") || !strcmp(op, "==")) return strcmp(a, b) == 0;
    if (!strcmp(op, "!=")) return strcmp(a, b) != 0;
    if (!strcmp(op, "<")) return strcmp(a, b) < 0;
    if (!strcmp(op, ">")) return strcmp(a, b) > 0;
    int64_t x, y;
    if (!t_num(a, &x) || !t_num(b, &y)) return 0;
    switch (op[1] * 256 + op[2]) {
    case 'e' * 256 + 'q': return x == y;
    case 'n' * 256 + 'e': return x != y;
    case 'l' * 256 + 't': return x < y;
    case 'l' * 256 + 'e': return x <= y;
    case 'g' * 256 + 't': return x > y;
    case 'g' * 256 + 'e': return x >= y;
    }
    return 0;
}

static int t_or(void);

static int t_primary(void) {
    if (t_i >= t_n) {
        if (!t_bad) say("test: argument expected");
        t_bad = 1;
        return 0;
    }
    const char* s = t_v[t_i];
    if (!strcmp(s, "!")) {
        t_i++;
        return !t_primary();
    }
    if (!strcmp(s, "(") && t_i + 1 < t_n) {
        t_i++;
        int r = t_or();
        if (t_i >= t_n || strcmp(t_v[t_i], ")")) {
            if (!t_bad) say("test: ')' expected");
            t_bad = 1;
            return 0;
        }
        t_i++;
        return r;
    }
    if (t_i + 2 < t_n && t_isbinary(t_v[t_i + 1])) {
        int r = t_binary(s, t_v[t_i + 1], t_v[t_i + 2]);
        t_i += 3;
        return r;
    }
    if (t_isunary(s) && t_i + 1 < t_n) {
        int r = t_unary(s[1], t_v[t_i + 1]);
        t_i += 2;
        return r;
    }
    t_i++;
    return s[0] != 0;
}

static int t_and(void) {
    int r = t_primary();
    while (!t_bad && t_i < t_n && !strcmp(t_v[t_i], "-a")) {
        t_i++;
        int r2 = t_primary();
        r = r && r2;
    }
    return r;
}

static int t_or(void) {
    int r = t_and();
    while (!t_bad && t_i < t_n && !strcmp(t_v[t_i], "-o")) {
        t_i++;
        int r2 = t_and();
        r = r || r2;
    }
    return r;
}

static int t_not(int st) { return st == 2 ? 2 : !st; }

// The exit status for the test v[0..n), by POSIX's rules for up to four
// arguments, and the grammar beyond.
static int t_eval(int n, char** v) {
    int r;
    t_bad = 0;
    switch (n) {
    case 0: return 1;
    case 1: return v[0][0] ? 0 : 1;
    case 2:
        if (!strcmp(v[0], "!")) return v[1][0] ? 1 : 0;
        if (t_isunary(v[0])) {
            r = t_unary(v[0][1], v[1]);
            return t_bad ? 2 : !r;
        }
        say("test: %s: unary operator expected", v[0]);
        return 2;
    case 3:
        if (t_isbinary(v[1])) {
            r = t_binary(v[0], v[1], v[2]);
            return t_bad ? 2 : !r;
        }
        if (!strcmp(v[1], "-a")) return !(v[0][0] && v[2][0]);
        if (!strcmp(v[1], "-o")) return !(v[0][0] || v[2][0]);
        if (!strcmp(v[0], "!")) return t_not(t_eval(2, v + 1));
        if (!strcmp(v[0], "(") && !strcmp(v[2], ")")) return t_eval(1, v + 1);
        break;
    case 4:
        if (!strcmp(v[0], "!")) return t_not(t_eval(3, v + 1));
        if (!strcmp(v[0], "(") && !strcmp(v[3], ")")) return t_eval(2, v + 1);
        break;
    }
    t_v = v;
    t_n = n;
    t_i = 0;
    t_bad = 0;
    r = t_or();
    if (!t_bad && t_i < t_n) {
        say("test: %s: unexpected operator", t_v[t_i]);
        return 2;
    }
    return t_bad ? 2 : !r;
}

static int b_test(int argc, char** argv) {
    if (!strcmp(argv[0], "[")) {
        if (strcmp(argv[argc - 1], "]")) {
            say("[: missing ']'");
            return 2;
        }
        argc--;
    }
    return t_eval(argc - 1, argv + 1);
}

static int b_exit(int argc, char** argv) {
    int st = S.last;
    if (argc > 1) {
        int64_t v;
        if (parse_int(argv[1], &v)) {
            say("exit: %s: not a number", argv[1]);
            st = 2;
        } else {
            st = (int)(v & 255);
        }
    }
    S.ctl = CTL_EXIT;
    S.exit_status = st;
    return st;
}

static int b_return(int argc, char** argv) {
    if (!S.fdepth && !S.dotdepth) {
        say("return: not in a function");
        return 1;
    }
    int st = S.last;
    if (argc > 1) {
        int64_t v;
        if (parse_int(argv[1], &v)) {
            say("return: %s: not a number", argv[1]);
            st = 2;
        } else {
            st = (int)(v & 255);
        }
    }
    S.ctl = CTL_RETURN;
    S.retval = st;
    return st;
}

static int loop_cmd(int argc, char** argv, int which) {
    int64_t n = 1;
    if (argc > 1 && (parse_int(argv[1], &n) || n < 1)) {
        say("%s: %s: not a count", argv[0], argv[1]);
        sh_abort(2);
        return 2;
    }
    if (!S.loops) return 0;
    if (n > S.loops) n = S.loops;
    S.ctl = which;
    S.ctln = (int)n;
    return 0;
}

static int b_break(int argc, char** argv) { return loop_cmd(argc, argv, CTL_BREAK); }
static int b_continue(int argc, char** argv) { return loop_cmd(argc, argv, CTL_CONTINUE); }

static int b_shift(int argc, char** argv) {
    int64_t n = 1;
    if (argc > 1 && (parse_int(argv[1], &n) || n < 0)) {
        say("shift: %s: not a count", argv[1]);
        sh_abort(2);
        return 2;
    }
    if (n > S.pc) {
        say("shift: can't shift that many");
        sh_abort(2);
        return 2;
    }
    for (int i = 0; i < n; i++) free(S.pv[i]);
    memmove(S.pv, S.pv + n, (size_t)(S.pc - n + 1) * sizeof *S.pv);
    S.pc -= (int)n;
    return 0;
}

static const char opt_letters[] = "eufx";
static const char* const opt_names[] = { "errexit", "nounset", "noglob", "xtrace" };

static int* opt_ref(char c) {
    switch (c) {
    case 'e': return &S.opt_e;
    case 'u': return &S.opt_u;
    case 'f': return &S.opt_f;
    case 'x': return &S.opt_x;
    }
    return 0;
}

static int var_cmp(const void* a, const void* b) { return strcmp((*(var_t* const*)a)->name, (*(var_t* const*)b)->name); }

// The variables, sorted, as commands that would set them again.
static void list_vars(const char* prefix, int exported_only) {
    var_t** v = 0;
    int n = 0, cap = 0;
    for (int h = 0; h < NBUCKETS; h++) {
        for (var_t* x = vars[h]; x; x = x->next) {
            if (exported_only && !x->exported) continue;
            if (n == cap) v = xrealloc(v, (size_t)(cap = cap * 2 + 16) * sizeof *v);
            v[n++] = x;
        }
    }
    for (int i = 1; i < n; i++) {  // few enough to sort by insertion
        var_t* t = v[i];
        int j = i;
        while (j > 0 && var_cmp(&v[j - 1], &t) > 0) {
            v[j] = v[j - 1];
            j--;
        }
        v[j] = t;
    }
    str_t o = { 0, 0, 0 };
    for (int i = 0; i < n; i++) {
        sputs(&o, prefix);
        sputs(&o, v[i]->name);
        sputc(&o, '=');
        put_quoted(&o, v[i]->val);
        sputc(&o, '\n');
    }
    if (o.n) bout(1, o.s, o.n);
    free(o.s);
    free(v);
}

static int b_set(int argc, char** argv) {
    if (argc == 1) {
        list_vars("", 0);
        return 0;
    }
    int a = 1, params = 0;
    for (; a < argc; a++) {
        char* s = argv[a];
        if ((s[0] != '-' && s[0] != '+') || !s[1]) {
            if (!strcmp(s, "-")) {  // ends the options
                a++;
                params = a < argc;
            }
            break;
        }
        if (!strcmp(s, "--")) {
            a++;
            params = 1;
            break;
        }
        int on = s[0] == '-';
        for (int i = 1; s[i]; i++) {
            if (s[i] == 'o') {
                if (a + 1 >= argc) {
                    str_t o = { 0, 0, 0 };
                    for (int k = 0; k < 4; k++) {
                        char line[40];
                        snprintf(line, sizeof line, "%-10s %s\n", opt_names[k], *opt_ref(opt_letters[k]) ? "on" : "off");
                        sputs(&o, line);
                    }
                    bout(1, o.s, o.n);
                    free(o.s);
                    continue;
                }
                const char* name = argv[++a];
                int k = 0;
                while (k < 4 && strcmp(name, opt_names[k])) k++;
                if (k == 4) {
                    say("set: %s: no such option", name);
                    sh_abort(2);
                    return 2;
                }
                *opt_ref(opt_letters[k]) = on;
                continue;
            }
            int* p = opt_ref(s[i]);
            if (!p) {
                say("set: %c%c: no such option", s[0], s[i]);
                sh_abort(2);
                return 2;
            }
            *p = on;
        }
    }
    if (a < argc) params = 1;
    if (params) set_params(argv + a, argc - a);
    return 0;
}

static int b_unset(int argc, char** argv) {
    int a = 1, fn = 0, st = 0;
    for (; a < argc && argv[a][0] == '-'; a++) {
        if (!strcmp(argv[a], "--")) {
            a++;
            break;
        }
        if (!strcmp(argv[a], "-f")) fn = 1;
        else if (!strcmp(argv[a], "-v")) fn = 0;
        else {
            say("unset: %s: no such option", argv[a]);
            return 2;
        }
    }
    for (; a < argc; a++) {
        if (fn) {
            fremove(argv[a]);
        } else if (!is_name(argv[a], (int)strlen(argv[a]))) {
            say("unset: %s: not a variable name", argv[a]);
            st = 1;
        } else {
            vunset(argv[a]);
        }
    }
    return st;
}

static int is_ifs(const char* sep, char c) { return c && strchr(sep, c); }

static int b_read(int argc, char** argv) {
    int a = 1, raw = 0;
    for (; a < argc && argv[a][0] == '-' && argv[a][1]; a++) {
        if (!strcmp(argv[a], "--")) {
            a++;
            break;
        }
        if (strcmp(argv[a], "-r")) {
            say("read: %s: no such option", argv[a]);
            return 2;
        }
        raw = 1;
    }
    for (int k = a; k < argc; k++) {
        if (!is_name(argv[k], (int)strlen(argv[k]))) {
            say("read: %s: not a variable name", argv[k]);
            return 2;
        }
    }
    str_t line = { 0, 0, 0 }, esc = { 0, 0, 0 };  // esc: which characters a backslash kept as they are
    sput(&line, "", 0);
    sput(&esc, "", 0);
    int eof = 0;
    for (;;) {
        char c;
        int r = shos_read(0, &c, 1);
        if (r == -E_INTR) {
            if (sh_interrupted) {
                eof = 1;
                break;
            }
            continue;
        }
        if (r <= 0) {
            eof = 1;
            break;
        }
        if (c == '\n') break;
        if (c == '\\' && !raw) {
            do r = shos_read(0, &c, 1);
            while (r == -E_INTR && !sh_interrupted);
            if (r <= 0) {
                eof = 1;
                break;
            }
            if (c == '\n') continue;
            sputc(&line, c);
            sputc(&esc, 1);
            continue;
        }
        if (!c) continue;
        sputc(&line, c);
        sputc(&esc, 0);
    }
    const char* s = line.s;
    const char* e = esc.s;
    int n = line.n;
    if (a == argc) {
        vset("REPLY", s);
    } else {
        const char* sep = ifs();
#define SEP(k) (!e[k] && is_ifs(sep, s[k]))
#define WS(k) (SEP(k) && is_ifs_ws(s[k]))
        int i = 0;
        while (i < n && WS(i)) i++;
        for (int k = a; k < argc; k++) {
            int from = i, to;
            if (k == argc - 1) {
                to = n;
                while (to > from && WS(to - 1)) to--;
                i = n;
            } else {
                while (i < n && !SEP(i)) i++;
                to = i;
                while (i < n && WS(i)) i++;
                if (i < n && SEP(i)) {
                    i++;
                    while (i < n && WS(i)) i++;
                }
            }
            char* v = xstrndup(s + from, (size_t)(to - from));
            vset(argv[k], v);
            free(v);
        }
#undef SEP
#undef WS
    }
    free(line.s);
    free(esc.s);
    return eof ? 1 : 0;
}

static int read_file(const char* path, str_t* out) {
    int fd = shos_open(path, SHO_READ);
    if (fd < 0) return fd;
    read_all(fd, out);
    shos_close(fd);
    return 0;
}

static int b_dot(int argc, char** argv) {
    if (argc < 2) {
        say(".: needs a file name");
        sh_abort(2);
        return 2;
    }
    if (S.dotdepth >= MAX_CALLS) {
        say(".: files read each other too deeply");
        sh_abort(2);
        return 2;
    }
    str_t t = { 0, 0, 0 };
    int e = read_file(argv[1], &t);
    if (e < 0) {
        say(".: %s: %s", argv[1], shos_strerror(argv[1], -e));
        sh_abort(2);
        return 2;
    }
    char** opv = S.pv;
    int opc = S.pc, swap = argc > 2;
    if (swap) {
        S.pv = 0;
        S.pc = 0;
        set_params(argv + 2, argc - 2);
    }
    S.dotdepth++;
    S.last = 0;
    run_text(t.s ? t.s : "", t.n, 1, 0, 0);
    S.dotdepth--;
    int st = S.last;
    if (S.ctl == CTL_RETURN) {
        S.ctl = CTL_NONE;
        st = S.retval;
    }
    if (swap) {
        free_params();
        S.pv = opv;
        S.pc = opc;
    }
    free(t.s);
    return st;
}

static int b_eval(int argc, char** argv) {
    str_t t = { 0, 0, 0 };
    for (int a = 1; a < argc; a++) {
        if (a > 1) sputc(&t, ' ');
        sputs(&t, argv[a]);
    }
    S.last = 0;
    int n = t.n;
    char* text = n ? adup(t.s, n) : 0;
    free(t.s);
    if (n) run_text(text, n, 1, 0, 0);
    return S.last;
}

static int b_local(int argc, char** argv) {
    if (!S.frame) {
        say("local: not in a function");
        return 2;
    }
    for (int a = 1; a < argc; a++) {
        char* s = argv[a];
        int eq = 0;
        while (s[eq] && s[eq] != '=') eq++;
        if (!is_name(s, eq)) {
            say("local: %s: not a variable name", s);
            return 2;
        }
        save_var(S.frame, s, eq);
        if (s[eq]) vsetn(s, eq, s + eq + 1);
    }
    return 0;
}

static int b_export(int argc, char** argv) {
    int a = 1, st = 0;
    if (a < argc && !strcmp(argv[a], "-p")) a++;
    if (a >= argc) {
        list_vars("export ", 1);
        return 0;
    }
    for (; a < argc; a++) {
        char* s = argv[a];
        int eq = 0;
        while (s[eq] && s[eq] != '=') eq++;
        if (!is_name(s, eq)) {
            say("export: %s: not a variable name", s);
            st = 1;
            continue;
        }
        if (s[eq]) vsetn(s, eq, s + eq + 1);
        var_t* v = vfind(s, eq);
        if (v) v->exported = 1;
    }
    return st;
}

static void bg_forget(int k) {
    memmove(S.bgids + k, S.bgids + k + 1, (size_t)(S.nbg - k - 1) * sizeof *S.bgids);
    S.nbg--;
}

static int b_wait(int argc, char** argv) {
    if (argc == 1) {
        while (S.nbg && !stop()) {
            int id = S.bgids[0];
            bg_forget(0);
            wait_task(id, 0, 1);
        }
        return 0;
    }
    int st = 0;
    for (int a = 1; a < argc && !stop(); a++) {
        int64_t id;
        if (parse_int(argv[a], &id) || id <= 0) {
            say("wait: %s: not a task id", argv[a]);
            st = 2;
            continue;
        }
        int k = 0;
        while (k < S.nbg && S.bgids[k] != id) k++;
        if (k == S.nbg) {
            st = 127;
            continue;
        }
        bg_forget(k);
        st = wait_task((int)id, 0, 1);
    }
    return st;
}

static int b_cd(int argc, char** argv) {
    const char* dir = argc > 1 ? argv[1] : "/";
    int show = 0;
    if (!strcmp(dir, "-")) {
        dir = vget("OLDPWD");
        if (!dir) {
            say("cd: OLDPWD not set");
            return 1;
        }
        show = 1;
    }
    char old[PBUF], cwd[PBUF + 1];
    if (shos_getcwd(old, sizeof old) < 0) old[0] = 0;
    int r = shos_chdir(dir);
    if (r < 0) {
        say("cd: %s: %s", dir, shos_strerror(dir, -r));
        return 1;
    }
    if (old[0]) vset("OLDPWD", old);
    if (shos_getcwd(cwd, PBUF) >= 0) {
        vset("PWD", cwd);
        if (show) {
            size_t n = strlen(cwd);
            cwd[n] = '\n';
            bout(1, cwd, (int)n + 1);
        }
    }
    return 0;
}

static int b_exec(int argc, char** argv) {
    if (argc < 2) return 0;  // (the redirections stay: run_builtin)
    int st = run_argv(argv + 1);
    if (!S.ctl) {
        S.ctl = CTL_EXIT;
        S.exit_status = st;
    }
    return st;
}

static int b_run(int argc, char** argv) {
    if (argc < 2) {
        say("run: needs a program");
        return 2;
    }
    return run_argv(argv + 1);
}

static const sh_builtin_t core[] = {
    { ":", SHB_PURE | SHB_SPECIAL, b_true, "", "do nothing, successfully" },
    { ".", SHB_SPECIAL, b_dot, "<file> [args]", "run the commands in a file, in this shell" },
    { "[", SHB_PURE, b_test, "<test> ]", "the same as test" },
    { "break", SHB_SPECIAL, b_break, "[n]", "leave the loop (or n loops)" },
    { "cd", 0, b_cd, "[dir]", "change directory (alone: the root; -: the one before)" },
    { "continue", SHB_SPECIAL, b_continue, "[n]", "go on with the loop's next round" },
    { "echo", SHB_PURE, b_echo, "[-neE] [text]", "write the words" },
    { "eval", SHB_SPECIAL, b_eval, "[text]", "run the words as commands" },
    { "exec", SHB_SPECIAL, b_exec, "[prog [args]]", "run a program and end; alone, keep its redirections" },
    { "exit", SHB_SPECIAL, b_exit, "[status]", "end this shell" },
    { "export", SHB_SPECIAL, b_export, "[name[=value]]", "mark variables (there is no environment)" },
    { "false", SHB_PURE, b_false, "", "fail" },
    { "local", SHB_SPECIAL, b_local, "name[=value]", "a variable of the function's own" },
    { "printf", SHB_PURE, b_printf, "<format> [args]", "write formatted text" },
    { "pwd", SHB_PURE, b_pwd, "", "show the current directory" },
    { "read", 0, b_read, "[-r] [names]", "read a line into variables" },
    { "return", SHB_SPECIAL, b_return, "[status]", "leave the function" },
    { "run", 0, b_run, "<prog> [args]", "run a program, not a command of the same name" },
    { "set", SHB_SPECIAL, b_set, "[-euxf] [-o opt] [-- args]", "set options or the arguments; alone, list variables" },
    { "shift", SHB_SPECIAL, b_shift, "[n]", "drop the first arguments" },
    { "test", SHB_PURE, b_test, "<test>", "check files, text and numbers" },
    { "true", SHB_PURE, b_true, "", "succeed" },
    { "unset", SHB_SPECIAL, b_unset, "[-f] names", "forget variables (-f: functions)" },
    { "wait", 0, b_wait, "[ids]", "wait for commands started with &" },
};

static const sh_builtin_t* core_find(const char* name) {
    for (size_t i = 0; i < sizeof core / sizeof *core; i++)
        if (!strcmp(core[i].name, name)) return &core[i];
    return 0;
}

static const sh_builtin_t* ext_find(const char* name) {
    if (!S.front) return 0;
    for (int i = 0; i < S.front->nbuiltins; i++)
        if (!strcmp(S.front->builtins[i].name, name)) return &S.front->builtins[i];
    return 0;
}

const sh_builtin_t* sh_core_builtins(int* n) {
    *n = (int)(sizeof core / sizeof *core);
    return core;
}

// ---- The shell as a whole ----

static int finish(void) {
    int st = S.last;
    if (S.ctl == CTL_EXIT) st = S.exit_status;
    else if (S.ctl == CTL_ABORT) st = S.abort_status;
    else if (S.ctl == CTL_RETURN) st = S.retval;
    return st & 255;
}

// sh -S: everything comes over descriptor 3 (spawn_shell).
static int child_main(void) {
    str_t in = { 0, 0, 0 };
    read_all(3, &in);
    shos_close(3);
    const char* text = "";
    int tn = 0, i = 0;
    argv_t pv = { 0, 0, 0 };
    while (i < in.n) {
        char type = in.s[i++];
        int64_t len = 0;
        while (i < in.n && is_digit((unsigned char)in.s[i]) && len < (1 << 30)) len = len * 10 + (in.s[i++] - '0');
        if (i >= in.n || in.s[i] != ':' || len > in.n - i - 1) break;
        i++;
        const char* d = in.s + i;
        int n = (int)len, eq = 0;
        i += n;
        while (eq < n && d[eq] != '=') eq++;
        switch (type) {
        case 'v':
            if (eq < n) {
                char* v = xstrndup(d + eq + 1, (size_t)(n - eq - 1));
                vsetn(d, eq, v);
                free(v);
            }
            break;
        case 'f':
            if (eq < n) fdefine(d, eq, xstrndup(d + eq + 1, (size_t)(n - eq - 1)), n - eq - 1);
            break;
        case 'p': av_add(&pv, xstrndup(d, (size_t)n)); break;
        case '0':
            free(S.arg0);
            S.arg0 = xstrndup(d, (size_t)n);
            break;
        case 'o':
            for (int k = 0; k < n; k++) {
                int* p = opt_ref(d[k]);
                if (p) *p = 1;
                else if (d[k] == 'N') S.noerrexit = 1;
                else if (d[k] == 'i') S.ui = 1;
                else if (d[k] == 'F') S.fdepth = 1;
            }
            break;
        case '?':
        case '$':
        case '!': {
            int64_t v = 0;
            for (int k = 0; k < n && is_digit((unsigned char)d[k]); k++) v = v * 10 + (d[k] - '0');
            if (type == '?') S.last = (int)v;
            else if (type == '$') S.pid = (int)v;
            else S.bg = (int)v;
            break;
        }
        case 'c':
            text = d;
            tn = n;
            break;
        case 'w':  // only write this out (hd_pipe)
            write_all(1, d, n);
            free(in.s);
            return 0;
        }
    }
    set_params(pv.v, pv.n);
    for (int k = 0; k < pv.n; k++) free(pv.v[k]);
    free(pv.v);
    run_text(text, tn, 1, 0, 0);
    free(in.s);
    return finish();
}

int sh_main(const sh_front_t* front, int argc, char** argv) {
    S.front = front;
    S.pid = shos_getpid();
    S.arg0 = xstrdup(argc > 0 && argv[0] ? argv[0] : "sh");
    set_params(0, 0);
    int a = 1, cmd = 0, child = 0, from_stdin = 0, interactive = 0;
    for (; a < argc; a++) {
        char* s = argv[a];
        if ((s[0] != '-' && s[0] != '+') || !s[1]) {
            if (!strcmp(s, "-")) a++;
            break;
        }
        if (!strcmp(s, "--")) {
            a++;
            break;
        }
        int on = s[0] == '-';
        for (int i = 1; s[i]; i++) {
            int* p = opt_ref(s[i]);
            if (p) *p = on;
            else if (s[i] == 'c') cmd = 1;
            else if (s[i] == 'S') child = 1;
            else if (s[i] == 's') from_stdin = 1;
            else if (s[i] == 'i') interactive = 1;
            else {
                say("%c%c: no such option", s[0], s[i]);
                return 2;
            }
        }
    }
    if (child) return child_main();
    if (cmd) {
        if (a >= argc) {
            say("-c: needs the commands to run");
            return 2;
        }
        const char* text = argv[a++];
        if (a < argc) {
            free(S.arg0);
            S.arg0 = xstrdup(argv[a++]);
        }
        set_params(argv + a, argc - a);
        run_text(text, (int)strlen(text), 1, 0, 0);
        return finish();
    }
    if (a < argc && !from_stdin) {
        str_t t = { 0, 0, 0 };
        int e = read_file(argv[a], &t);
        if (e < 0) {
            say("%s: %s", argv[a], shos_strerror(argv[a], -e));
            return 127;
        }
        free(S.arg0);
        S.arg0 = xstrdup(argv[a++]);
        set_params(argv + a, argc - a);
        run_text(t.s ? t.s : "", t.n, 1, 0, 0);
        free(t.s);
        return finish();
    }
    set_params(argv + a, argc - a);
    if (interactive || (!from_stdin && shos_isatty(0))) {
        S.interactive = 1;
        S.ui = 1;
        return SH_INTERACTIVE;
    }
    // Commands from standard input, read a line at a time so that what
    // they read themselves (read, a program) comes after them.
    str_t buf = { 0, 0, 0 };
    int skip = 0;
    for (;;) {
        int eof = 1;
        for (;;) {
            char c;
            int r = shos_read(0, &c, 1);
            if (r == -E_INTR) continue;
            if (r <= 0) break;
            sputc(&buf, c);
            if (c == '\n') {
                eof = 0;
                break;
            }
        }
        if (buf.n > skip) {
            int more_at = 0;
            int r = run_text(buf.s + skip, buf.n - skip, eof, &more_at, 0);
            if (S.ctl) break;
            if (r == P_MORE) {
                skip += more_at;
            } else {
                buf.n = 0;
                skip = 0;
            }
        }
        if (eof) break;
    }
    free(buf.s);
    return finish();
}

int sh_feed(const char* text) {
    if (!text) {
        S.feed_skip = 0;
        return SH_DONE;
    }
    int n = (int)strlen(text);
    if (S.feed_skip > n) S.feed_skip = 0;
    sh_interrupted = 0;
    int more_at = 0;
    int r = run_text(text + S.feed_skip, n - S.feed_skip, 0, &more_at, 1);
    if (r == P_MORE && !S.ctl) {
        S.feed_skip += more_at;
        return SH_MORE;
    }
    S.feed_skip = 0;
    if (S.ctl == CTL_EXIT) {
        S.exit_asked = 1;
    } else {
        if (S.ctl == CTL_ABORT) S.last = S.abort_status;
        S.ctl = CTL_NONE;
    }
    sh_interrupted = 0;
    return SH_DONE;
}

int sh_exit_asked(int* status) {
    if (!S.exit_asked) return 0;
    *status = S.exit_status & 255;
    return 1;
}
