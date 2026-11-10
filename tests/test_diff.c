// Host test for diff's core (src/programs/lib/diffcore.c): which lines
// changed and the three ways of printing them. diff_cases.h holds some
// 2000 pairs of files with what GNU diff prints for them, in each format
// and under -i, -b, -w and --minimal (tests/gen_diff_cases.py makes it),
// so this checks that we pick the same edit script GNU does, byte for
// byte. Run under the address and undefined-behaviour sanitizers.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "diffcore.h"
#include "diff_cases.h"

static int failures, checks;

typedef struct {
    char* p;
    uint32_t n, cap;
} buf_t;

static void put(void* ctx, const char* s, uint32_t n) {
    buf_t* b = ctx;
    if (b->n + n > b->cap) {
        b->cap = (b->n + n) * 2 + 64;
        b->p = realloc(b->p, b->cap);
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
}

static void show(const char* what, const char* s, uint32_t n) {
    printf("  %s:\n", what);
    fwrite(s, 1, n, stdout);
    if (n && s[n - 1] != '\n') printf("<no newline>\n");
}

static void run(const diff_case_t* c, int idx) {
    char style = c->opts[0];
    uint32_t context = (uint32_t)atoi(c->opts + 1);
    unsigned flags = 0;
    const char* sl = strchr(c->opts, '/');
    for (const char* f = sl ? sl + 1 : ""; *f; f++) {
        if (*f == 'i') flags |= DF_ICASE;
        if (*f == 'b') flags |= DF_SPACE;
        if (*f == 'w') flags |= DF_ALLSPACE;
        if (*f == 'd') flags |= DF_MINIMAL;
    }
    // Copies, so the sanitizer catches a read past either end.
    char* a = malloc(c->alen + 1);
    char* b = malloc(c->blen + 1);
    memcpy(a, c->a, c->alen);
    memcpy(b, c->b, c->blen);
    df_file_t f[2];
    if (df_split(&f[0], a, c->alen) || df_split(&f[1], b, c->blen)) {
        printf("FAIL case %d: out of memory\n", idx);
        exit(1);
    }
    df_change_t* script = 0;
    if (df_compare(f, flags, style == 'n' ? 0 : context, &script)) {
        printf("FAIL case %d: out of memory\n", idx);
        exit(1);
    }
    buf_t out = {0};
    if (style == 'n') df_print_normal(put, &out, f, script);
    else if (style == 'u') df_print_unified(put, &out, f, script, context);
    else df_print_context(put, &out, f, script, context);
    int status = script ? 1 : 0;
    checks++;
    if (status != c->status || out.n != c->outlen || (out.n && memcmp(out.p, c->out, out.n))) {
        failures++;
        if (failures <= 5) {
            printf("FAIL case %d (%s): status %d, GNU %d\n", idx, c->opts, status, c->status);
            show("a", c->a, c->alen);
            show("b", c->b, c->blen);
            show("ours", out.p, out.n);
            show("GNU", c->out, c->outlen);
        }
    }
    free(out.p);
    df_free_script(script);
    df_free_file(&f[0]);
    df_free_file(&f[1]);
    free(a);
    free(b);
}

int main(int argc, char** argv) {
    int n = (int)(sizeof diff_cases / sizeof *diff_cases);
    if (argc > 1) {
        int i = atoi(argv[1]);
        if (i >= 0 && i < n) run(&diff_cases[i], i);
    } else {
        for (int i = 0; i < n; i++) run(&diff_cases[i], i);
    }
    printf("test_diff: %d of %d cases match GNU diff\n", checks - failures, checks);
    return failures ? 1 : 0;
}
