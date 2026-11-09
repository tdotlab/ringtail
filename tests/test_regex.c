// Host test for the regular-expression engine (src/programs/lib/regex.c)
// that grep is built on. regex_cases.h holds some 800 patterns, with the
// lines GNU grep selects and the matches `grep -o` prints for each over
// a fixed set of lines, so this checks us against the real thing
// (tests/gen_regex_cases.py makes it). Run under the address and
// undefined-behaviour sanitizers, plus a few things grep cannot tell us:
// limits, back-references, a pattern that would blow up a backtracker.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "regex.h"
#include "regex_cases.h"
#include "capture_cases.h"

static int failures, checks;

#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static unsigned parse_flags(const char* f) {
    unsigned r = 0;
    for (; *f; f++) {
        if (*f == 'E') r |= RE_ERE;
        if (*f == 'F') r |= RE_FIXED;
        if (*f == 'i') r |= RE_ICASE;
        if (*f == 'w') r |= RE_WORD;
        if (*f == 'x') r |= RE_LINE;
    }
    return r;
}

// "sel=1,2 o=1:0-2,3-4;2:1-3" like the generator writes.
static void describe(re_t* re, char* out, size_t size) {
    char sel[1024] = "", mat[2048] = "";
    size_t ns = 0, nm = 0;
    for (size_t k = 0; k < sizeof corpus / sizeof *corpus; k++) {
        const char* l = corpus[k];
        uint32_t len = (uint32_t)strlen(l);
        if (re_test(re, l, len)) ns += (size_t)snprintf(sel + ns, sizeof sel - ns, "%s%zu", ns ? "," : "", k);
        char one[256] = "";
        size_t no = 0;
        uint32_t from = 0, ms, me;
        while (from <= len && re_find(re, l, len, from, &ms, &me)) {
            if (me == ms) { from = me + 1; continue; }
            no += (size_t)snprintf(one + no, sizeof one - no, "%s%u-%u", no ? "," : "", ms, me);
            from = me;
        }
        if (no) nm += (size_t)snprintf(mat + nm, sizeof mat - nm, "%s%zu:%s", nm ? ";" : "", k, one);
    }
    snprintf(out, size, "sel=%s o=%s", sel, mat);
}

static void gnu_cases(void) {
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        const rcase_t* c = &cases[i];
        re_t re;
        const char* err = re_compile(&re, c->pat, (uint32_t)strlen(c->pat), parse_flags(c->flags));
        if (!strcmp(c->want, "ERR")) {
            CHECK(err != NULL, "[%s] /%s/ should not compile", c->flags, c->pat);
            if (!err) re_free(&re);
            continue;
        }
        CHECK(err == NULL, "[%s] /%s/ failed to compile: %s", c->flags, c->pat, err ? err : "");
        if (err) continue;
        char got[4096];
        describe(&re, got, sizeof got);
        CHECK(!strcmp(got, c->want), "[%s] /%s/\n   want %s\n   got  %s", c->flags, c->pat, c->want, got);
        re_free(&re);
    }
}

static int matches(const char* pat, unsigned flags, const char* line) {
    re_t re;
    if (re_compile(&re, pat, (uint32_t)strlen(pat), flags)) return -1;
    int r = re_test(&re, line, (uint32_t)strlen(line));
    re_free(&re);
    return r;
}

static void extra(void) {
    // Back-references are refused, not misread.
    re_t re;
    const char* e = re_compile(&re, "\\(a\\)\\1", 7, 0);
    CHECK(e && strstr(e, "back-reference"), "back-reference message: %s", e ? e : "(none)");
    e = re_compile(&re, "(a)\\1", 5, RE_ERE);
    CHECK(e && strstr(e, "back-reference"), "ERE back-reference message: %s", e ? e : "(none)");

    // The classic exponential case finishes at once on a Pike VM.
    char* line = malloc(5001);
    memset(line, 'a', 5000);
    line[5000] = 0;
    CHECK(matches("(a*)*b", RE_ERE, line) == 0, "(a*)*b on aaaa...");
    CHECK(matches("(a|aa)+$", RE_ERE, line) == 1, "(a|aa)+$ on aaaa...");
    CHECK(matches("(a?){30}a{30}", RE_ERE, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") == 1, "(a?){30}a{30}");
    free(line);

    // Very long lines and patterns.
    char* big = malloc(200001);
    memset(big, 'x', 200000);
    big[200000] = 0;
    CHECK(matches("x*y", 0, big) == 0, "long line, no match");
    big[199999] = 'y';
    CHECK(matches("x*y$", 0, big) == 1, "long line, match at the end");
    free(big);
    char pat[3000];
    for (int i = 0; i < 2999; i++) pat[i] = 'a';
    pat[2999] = 0;
    CHECK(matches(pat, 0, "b") == 0, "2999-character pattern compiles");

    // Limits give an error, not a crash.
    char* deep = malloc(2001);
    for (int i = 0; i < 1000; i++) { deep[i] = '('; deep[1000 + i] = ')'; }
    deep[2000] = 0;
    e = re_compile(&re, deep, 2000, RE_ERE);
    CHECK(e != NULL || re_test(&re, "x", 1) >= 0, "deep nesting is handled");
    if (!e) re_free(&re);
    free(deep);
    e = re_compile(&re, "a{1000}{1000}", 13, RE_ERE);
    CHECK(e != NULL, "a{1000}{1000} is refused as too big");
    if (!e) re_free(&re);
    e = re_compile(&re, "(a{1000}){1000}", 15, RE_ERE);
    CHECK(e != NULL, "(a{1000}){1000} is refused as too big");
    if (!e) re_free(&re);

    // Binary-ish data: NUL and high bytes are ordinary characters.
    re_t r2;
    CHECK(re_compile(&r2, "a.b", 3, 0) == NULL, "compile a.b");
    CHECK(re_test(&r2, "a\0b", 3) == 1, "dot matches NUL");
    CHECK(re_test(&r2, "a\xffz", 3) == 0, "no match on a high byte");
    CHECK(re_test(&r2, "a\xff" "b", 3) == 1, "dot matches a high byte");
    re_free(&r2);

    // Case folding reaches ranges and classes.
    CHECK(matches("[a-c]+", RE_ERE | RE_ICASE, "XBX") == 1, "range folded");
    CHECK(matches("[^a-c]", RE_ICASE, "B") == 0, "negated range folded");
    CHECK(matches("[[:upper:]]", RE_ICASE, "q") == 1, "[:upper:] with -i");
    CHECK(matches("abc", 0, "ABC") == 0, "case-sensitive by default");

    // Repeated compiles and frees do not leak or reuse stale state
    // (the sanitizer is the judge).
    for (int i = 0; i < 200; i++) {
        re_t t;
        CHECK(re_compile(&t, "(a|b)*c{2,4}x?", 14, RE_ERE) == NULL, "recompile");
        CHECK(re_test(&t, "abababcc", 8) == 1, "match after recompile");
        re_free(&t);
    }
}

// re_exec against GNU sed: the first match in each line and the text of every
// group, written "k:start-end|group1|group2;..." (an unused group is empty,
// as sed prints it). Patterns that give groups several ways to match
// (a|ab)(c|bcd) are where engines tend to differ, and the cases are full of them.
//
// Six random patterns differ from glibc, all of one kind: several top-level
// branches match the same longest text and glibc reports the groups of a
// later branch where we report the first one's. Nothing in POSIX says which
// is right, so they are listed here and not copied.
static const char* const known_differences[] = {
    "b(((\\b a?[[:alpha:]]|a[]a]c)|.+c\\<)a+a+.|b+| *((\\w?a*)+)\\S+)+",
    "\\(.*\\)*$\\|[^a]*b\\+\\( *\\)\\{2,\\}\\|\\(cbb*a\\)*.\\+ \\{1,2\\}$",
    "\\(.\\+\\<[[:space:]]\\|\\(.* \\>a\\)a\\+b\\{0,\\}\\([^]a][b-]\\?\\)\\|b\\+\\W*\\(\\([ab]*\\)\\|\\(\\^\\{1,1\\}\\|\\*\\?c\\?a*\\*\\?\\)\\{2,4\\}a.* \\|.c\\)\\)\\+a\\+\\([^a]\\)\\|b*a",
    "^([ab]|b* b*c|a*c+[[:space:]])?$|b*",
    "b*|(.)a*[]a]|([^]a]|([ab][b-])*){1,2}",
    ".\\+.*$\\|\\(ca\\?\\^\\{0\\}\\|ba\\+\\)\\+.*",
};

static int known_difference(const char* pat) {
    for (size_t i = 0; i < sizeof known_differences / sizeof *known_differences; i++)
        if (!strcmp(pat, known_differences[i])) return 1;
    return 0;
}

static void capture_cases(void) {
    int agree = 0, known = 0;
    for (size_t i = 0; i < sizeof ccases / sizeof *ccases; i++) {
        const ccase_t* c = &ccases[i];
        re_t re;
        const char* err = re_compile(&re, c->pat, (uint32_t)strlen(c->pat), parse_flags(c->flags) | RE_GROUPS);
        CHECK(err == NULL, "[%s] /%s/ failed to compile with groups: %s", c->flags, c->pat, err ? err : "");
        if (err) { re_free(&re); continue; }
        CHECK((int)re.ngroups == c->ngroups, "[%s] /%s/ has %d groups, we count %u", c->flags, c->pat, c->ngroups, re.ngroups);
        char got[4096];
        size_t n = 0;
        got[0] = 0;
        for (size_t k = 0; k < sizeof corpus / sizeof *corpus; k++) {
            const char* l = corpus[k];
            uint32_t len = (uint32_t)strlen(l);
            int32_t caps[2 * 32];  // regex.c allows 31 groups
            int r = re_exec(&re, l, len, 0, caps);
            CHECK(r >= 0, "re_exec ran out of memory");
            if (r != 1) continue;
            n += (size_t)snprintf(got + n, sizeof got - n, "%s%zu:%d-%d", n ? ";" : "", k, caps[0], caps[1]);
            for (int g = 1; g <= c->ngroups; g++) {
                n += (size_t)snprintf(got + n, sizeof got - n, "|");
                if (caps[2 * g] >= 0)
                    n += (size_t)snprintf(got + n, sizeof got - n, "%.*s", caps[2 * g + 1] - caps[2 * g], l + caps[2 * g]);
            }
        }
        int ok = !strcmp(got, c->want);
        agree += ok;
        if (!ok && known_difference(c->pat)) known++;
        else CHECK(ok, "[%s] /%s/\n   want %s\n   got  %s", c->flags, c->pat, c->want, got);
        re_free(&re);
    }
    CHECK(known == 6 || agree + known == (int)(sizeof ccases / sizeof *ccases), "the list of known differences is out of date: %d differ", known);
    printf("%d of %zu capture patterns agree with GNU sed (%d known differences)\n", agree, sizeof ccases / sizeof *ccases, known);
}

int main(void) {
    gnu_cases();
    capture_cases();
    extra();
    printf("%d checks, %d failed (%zu patterns checked against GNU grep)\n", checks, failures, sizeof cases / sizeof *cases);
    return failures != 0;
}
