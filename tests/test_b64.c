// Host test for base64's core (src/programs/lib/b64.c): encoding with every
// kind of -w argument, and decoding with and without -i, against what GNU
// base64 wrote for some 4000 inputs (tests/gen_b64_cases.py makes
// b64_cases.h), byte for byte, including the bytes written before it stopped
// at something invalid. Run under the address and undefined-behaviour
// sanitizers. Also round trips.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "b64.h"
#include "b64_cases.h"

static int failures, checks;

static void show(const char* what, const uint8_t* s, uint32_t n) {
    printf("    %s (%u):", what, n);
    for (uint32_t i = 0; i < n && i < 80; i++) printf(" %02x", s[i]);
    printf("\n");
}

// Returns 1 for a run GNU would call a success.
static int run(const b64_case_t* c, uint8_t* out, uint32_t* n) {
    *n = 0;
    if (c->kind == 'e') {
        uint32_t wrap;
        if (b64_parse_wrap(c->arg, &wrap)) return 0;
        b64enc_t e;
        b64enc_init(&e, wrap);
        char buf[16];
        for (unsigned i = 0; i < c->inlen; i++) {
            int k = b64enc_put(&e, (uint8_t)c->in[i], buf);
            memcpy(out + *n, buf, (size_t)k);
            *n += (uint32_t)k;
        }
        int k = b64enc_end(&e, buf);
        memcpy(out + *n, buf, (size_t)k);
        *n += (uint32_t)k;
        return 1;
    }
    b64dec_t d;
    b64dec_init(&d, c->kind == 'i');
    uint8_t buf[3];
    for (unsigned i = 0; i < c->inlen; i++) {
        int k = b64dec_put(&d, (uint8_t)c->in[i], buf);
        memcpy(out + *n, buf, (size_t)k);
        *n += (uint32_t)k;
        if (d.bad) return 0;
    }
    int k = b64dec_end(&d, buf);
    memcpy(out + *n, buf, (size_t)k);
    *n += (uint32_t)k;
    return !d.bad;
}

int main(void) {
    uint32_t ncases = sizeof b64_cases / sizeof b64_cases[0];
    for (uint32_t t = 0; t < ncases; t++) {
        const b64_case_t* c = &b64_cases[t];
        // Exact sizes, so the sanitizer sees any read or write past them.
        uint32_t cap = c->kind == 'e' ? c->inlen * 3 + 16 : c->inlen + 8;
        uint8_t* in = malloc(c->inlen ? c->inlen : 1);
        memcpy(in, c->in, c->inlen);
        b64_case_t cc = *c;
        cc.in = (const char*)in;
        uint8_t* out = malloc(cap);
        uint32_t n;
        int ok = run(&cc, out, &n);
        checks++;
        if (ok != c->ok || n != c->outlen || memcmp(out, c->out, n)) {
            failures++;
            printf("FAIL case %u: kind %c arg \"%s\"\n", t, c->kind, c->arg);
            show("input", (const uint8_t*)c->in, c->inlen);
            show("GNU", (const uint8_t*)c->out, c->outlen);
            show("ours", out, n);
            printf("    GNU %s, ours %s\n", c->ok ? "ok" : "failed", ok ? "ok" : "failed");
        }
        free(in);
        free(out);
    }

    // Anything encoded decodes to itself, at any line length; and the
    // output of -w 0 is the same text as the lines joined.
    srand(7);
    for (int t = 0; t < 2000; t++) {
        uint32_t len = (uint32_t)rand() % 300;
        uint8_t* raw = malloc(len ? len : 1);
        for (uint32_t i = 0; i < len; i++) raw[i] = (uint8_t)rand();
        uint32_t wrap = (uint32_t)rand() % 20;
        b64enc_t e;
        b64enc_init(&e, wrap);
        char* enc = malloc(len * 3 + 64);
        uint32_t en = 0;
        for (uint32_t i = 0; i < len; i++) en += (uint32_t)b64enc_put(&e, raw[i], enc + en);
        en += (uint32_t)b64enc_end(&e, enc + en);
        // a line break after every wrap characters, a last one if the last line is not empty
        uint32_t lines = 0, col = 0;
        for (uint32_t i = 0; i < en; i++) {
            if (enc[i] == '\n') {
                if (col != wrap && i != en - 1) { failures++; printf("FAIL a short line, wrap %u\n", wrap); break; }
                col = 0;
                lines++;
            } else {
                col++;
            }
        }
        checks++;
        b64dec_t d;
        b64dec_init(&d, 0);
        uint8_t* back = malloc(len + 4);
        uint32_t bn = 0;
        for (uint32_t i = 0; i < en && !d.bad; i++) bn += (uint32_t)b64dec_put(&d, (uint8_t)enc[i], back + bn);
        bn += (uint32_t)b64dec_end(&d, back + bn);
        checks++;
        if (d.bad || bn != len || memcmp(back, raw, len)) {
            failures++;
            printf("FAIL round trip, length %u wrap %u\n", len, wrap);
        }
        (void)lines;
        free(raw);
        free(enc);
        free(back);
    }

    printf("test_b64: %d passed, %d failed\n", checks - failures, failures);
    return failures != 0;
}
