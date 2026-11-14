// Host test for the core of factor (src/programs/lib/factorcore.c): the
// factors of thousands of numbers, and how an argument is read, against what
// GNU factor made of them (tests/gen_fcore_cases.py makes fcore_cases.h).
// Then numbers with no GNU answer to compare with, checked by multiplying
// the factors back and testing each for primality. Run under the address and
// undefined-behaviour sanitizers.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "factorcore.h"
#include "fcore_cases.h"

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

// The line GNU factor writes: "n: f f f".
static void line_of(uint64_t n, char* out) {
    uint64_t f[64];
    uint32_t cnt = fc_factor(n, f);
    fc_u64_text(n, out);
    out += strlen(out);
    *out++ = ':';
    for (uint32_t i = 0; i < cnt; i++) {
        *out++ = ' ';
        fc_u64_text(f[i], out);
        out += strlen(out);
    }
    *out = 0;
}

static uint64_t rng = 88172645463325252ull;
static uint64_t next(void) {  // xorshift64
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

int main(void) {
    char got[2048];
    uint64_t checked = 0;

    for (size_t i = 0; i < sizeof fc_cases / sizeof fc_cases[0]; i++) {
        const fc_case_t* c = &fc_cases[i];
        line_of(c->n, got);
        CHECK(!strcmp(got, c->line), "factor %llu: GNU \"%s\", ours \"%s\"", c->n, c->line, got);
        uint64_t f[64];
        uint32_t cnt = fc_factor(c->n, f);
        int prime = cnt == 1;
        CHECK(fc_is_prime(c->n) == prime, "is_prime %llu: GNU says %s", c->n, prime ? "prime" : "not prime");
        checked += 2;
    }

    for (size_t i = 0; i < sizeof fc_parse_cases / sizeof fc_parse_cases[0]; i++) {
        const fc_parse_case_t* c = &fc_parse_cases[i];
        uint64_t v = 12345;
        char* s = strdup(c->s);  // exact size, for the sanitizer
        int r = fc_parse(s, &v);
        CHECK(r == c->result, "parse \"%s\": GNU %d, ours %d", c->s, c->result, r);
        CHECK(r != FC_OK || v == c->val, "parse \"%s\": GNU %llu, ours %llu", c->s, c->val, (unsigned long long)v);
        CHECK(r == FC_OK || v == 12345, "parse \"%s\" wrote a value on failure", c->s);
        free(s);
        checked++;
    }

    // text of a number, and back
    for (int i = 0; i < 20000; i++) {
        uint64_t n = next() >> (next() % 64);
        char t[24];
        fc_u64_text(n, t);
        uint64_t back = 0;
        CHECK(fc_parse(t, &back) == FC_OK && back == n, "text/parse round trip of %llu (\"%s\")", (unsigned long long)n, t);
        checked++;
    }

    // numbers with no GNU answer: the factors must multiply to the number,
    // be prime, and come in order. Many are built from two or three primes
    // so that the work is the hard kind.
    for (int i = 0; i < 4000; i++) {
        uint64_t n;
        switch (i % 4) {
        case 0: n = next() >> (next() % 64); break;
        case 1: n = (next() >> 33 | 1ull << 31) * (next() >> 33 | 1ull << 31); break;  // two 32-bit numbers
        case 2: n = (next() >> 43 | 1ull << 20) * (next() >> 43 | 1ull << 20) * (next() >> 43 | 1ull << 20); break;
        default: {
            uint64_t p = next() >> 33 | 1ull << 31;
            n = p * p;  // the square of a 32-bit number
            break;
        }
        }
        uint64_t f[64];
        uint32_t cnt = fc_factor(n, f);
        uint64_t prod = 1;
        int ok = n >= 2 || cnt == 0;
        for (uint32_t k = 0; k < cnt; k++) {
            prod *= f[k];
            ok = ok && fc_is_prime(f[k]) && (k == 0 || f[k - 1] <= f[k]);
        }
        CHECK(ok && (n < 2 || prod == n), "factors of %llu are wrong (%u of them)", (unsigned long long)n, cnt);
        CHECK(fc_is_prime(n) == (cnt == 1), "is_prime %llu disagrees with its factors", (unsigned long long)n);
        checked += 2;
    }

    if (failures) {
        printf("%d FAILED of %llu checks\n", failures, (unsigned long long)checked);
        return 1;
    }
    printf("fcore: %llu checks passed\n", (unsigned long long)checked);
    return 0;
}
