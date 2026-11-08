// Regular expressions: see regex.h. A pattern becomes a tree, the tree
// becomes a short program for a simulated automaton, and a match runs
// every possible position of the program side by side, one character at
// a time, so the work is the length of the line times the size of the
// program whatever the pattern.
#include <stddef.h>
#include <stdint.h>
#include "regex.h"

void* malloc(size_t size);
void* realloc(void* p, size_t size);
void free(void* p);
void* memset(void* dst, int c, size_t n);
void* memcpy(void* dst, const void* src, size_t n);

#define MAX_INSTS 30000  // a program longer than this is "too big"
#define MAX_DEPTH 200    // nested groups
#define MAX_REPEAT 1000

// ---- the program ----
enum { I_CHAR, I_ANY, I_SET, I_SPLIT, I_JMP, I_BOL, I_EOL, I_WORDB, I_NWORDB, I_WSTART, I_WEND, I_MATCH };

typedef struct {
    uint8_t op, c;
    uint16_t x, y;  // I_SET: x is the set; I_SPLIT and I_JMP: where to go
} inst_t;

typedef uint8_t set_t[32];

// ---- the tree ----
enum { N_EMPTY, N_CHAR, N_ANY, N_SET, N_ASSERT, N_SEQ, N_ALT, N_STAR, N_PLUS, N_QUEST, N_REP };

typedef struct {
    uint8_t type, c;
    uint16_t set;
    int32_t a;     // N_SEQ, N_ALT: the first child; the repeats: the child
    int32_t nx;    // the next sibling in a N_SEQ or N_ALT
    int32_t min, max;
    uint8_t what;  // N_ASSERT: the I_ op
} node_t;

typedef struct {
    const char* s;
    uint32_t i, len;
    unsigned flags;
    node_t* nodes;
    uint32_t nn, ncap;
    const char* err;
    re_t* re;
} parser_t;

static int fail(parser_t* p, const char* msg) {
    if (!p->err) p->err = msg;
    return -1;
}

static uint8_t lower(uint8_t c) { return c >= 'A' && c <= 'Z' ? (uint8_t)(c + 32) : c; }
static int is_word(uint8_t c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int32_t new_node(parser_t* p, int type) {
    if (p->nn == p->ncap) {
        uint32_t want = p->ncap ? p->ncap * 2 : 64;
        node_t* q = realloc(p->nodes, want * sizeof *q);
        if (!q) return fail(p, "out of memory");
        p->nodes = q;
        p->ncap = want;
    }
    node_t* n = &p->nodes[p->nn];
    memset(n, 0, sizeof *n);
    n->type = (uint8_t)type;
    n->a = n->nx = -1;
    return (int32_t)p->nn++;
}

static int32_t leaf(parser_t* p, int type, int c) {
    int32_t n = new_node(p, type);
    if (n >= 0) p->nodes[n].c = (uint8_t)c;
    return n;
}

static int32_t assertion(parser_t* p, int op) {
    int32_t n = new_node(p, N_ASSERT);
    if (n >= 0) p->nodes[n].what = (uint8_t)op;
    return n;
}

// ---- sets ----
static int32_t new_set(parser_t* p) {
    re_t* re = p->re;
    if (re->nsets == re->setcap) {
        uint32_t want = re->setcap ? re->setcap * 2 : 8;
        void* q = realloc(re->sets, want * sizeof(set_t));
        if (!q) return fail(p, "out of memory");
        re->sets = q;
        re->setcap = want;
    }
    memset((set_t*)re->sets + re->nsets, 0, sizeof(set_t));
    return (int32_t)re->nsets++;
}

static void set_add(set_t s, unsigned c) { s[c >> 3] |= (uint8_t)(1u << (c & 7)); }
static int set_has(const set_t s, unsigned c) { return (s[c >> 3] >> (c & 7)) & 1; }

static int memcmp_(const char* a, const char* b, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        if (a[i] != b[i]) return (uint8_t)a[i] - (uint8_t)b[i];
    return 0;
}

static int in_class(const char* name, uint32_t n, unsigned c) {
    int alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    int digit = c >= '0' && c <= '9';
#define IS(s) (n == sizeof(s) - 1 && !memcmp_(name, s, n))
    if (IS("alpha")) return alpha;
    if (IS("digit")) return digit;
    if (IS("alnum")) return alpha || digit;
    if (IS("upper")) return c >= 'A' && c <= 'Z';
    if (IS("lower")) return c >= 'a' && c <= 'z';
    if (IS("space")) return c == ' ' || (c >= 9 && c <= 13);
    if (IS("blank")) return c == ' ' || c == '\t';
    if (IS("punct")) return c > 32 && c < 127 && !alpha && !digit;
    if (IS("print")) return c >= 32 && c < 127;
    if (IS("graph")) return c > 32 && c < 127;
    if (IS("cntrl")) return c < 32 || c == 127;
    if (IS("xdigit")) return digit || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
#undef IS
    return -1;
}

// ---- parsing ----
static void set_fold(set_t s) {
    for (unsigned c = 'a'; c <= 'z'; c++) {
        if (set_has(s, c) || set_has(s, c - 32)) {
            set_add(s, c);
            set_add(s, c - 32);
        }
    }
}

static void set_negate(set_t s) {
    for (int i = 0; i < 32; i++) s[i] = (uint8_t)~s[i];
}

// [...], with p->i just past the "[".
static int32_t parse_bracket(parser_t* p) {
    int32_t si = new_set(p);
    if (si < 0) return -1;
    int neg = 0, first = 1;
    if (p->i < p->len && p->s[p->i] == '^') {
        neg = 1;
        p->i++;
    }
    for (;;) {
        if (p->i >= p->len) return fail(p, "Unmatched [, [^, [:, [., or [=");
        uint8_t c = (uint8_t)p->s[p->i];
        if (c == ']' && !first) {
            p->i++;
            break;
        }
        first = 0;
        set_t* set = (set_t*)p->re->sets + si;
        if (c == '[' && p->i + 1 < p->len && p->s[p->i + 1] == ':') {
            uint32_t j = p->i + 2;
            while (j + 1 < p->len && !(p->s[j] == ':' && p->s[j + 1] == ']')) j++;
            if (j + 1 >= p->len) return fail(p, "Unmatched [, [^, [:, [., or [=");
            const char* name = p->s + p->i + 2;
            uint32_t n = j - (p->i + 2);
            if (in_class(name, n, 0) < 0 && in_class(name, n, 'a') < 0) return fail(p, "Invalid character class name");
            for (unsigned k = 0; k < 256; k++)
                if (in_class(name, n, k) > 0) set_add(*set, k);
            p->i = j + 2;
            continue;
        }
        if (c == '[' && p->i + 1 < p->len && (p->s[p->i + 1] == '.' || p->s[p->i + 1] == '='))
            return fail(p, "collating symbols and equivalence classes are not supported");
        p->i++;
        uint8_t hi = c;
        if (p->i + 1 < p->len && p->s[p->i] == '-' && p->s[p->i + 1] != ']') {
            hi = (uint8_t)p->s[p->i + 1];
            p->i += 2;
            if (hi < c) return fail(p, "Invalid range end");
        }
        for (unsigned k = c; k <= hi; k++) set_add(*set, k);
    }
    set_t* set = (set_t*)p->re->sets + si;
    if (p->flags & RE_ICASE) set_fold(*set);
    if (neg) set_negate(*set);
    int32_t n = new_node(p, N_SET);
    if (n >= 0) p->nodes[n].set = (uint16_t)si;
    return n;
}

// \w \W \s \S as a set.
static int32_t class_node(parser_t* p, int word, int neg) {
    int32_t si = new_set(p);
    if (si < 0) return -1;
    set_t* set = (set_t*)p->re->sets + si;
    for (unsigned c = 0; c < 256; c++) {
        int in = word ? is_word((uint8_t)c) : (c == ' ' || (c >= 9 && c <= 13));
        if (in) set_add(*set, c);
    }
    if (neg) set_negate(*set);
    int32_t n = new_node(p, N_SET);
    if (n >= 0) p->nodes[n].set = (uint16_t)si;
    return n;
}

static int32_t parse_alt(parser_t* p, int depth);

static int ere(const parser_t* p) { return (p->flags & RE_ERE) != 0; }

static int at_alt(const parser_t* p) {
    if (p->i >= p->len) return 0;
    if (ere(p)) return p->s[p->i] == '|';
    return p->s[p->i] == '\\' && p->i + 1 < p->len && p->s[p->i + 1] == '|';
}

static int at_close(const parser_t* p) {
    if (p->i >= p->len) return 0;
    if (ere(p)) return p->s[p->i] == ')';
    return p->s[p->i] == '\\' && p->i + 1 < p->len && p->s[p->i + 1] == ')';
}

// A {m,n} (or \{m,n\}) with p->i at its first digit or comma. 1 and the
// limits and p->i moved past it, or 0 and nothing moved if it is not
// one. A basic-syntax one that is wrong is an error, -1.
static int interval(parser_t* p, int32_t* lo, int32_t* hi) {
    uint32_t i = p->i;
    int32_t a = -1, b = -1;
    int comma = 0;
    while (i < p->len && p->s[i] >= '0' && p->s[i] <= '9') {
        a = (a < 0 ? 0 : a) * 10 + (p->s[i++] - '0');
        if (a > 100000) a = 100000;
    }
    if (i < p->len && p->s[i] == ',') {
        comma = 1;
        i++;
        while (i < p->len && p->s[i] >= '0' && p->s[i] <= '9') {
            b = (b < 0 ? 0 : b) * 10 + (p->s[i++] - '0');
            if (b > 100000) b = 100000;
        }
    }
    int closed;
    if (ere(p)) {
        closed = i < p->len && p->s[i] == '}';
        if (closed) i++;
    } else {
        closed = i + 1 < p->len && p->s[i] == '\\' && p->s[i + 1] == '}';
        if (closed) i += 2;
    }
    if (!closed || (a < 0 && !comma)) return ere(p) ? 0 : fail(p, "Unmatched \\{");
    if (a < 0) a = 0;
    if (!comma) b = a;
    if (comma && b < 0) b = -1;  // {n,}: no top
    if (b >= 0 && a > b) return fail(p, "Invalid content of \\{\\}");
    if (a > MAX_REPEAT || b > MAX_REPEAT) return fail(p, "Regular expression too big");
    *lo = a;
    *hi = b;
    p->i = i;
    return 1;
}

static int32_t parse_atom(parser_t* p, int depth, int first, int* anchor) {
    uint8_t c = (uint8_t)p->s[p->i];
    *anchor = 0;
    if (ere(p)) {
        if (c == '(') {
            if (depth >= MAX_DEPTH) return fail(p, "Regular expression too big");
            p->i++;
            int32_t inner = parse_alt(p, depth + 1);
            if (inner < 0) return -1;
            if (p->i >= p->len || p->s[p->i] != ')') return fail(p, "Unmatched ( or \\(");
            p->i++;
            return inner;
        }
        if (c == '^') {
            p->i++;
            *anchor = 1;
            return assertion(p, I_BOL);
        }
        if (c == '$') {
            p->i++;
            return assertion(p, I_EOL);
        }
        // GNU: a * + ? with nothing before it repeats nothing, and is dropped.
        if (first && (c == '*' || c == '+' || c == '?')) {
            p->i++;
            return new_node(p, N_EMPTY);
        }
    } else {
        // A \+ \? \{ with nothing before it is just that character.
        if (first && c == '\\' && p->i + 1 < p->len && (p->s[p->i + 1] == '+' || p->s[p->i + 1] == '?' || p->s[p->i + 1] == '{')) {
            uint8_t lit = (uint8_t)p->s[p->i + 1];
            p->i += 2;
            return leaf(p, N_CHAR, lit);
        }
        if (c == '\\' && p->i + 1 < p->len && p->s[p->i + 1] == '(') {
            if (depth >= MAX_DEPTH) return fail(p, "Regular expression too big");
            p->i += 2;
            int32_t inner = parse_alt(p, depth + 1);
            if (inner < 0) return -1;
            if (!at_close(p)) return fail(p, "Unmatched ( or \\(");
            p->i += 2;
            return inner;
        }
        if (c == '^' && first == 1) {
            p->i++;
            *anchor = 1;
            return assertion(p, I_BOL);
        }
        if (c == '$') {
            uint32_t j = p->i + 1;
            int end = j >= p->len || (p->s[j] == '\\' && j + 1 < p->len && (p->s[j + 1] == ')' || p->s[j + 1] == '|'));
            if (end) {
                p->i++;
                return assertion(p, I_EOL);
            }
        }
        if (c == '\\' && p->i + 1 < p->len && p->s[p->i + 1] == '{' && first)
            return fail(p, "Invalid preceding regular expression");
    }
    if (c == '.') {
        p->i++;
        return new_node(p, N_ANY);
    }
    if (c == '[') {
        p->i++;
        return parse_bracket(p);
    }
    if (c == '\\') {
        if (p->i + 1 >= p->len) return fail(p, "Trailing backslash");
        uint8_t e = (uint8_t)p->s[p->i + 1];
        p->i += 2;
        switch (e) {
        case 'w': return class_node(p, 1, 0);
        case 'W': return class_node(p, 1, 1);
        case 's': return class_node(p, 0, 0);
        case 'S': return class_node(p, 0, 1);
        case 'b': return assertion(p, I_WORDB);
        case 'B': return assertion(p, I_NWORDB);
        case '<': return assertion(p, I_WSTART);
        case '>': return assertion(p, I_WEND);
        default:
            if (e >= '1' && e <= '9') return fail(p, "back-references are not supported");
            return leaf(p, N_CHAR, e);
        }
    }
    p->i++;
    return leaf(p, N_CHAR, c);
}

static int32_t wrap(parser_t* p, int type, int32_t child, int32_t lo, int32_t hi) {
    node_t* ch = &p->nodes[child];
    // (a*)* is a*, (a+)* and (a?)* too: the same language, a shallower tree.
    if (ch->type == N_STAR) return child;
    if (type == N_STAR && (ch->type == N_PLUS || ch->type == N_QUEST)) child = ch->a;
    int32_t n = new_node(p, type);
    if (n < 0) return -1;
    p->nodes[n].a = child;
    p->nodes[n].min = lo;
    p->nodes[n].max = hi;
    return n;
}

static int32_t parse_seq(parser_t* p, int depth) {
    int32_t seq = new_node(p, N_SEQ);
    if (seq < 0) return -1;
    int32_t tail = -1;
    int first = 1;
    while (p->i < p->len && !at_alt(p) && !(at_close(p) && (depth > 0 || !ere(p)))) {
        int anchor;
        int32_t atom = parse_atom(p, depth, first, &anchor);
        if (atom < 0) return -1;
        // Operators after the atom.
        int stacked = 0;
        for (;;) {
            if (p->i >= p->len) break;
            uint8_t c = (uint8_t)p->s[p->i];
            int type = -1;
            int32_t lo = 0, hi = -1;
            if (anchor && ere(p) && (c == '*' || c == '+' || c == '?')) {  // ^* is ^ for GNU
                p->i++;
                continue;
            }
            if (c == '*' && !(anchor && !ere(p))) {
                type = N_STAR;
                p->i++;
            } else if (ere(p) && (c == '+' || c == '?')) {
                type = c == '+' ? N_PLUS : N_QUEST;
                p->i++;
            } else if (ere(p) && c == '{') {
                p->i++;
                int r = interval(p, &lo, &hi);
                if (r < 0) return -1;
                if (!r) {
                    p->i--;
                    break;
                }
                type = N_REP;
            } else if (!ere(p) && c == '\\' && p->i + 1 < p->len) {
                uint8_t e = (uint8_t)p->s[p->i + 1];
                if (e == '+' || e == '?') {
                    type = e == '+' ? N_PLUS : N_QUEST;
                    p->i += 2;
                } else if (e == '{') {
                    p->i += 2;
                    int r = interval(p, &lo, &hi);
                    if (r <= 0) return -1;
                    type = N_REP;
                } else {
                    break;
                }
            } else {
                break;
            }
            if (++stacked > 32) return fail(p, "Regular expression too big");
            atom = wrap(p, type, atom, lo, hi);
            if (atom < 0) return -1;
        }
        if (tail < 0) p->nodes[seq].a = atom;
        else p->nodes[tail].nx = atom;
        tail = atom;
        // A leading ^ keeps the context "first": in the basic syntax a
        // * straight after it is still a plain *.
        // (2 = after it: a second ^ is then a plain character).
        first = (anchor && first == 1 && !ere(p)) ? 2 : 0;
    }
    return seq;
}

static int32_t parse_alt(parser_t* p, int depth) {
    int32_t head = parse_seq(p, depth);
    if (head < 0) return -1;
    if (!at_alt(p)) return head;
    int32_t alt = new_node(p, N_ALT);
    if (alt < 0) return -1;
    p->nodes[alt].a = head;
    int32_t tail = head;
    while (at_alt(p)) {
        p->i += ere(p) ? 1 : 2;
        int32_t b = parse_seq(p, depth);
        if (b < 0) return -1;
        p->nodes[tail].nx = b;
        tail = b;
    }
    return alt;
}

// ---- code ----
static int push_inst(parser_t* p, int op, int c, uint32_t x, uint32_t y) {
    re_t* re = p->re;
    if (re->n >= MAX_INSTS) return fail(p, "Regular expression too big");
    if (re->n == re->cap) {
        uint32_t want = re->cap ? re->cap * 2 : 64;
        void* q = realloc(re->prog, want * sizeof(inst_t));
        if (!q) return fail(p, "out of memory");
        re->prog = q;
        re->cap = want;
    }
    inst_t* in = (inst_t*)re->prog + re->n;
    in->op = (uint8_t)op;
    in->c = (uint8_t)c;
    in->x = (uint16_t)x;
    in->y = (uint16_t)y;
    return (int)re->n++;
}

#define PROG(p) ((inst_t*)(p)->re->prog)
#define NONE 0xFFFF

static int emit(parser_t* p, int32_t ni, int depth);

// child, as a loop that may run any number of times.
static int emit_star(parser_t* p, int32_t child, int depth) {
    int s = push_inst(p, I_SPLIT, 0, 0, 0);
    if (s < 0) return -1;
    PROG(p)[s].x = (uint16_t)(s + 1);
    if (emit(p, child, depth + 1)) return -1;
    if (push_inst(p, I_JMP, 0, (uint32_t)s, 0) < 0) return -1;
    PROG(p)[s].y = (uint16_t)p->re->n;
    return 0;
}

static int emit(parser_t* p, int32_t ni, int depth) {
    node_t n = p->nodes[ni];  // a copy: emitting may move the pool
    switch (n.type) {
    case N_EMPTY: return 0;
    case N_CHAR: return push_inst(p, I_CHAR, (p->flags & RE_ICASE) ? lower(n.c) : n.c, 0, 0) < 0 ? -1 : 0;
    case N_ANY: return push_inst(p, I_ANY, 0, 0, 0) < 0 ? -1 : 0;
    case N_SET: return push_inst(p, I_SET, 0, n.set, 0) < 0 ? -1 : 0;
    case N_ASSERT: return push_inst(p, n.what, 0, 0, 0) < 0 ? -1 : 0;
    case N_SEQ:
        for (int32_t k = n.a; k >= 0; k = p->nodes[k].nx)
            if (emit(p, k, depth + 1)) return -1;
        return 0;
    case N_ALT: {
        uint32_t chain = NONE;
        for (int32_t k = n.a; k >= 0; k = p->nodes[k].nx) {
            int last = p->nodes[k].nx < 0;
            int s = -1;
            if (!last) {
                s = push_inst(p, I_SPLIT, 0, 0, 0);
                if (s < 0) return -1;
                PROG(p)[s].x = (uint16_t)(s + 1);
            }
            if (emit(p, k, depth + 1)) return -1;
            if (!last) {
                int j = push_inst(p, I_JMP, 0, chain, 0);
                if (j < 0) return -1;
                chain = (uint32_t)j;
                PROG(p)[s].y = (uint16_t)p->re->n;
            }
        }
        while (chain != NONE) {
            uint32_t next = PROG(p)[chain].x;
            PROG(p)[chain].x = (uint16_t)p->re->n;
            chain = next;
        }
        return 0;
    }
    case N_STAR: return emit_star(p, n.a, depth);
    case N_PLUS: {
        uint32_t start = p->re->n;
        if (emit(p, n.a, depth + 1)) return -1;
        return push_inst(p, I_SPLIT, 0, start, p->re->n + 1) < 0 ? -1 : 0;
    }
    case N_QUEST: {
        int s = push_inst(p, I_SPLIT, 0, 0, 0);
        if (s < 0) return -1;
        PROG(p)[s].x = (uint16_t)(s + 1);
        if (emit(p, n.a, depth + 1)) return -1;
        PROG(p)[s].y = (uint16_t)p->re->n;
        return 0;
    }
    case N_REP: {
        for (int32_t i = 0; i < n.min; i++)
            if (emit(p, n.a, depth + 1)) return -1;
        if (n.max < 0) return emit_star(p, n.a, depth);
        uint32_t chain = NONE;
        for (int32_t i = 0; i < n.max - n.min; i++) {
            int s = push_inst(p, I_SPLIT, 0, 0, chain);
            if (s < 0) return -1;
            PROG(p)[s].x = (uint16_t)(s + 1);
            chain = (uint32_t)s;
            if (emit(p, n.a, depth + 1)) return -1;
        }
        while (chain != NONE) {
            uint32_t next = PROG(p)[chain].y;
            PROG(p)[chain].y = (uint16_t)p->re->n;
            chain = next;
        }
        return 0;
    }
    }
    return 0;
}

const char* re_compile(re_t* re, const char* pat, uint32_t len, unsigned flags) {
    memset(re, 0, sizeof *re);
    re->flags = flags;
    parser_t p;
    memset(&p, 0, sizeof p);
    p.s = pat;
    p.len = len;
    p.flags = flags;
    p.re = re;

    int32_t root = new_node(&p, N_ALT), tail = -1;
    uint32_t start = 0;
    while (root >= 0 && !p.err) {
        uint32_t end = start;
        while (end < len && pat[end] != '\n') end++;
        int32_t one;
        if (flags & RE_FIXED) {
            one = new_node(&p, N_SEQ);
            int32_t t = -1;
            for (uint32_t k = start; one >= 0 && k < end; k++) {
                int32_t c = leaf(&p, N_CHAR, (uint8_t)pat[k]);
                if (c < 0) { one = -1; break; }
                if (t < 0) p.nodes[one].a = c;
                else p.nodes[t].nx = c;
                t = c;
            }
        } else {
            p.i = start;
            p.len = end;
            one = parse_alt(&p, 0);
            if (one >= 0 && p.i < end) one = fail(&p, "Unmatched ) or \\)");
            p.len = len;
        }
        if (one < 0) break;
        if (tail < 0) p.nodes[root].a = one;
        else p.nodes[tail].nx = one;
        tail = one;
        if (end >= len) break;
        start = end + 1;
    }
    if (!p.err && root >= 0 && emit(&p, root, 0) == 0 && push_inst(&p, I_MATCH, 0, 0, 0) >= 0) {
        uint32_t n = re->n;
        re->cur = malloc(n * sizeof *re->cur);
        re->nxt = malloc(n * sizeof *re->nxt);
        re->stack = malloc((2 * n + 4) * sizeof *re->stack);
        re->mark = malloc(n * sizeof *re->mark);
        if (!re->cur || !re->nxt || !re->stack || !re->mark) p.err = "out of memory";
        else memset(re->mark, 0, n * sizeof *re->mark);
    }
    free(p.nodes);
    if (p.err) {
        re_free(re);
        return p.err;
    }
    return 0;
}

void re_free(re_t* re) {
    free(re->prog);
    free(re->sets);
    free(re->cur);
    free(re->nxt);
    free(re->stack);
    free(re->mark);
    memset(re, 0, sizeof *re);
}

// ---- matching ----
typedef struct {
    const char* line;
    uint32_t len;
} subject_t;

static void next_gen(re_t* re) {
    if (++re->gen == 0) {
        memset(re->mark, 0, re->n * sizeof *re->mark);
        re->gen = 1;
    }
}

// Every program position reachable from pc without reading a character,
// at position pos of the line, that is waiting for one (or is the end).
static void add_thread(re_t* re, uint16_t* list, uint32_t* n, uint32_t pc0, const subject_t* sj, uint32_t pos) {
    const inst_t* prog = re->prog;
    uint32_t sp = 0;
    re->stack[sp++] = (uint16_t)pc0;
    while (sp) {
        uint32_t pc = re->stack[--sp];
        if (re->mark[pc] == re->gen) continue;
        re->mark[pc] = re->gen;
        const inst_t* in = &prog[pc];
        int before = pos > 0 && is_word((uint8_t)sj->line[pos - 1]);
        int after = pos < sj->len && is_word((uint8_t)sj->line[pos]);
        switch (in->op) {
        case I_JMP: re->stack[sp++] = in->x; break;
        case I_SPLIT:
            re->stack[sp++] = in->y;
            re->stack[sp++] = in->x;
            break;
        case I_BOL: if (pos == 0) re->stack[sp++] = (uint16_t)(pc + 1); break;
        case I_EOL: if (pos == sj->len) re->stack[sp++] = (uint16_t)(pc + 1); break;
        case I_WORDB: if (before != after) re->stack[sp++] = (uint16_t)(pc + 1); break;
        case I_NWORDB: if (before == after) re->stack[sp++] = (uint16_t)(pc + 1); break;
        case I_WSTART: if (!before && after) re->stack[sp++] = (uint16_t)(pc + 1); break;
        case I_WEND: if (before && !after) re->stack[sp++] = (uint16_t)(pc + 1); break;
        default: list[(*n)++] = (uint16_t)pc; break;
        }
    }
}

// Whether a match of line[start..end) is allowed by -w and -x.
static int accepts(const re_t* re, const subject_t* sj, uint32_t start, uint32_t end) {
    if ((re->flags & RE_LINE) && (start != 0 || end != sj->len)) return 0;
    if (re->flags & RE_WORD) {  // like GNU, an empty match counts if both sides are not word characters
        if (start > 0 && is_word((uint8_t)sj->line[start - 1])) return 0;
        if (end < sj->len && is_word((uint8_t)sj->line[end])) return 0;
    }
    return 1;
}

static int consumes(const inst_t* in, const subject_t* sj, uint32_t pos, const set_t* sets, unsigned flags) {
    if (pos >= sj->len) return 0;
    uint8_t c = (uint8_t)sj->line[pos];
    switch (in->op) {
    case I_CHAR: return ((flags & RE_ICASE) ? lower(c) : c) == in->c;  // in->c is already lower case when ignoring case
    case I_ANY: return 1;
    case I_SET: return set_has(sets[in->x], c);
    }
    return 0;
}

// Any match anywhere (no -w or -x): every start at once.
static int run_any(re_t* re, const subject_t* sj) {
    const inst_t* prog = re->prog;
    uint16_t *cur = re->cur, *nxt = re->nxt;
    uint32_t nc = 0, nn;
    next_gen(re);
    add_thread(re, cur, &nc, 0, sj, 0);
    for (uint32_t pos = 0;; pos++) {
        next_gen(re);
        nn = 0;
        for (uint32_t i = 0; i < nc; i++) {
            const inst_t* in = &prog[cur[i]];
            if (in->op == I_MATCH) return 1;
            if (consumes(in, sj, pos, re->sets, re->flags)) add_thread(re, nxt, &nn, cur[i] + 1u, sj, pos + 1);
        }
        if (pos == sj->len) return 0;
        add_thread(re, nxt, &nn, 0, sj, pos + 1);
        uint16_t* t = cur;
        cur = nxt;
        nxt = t;
        nc = nn;
    }
}

// The longest allowed match that starts exactly at start: its end, or -1.
static int64_t run_from(re_t* re, const subject_t* sj, uint32_t start) {
    const inst_t* prog = re->prog;
    uint16_t *cur = re->cur, *nxt = re->nxt;
    uint32_t nc = 0, nn;
    int64_t best = -1;
    next_gen(re);
    add_thread(re, cur, &nc, 0, sj, start);
    for (uint32_t pos = start; nc; pos++) {
        next_gen(re);
        nn = 0;
        for (uint32_t i = 0; i < nc; i++) {
            const inst_t* in = &prog[cur[i]];
            if (in->op == I_MATCH) {
                if (accepts(re, sj, start, pos)) best = pos;
            } else if (consumes(in, sj, pos, re->sets, re->flags)) {
                add_thread(re, nxt, &nn, cur[i] + 1u, sj, pos + 1);
            }
        }
        if (pos == sj->len) break;
        uint16_t* t = cur;
        cur = nxt;
        nxt = t;
        nc = nn;
    }
    return best;
}

int re_find(re_t* re, const char* line, uint32_t len, uint32_t from, uint32_t* ms, uint32_t* me) {
    subject_t sj = { line, len };
    for (uint32_t s = from; s <= len; s++) {
        if ((re->flags & RE_LINE) && s > 0) break;
        if ((re->flags & RE_WORD) && s > 0 && is_word((uint8_t)line[s - 1])) continue;
        int64_t e = run_from(re, &sj, s);
        if (e >= 0) {
            *ms = s;
            *me = (uint32_t)e;
            return 1;
        }
    }
    return 0;
}

int re_test(re_t* re, const char* line, uint32_t len) {
    if (re->flags & (RE_WORD | RE_LINE)) {
        uint32_t ms, me;
        return re_find(re, line, len, 0, &ms, &me);
    }
    subject_t sj = { line, len };
    return run_any(re, &sj);
}
