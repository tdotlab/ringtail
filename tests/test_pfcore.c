// Host test for the core of printf (src/programs/lib/pfcore.c): some 4600
// formats with their arguments (many of them a little wrong), against what
// GNU printf wrote: its standard output and standard error in the order
// they were written, and its exit status (tests/gen_pfcore_cases.py makes
// pfcore_cases.h). Run under the address and undefined-behaviour
// sanitizers.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pfcore.h"
#include "pfcore_cases.h"

typedef struct {
    char* s;
    size_t n, cap;
} buf_t;

static void add(void* ctx, const char* s, uint32_t n) {
    buf_t* b = ctx;
    if (b->n + n > b->cap) {
        b->cap = (b->n + n) * 2 + 64;
        b->s = realloc(b->s, b->cap);
    }
    memcpy(b->s + b->n, s, n);
    b->n += n;
}

static void show(const char* what, const char* s, size_t n) {
    printf("    %s (%zu): \"", what, n);
    for (size_t i = 0; i < n && i < 160; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 32 && c < 127 && c != '"' && c != '\\') putchar(c);
        else printf("\\x%02x", c);
    }
    printf("\"\n");
}

int main(void) {
    int failures = 0;
    size_t ncases = sizeof pf_cases / sizeof pf_cases[0];
    for (size_t t = 0; t < ncases; t++) {
        const pf_case_t* c = &pf_cases[t];
        // Exact sizes, so the sanitizer sees any read past them.
        char* fmt = strdup(c->format);
        char* args[sizeof pf_cases[0].args / sizeof pf_cases[0].args[0]];
        for (int i = 0; i < c->nargs; i++) args[i] = strdup(c->args[i]);
        buf_t b = {0, 0, 0};
        pf_io_t io = {add, add, &b};
        int st = pf_run(&io, fmt, c->nargs, args);
        if (st != c->status || b.n != c->wantlen || (b.n && memcmp(b.s, c->want, b.n))) {
            failures++;
            if (failures <= 20) {
                printf("FAIL case %zu: status GNU %d, ours %d\n", t, c->status, st);
                show("format", c->format, strlen(c->format));
                for (int i = 0; i < c->nargs; i++) show("arg", c->args[i], strlen(c->args[i]));
                show("GNU", c->want, c->wantlen);
                show("ours", b.s, b.n);
            }
        }
        free(fmt);
        for (int i = 0; i < c->nargs; i++) free(args[i]);
        free(b.s);
    }
    printf("pfcore: %zu cases, %d failed\n", ncases, failures);
    return failures != 0;
}
