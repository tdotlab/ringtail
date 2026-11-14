// Host test for the core of join (src/programs/lib/joincore.c): field numbers,
// the items of -o, the cutting of lines into fields and the comparing of keys,
// against what GNU join made of them (tests/gen_jncore_cases.py makes
// jncore_cases.h). Run under the address and undefined-behaviour sanitizers.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "joincore.h"
#include "jncore_cases.h"

static int failures;

#define CHECK(cond, ...) \
    do { \
        if (!(cond)) { \
            failures++; \
            if (failures <= 30) { \
                printf("FAIL: "); \
                printf(__VA_ARGS__); \
                printf("\n"); \
            } \
        } \
    } while (0)

static void show(char* out, const char* s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 32 && c < 127) *out++ = (char)c;
        else out += sprintf(out, "\\x%02x", c);
    }
    *out = 0;
}

// A copy of exactly n bytes, so that the sanitizer sees a read past the end.
static char* exact(const char* s, size_t n) {
    char* p = malloc(n ? n : 1);
    if (n) memcpy(p, s, n);
    return p;
}

int main(void) {
    char shown[1024], shown2[1024];
    uint64_t checked = 0;

    for (size_t i = 0; i < sizeof jn_field_cases / sizeof jn_field_cases[0]; i++) {
        const jn_field_case_t* c = &jn_field_cases[i];
        char* s = strdup(c->s);
        uint64_t v = 12345;
        int r = jn_parse_field(s, &v);
        show(shown, c->s, strlen(c->s));
        CHECK((r == 0) == c->ok && (!c->ok || v == c->val), "field \"%s\": GNU %s %llu, ours %d %llu", shown, c->ok ? "ok" : "bad", c->val, r, (unsigned long long)v);
        CHECK(c->ok || v == 12345, "field \"%s\" wrote a value on failure", shown);
        free(s);
        checked++;
    }
    for (size_t i = 0; i < sizeof jn_filenum_cases / sizeof jn_filenum_cases[0]; i++) {
        const jn_filenum_case_t* c = &jn_filenum_cases[i];
        char* s = strdup(c->s);
        int r = jn_parse_filenum(s);
        show(shown, c->s, strlen(c->s));
        CHECK(r == c->which, "-a \"%s\": GNU %d, ours %d", shown, c->which, r);
        free(s);
        checked++;
    }
    for (size_t i = 0; i < sizeof jn_spec_cases / sizeof jn_spec_cases[0]; i++) {
        const jn_spec_case_t* c = &jn_spec_cases[i];
        char* s = strdup(c->s);
        int file = 99;
        uint64_t field = 12345;
        int r = jn_decode_spec(s, &file, &field);
        show(shown, c->s, strlen(c->s));
        CHECK(r == c->status, "item \"%s\": GNU status %d, ours %d", shown, c->status, r);
        if (r == JN_OK && c->status == JN_OK) {
            CHECK(file == c->file, "item \"%s\": GNU file %d, ours %d", shown, c->file, file);
            if (c->field >= 0) CHECK(field == (uint64_t)c->field, "item \"%s\": GNU field %d, ours %llu", shown, c->field, (unsigned long long)field);
            else CHECK(field >= 60, "item \"%s\": GNU past the end, ours %llu", shown, (unsigned long long)field);
        }
        free(s);
        checked++;
    }
    for (size_t i = 0; i < sizeof jn_cut_cases / sizeof jn_cut_cases[0]; i++) {
        const jn_cut_case_t* c = &jn_cut_cases[i];
        char* line = exact(c->line.s, c->line.n);
        jn_field_t* f = malloc((c->nf ? c->nf : 1) * sizeof *f);
        uint32_t nf = jn_split(line, c->line.n, c->tab == '\n' ? '\n' : c->tab, f, c->nf);
        show(shown, line, c->line.n);
        CHECK(nf == c->nf, "tab %d, line \"%s\": GNU %u fields, ours %u", c->tab, shown, c->nf, nf);
        for (uint32_t k = 0; k < c->nf && k < nf; k++) {
            int same = f[k].len == c->f[k].n && !memcmp(line + f[k].beg, c->f[k].s, f[k].len);
            show(shown2, line + f[k].beg, f[k].len);
            CHECK(same, "tab %d, line \"%s\", field %u: ours \"%s\"", c->tab, shown, k, shown2);
        }
        // With no room at all, or with room for only one, the count is the same.
        CHECK(jn_split(line, c->line.n, c->tab, 0, 0) == c->nf, "tab %d, line \"%s\": count with no room", c->tab, shown);
        CHECK(jn_split(line, c->line.n, c->tab, f, c->nf ? 1 : 0) == c->nf, "tab %d, line \"%s\": count with room for one", c->tab, shown);
        free(f);
        free(line);
        checked++;
    }
    for (size_t i = 0; i < sizeof jn_key_cases / sizeof jn_key_cases[0]; i++) {
        const jn_key_case_t* c = &jn_key_cases[i];
        char* a = exact(c->a.s, c->a.n);
        char* b = exact(c->b.s, c->b.n);
        int r = jn_keycmp(a, c->a.n, b, c->b.n, c->icase);
        int sign = r < 0 ? -1 : r > 0;
        show(shown, a, c->a.n);
        show(shown2, b, c->b.n);
        CHECK(sign == c->order, "keys \"%s\" \"%s\"%s: GNU %d, ours %d", shown, shown2, c->icase ? " -i" : "", c->order, sign);
        int back = jn_keycmp(b, c->b.n, a, c->a.n, c->icase);
        CHECK((back < 0 ? -1 : back > 0) == -c->order, "keys \"%s\" \"%s\"%s reversed: %d", shown, shown2, c->icase ? " -i" : "", back);
        free(a);
        free(b);
        checked++;
    }

    char t[24];
    jn_u64_text(0, t);
    CHECK(!strcmp(t, "0"), "u64 text of 0: %s", t);
    jn_u64_text(18446744073709551615ull, t);
    CHECK(!strcmp(t, "18446744073709551615"), "u64 text of the biggest: %s", t);
    jn_u64_text(9223372036854775806ull, t);
    CHECK(!strcmp(t, "9223372036854775806"), "u64 text: %s", t);

    if (failures) {
        printf("test_jncore: %d failures\n", failures);
        return 1;
    }
    printf("test_jncore: all %llu cases agree with GNU join\n", (unsigned long long)checked);
    return 0;
}
