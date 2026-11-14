// Host test for the core of split (src/programs/lib/splitcore.c): the numbers
// of -l, -b and -a, and the names of the files, against what GNU split made of
// them (tests/gen_spcore_cases.py makes spcore_cases.h). Run under the address
// and undefined-behaviour sanitizers.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "splitcore.h"
#include "spcore_cases.h"

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

static void show(char* out, const char* s) {
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c >= 32 && c < 127) *out++ = (char)c;
        else out += sprintf(out, "\\x%02x", c);
    }
    *out = 0;
}

int main(void) {
    char shown[256];
    uint64_t checked = 0;

    for (size_t i = 0; i < sizeof sp_lines_cases / sizeof sp_lines_cases[0]; i++) {
        const sp_size_case_t* c = &sp_lines_cases[i];
        char* s = strdup(c->s);  // exact size, for the sanitizer
        uint64_t v = 12345;
        int r = sp_parse_lines(s, &v);
        show(shown, c->s);
        CHECK((r == 0) == c->ok && (!c->ok || v == c->val), "-l \"%s\": GNU %s %llu, ours %d %llu", shown, c->ok ? "ok" : "bad", (unsigned long long)c->val, r,
              (unsigned long long)v);
        CHECK(c->ok || v == 12345, "-l \"%s\" wrote a value on failure", shown);
        free(s);
        checked++;
    }
    for (size_t i = 0; i < sizeof sp_bytes_cases / sizeof sp_bytes_cases[0]; i++) {
        const sp_size_case_t* c = &sp_bytes_cases[i];
        char* s = strdup(c->s);
        uint64_t v = 12345;
        int r = sp_parse_bytes(s, &v);
        show(shown, c->s);
        CHECK((r == 0) == c->ok && (!c->ok || v == c->val), "-b \"%s\": GNU %s %llu, ours %d %llu", shown, c->ok ? "ok" : "bad", (unsigned long long)c->val, r,
              (unsigned long long)v);
        CHECK(c->ok || v == 12345, "-b \"%s\" wrote a value on failure", shown);
        free(s);
        checked++;
    }
    for (size_t i = 0; i < sizeof sp_alen_cases / sizeof sp_alen_cases[0]; i++) {
        const sp_alen_case_t* c = &sp_alen_cases[i];
        char* s = strdup(c->s);
        uint32_t v = 12345;
        int r = sp_parse_suffix_len(s, &v);
        show(shown, c->s);
        CHECK(r == c->rc && (r != 0 || v == c->val), "-a \"%s\": GNU %d %u, ours %d %u", shown, c->rc, c->val, r, v);
        free(s);
        checked++;
    }

    for (size_t i = 0; i < sizeof sp_suffix_cases / sizeof sp_suffix_cases[0]; i++) {
        const sp_suffix_case_t* c = &sp_suffix_cases[i];
        char* from = c->from ? strdup(c->from) : 0;
        sp_suffix_t* s = malloc(sizeof *s);
        int r = sp_suffix_init(s, c->kind, c->alen, from);
        int want = c->status == 1 ? SP_BAD_START : c->status == 2 ? SP_START_TOO_LARGE : SP_OK;
        CHECK(r == want, "suffix config %zu (kind %d, -a %u, from %s): GNU status %d, ours %d", i, c->kind, c->alen, c->from ? c->from : "-", want, r);
        if (r == SP_OK && c->status == 0) {
            unsigned made = 0, at = 0;
            char text[SP_MAXLEN + 1];
            int ended = 0;
            for (;;) {
                sp_suffix_text(s, text);
                made++;
                if (at < c->nsamples && c->samples[at].idx == made - 1) {
                    CHECK(!strcmp(text, c->samples[at].suffix), "suffix config %zu (kind %d, -a %u, from %s), name %u: GNU \"%s\", ours \"%s\"", i, c->kind,
                          c->alen, c->from ? c->from : "-", made - 1, c->samples[at].suffix, text);
                    at++;
                }
                if (made == c->total) break;
                if (sp_suffix_next(s)) {
                    ended = 1;
                    break;
                }
                if (made > c->total + 10) break;
            }
            CHECK(!ended, "suffix config %zu (kind %d, -a %u, from %s): ours ran out after %u names, GNU made %u", i, c->kind, c->alen, c->from ? c->from : "-",
                  made, c->total);
            if (!ended && c->exhausted >= 0) {
                int more = sp_suffix_next(s);
                CHECK((more != 0) == (c->exhausted != 0), "suffix config %zu (kind %d, -a %u, from %s): after %u names GNU %s, ours %s", i, c->kind, c->alen,
                      c->from ? c->from : "-", c->total, c->exhausted ? "ran out" : "went on", more ? "ran out" : "went on");
            }
            CHECK(at == c->nsamples, "suffix config %zu: only %u of %u samples seen", i, at, c->nsamples);
            checked += c->total;
        }
        free(s);
        free(from);
    }

    // A suffix longer than the room there is.
    sp_suffix_t* s = malloc(sizeof *s);
    CHECK(sp_suffix_init(s, SP_ALPHA, SP_MAXLEN + 1, 0) == SP_TOO_LONG, "too long a suffix");
    CHECK(sp_suffix_init(s, SP_ALPHA, SP_MAXLEN, 0) == SP_OK, "the longest suffix");
    CHECK(sp_suffix_init(s, SP_ALPHA, 4294967295u, 0) == SP_TOO_LONG, "a suffix of 4G letters");
    // A start for the letters is no start.
    CHECK(sp_suffix_init(s, SP_ALPHA, 2, "1") == SP_BAD_START, "a start for letters");
    CHECK(sp_suffix_init(s, SP_ALPHA, 2, "") == SP_BAD_START, "an empty start for letters");

    // The widening stops where the suffix would be longer than the room there
    // is: put one 254 letters long (125 markers and 129 free letters) at its
    // last name by hand, and see it grow to 256, and no further.
    for (int kind = SP_ALPHA; kind <= SP_HEX; kind++) {
        int last = kind == SP_ALPHA ? 25 : kind == SP_NUM ? 9 : 15;
        for (int len = 254; len <= 256; len += 2) {
            memset(s, 0, sizeof *s);
            sp_suffix_init(s, kind, 0, 0);
            s->len = len;
            s->markers = (len - 4) / 2;
            for (int i = 0; i < len; i++) s->dig[i] = (unsigned char)(i < s->markers ? last : i == s->markers ? last - 1 : last);
            int r = sp_suffix_next(s);
            CHECK((r == 0) == (len < 256), "the widening of a suffix of %d letters (kind %d): %d", len, kind, r);
            if (r == 0) CHECK(s->len == 256 && s->dig[0] == last && s->dig[s->len - 1] == 0, "the widened suffix (kind %d)", kind);
        }
    }
    free(s);

    if (failures) {
        printf("test_spcore: %d failures\n", failures);
        return 1;
    }
    printf("test_spcore: %llu checks passed\n", (unsigned long long)checked);
    return 0;
}
