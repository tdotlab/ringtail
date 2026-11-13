// Host test for the checksum-list parser of sha256 -c
// (src/programs/lib/shasum.c): some 6000 lines, most of them a little wrong,
// a third of them after another line, against what GNU sha256sum -c made of
// each (tests/gen_shasum_cases.py makes shasum_cases.h): skipped, improperly
// formatted, or a line with this name.
// Run under the address and undefined-behaviour sanitizers.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "shasum.h"
#include "shasum_cases.h"

static int failures, checks;

static void show(const char* what, const char* s, uint32_t n) {
    printf("    %s (%u):", what, n);
    for (uint32_t i = 0; i < n && i < 80; i++) printf(" %02x", (uint8_t)s[i]);
    printf("\n");
}

static int hex(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c - 'A' + 10;
}

int main(void) {
    uint32_t ncases = sizeof shasum_cases / sizeof shasum_cases[0];
    for (uint32_t t = 0; t < ncases; t++) {
        const shasum_case_t* c = &shasum_cases[t];
        // Exact sizes, so the sanitizer sees any read or write past them.
        char* line = malloc(c->linelen ? c->linelen : 1);
        memcpy(line, c->line, c->linelen);
        char* name = malloc(c->linelen + 1);
        uint8_t hash[32];
        uint32_t namelen = 0;
        int style = SHASUM_STYLE_NONE;
        if (c->prefix >= 0) {
            const shasum_prefix_t* p = &shasum_prefixes[c->prefix];
            char* pl = malloc(p->linelen ? p->linelen : 1);
            memcpy(pl, p->line, p->linelen);
            char* pname = malloc(p->linelen + 1);
            uint32_t pnamelen;
            shasum_parse(pl, p->linelen, &style, hash, pname, &pnamelen);
            free(pl);
            free(pname);
        }
        int r = shasum_parse(line, c->linelen, &style, hash, name, &namelen);
        int want = c->kind == 0 ? SHASUM_SKIP : c->kind == 1 ? SHASUM_IMPROPER : SHASUM_LINE;
        int bad = r != want;
        if (!bad && r == SHASUM_LINE) {
            bad = namelen != c->namelen || memcmp(name, c->name, namelen) || name[namelen];
            for (int i = 0; i < 32; i++)
                if (hash[i] != hex(c->line[c->hashoff + 2 * i]) * 16 + hex(c->line[c->hashoff + 2 * i + 1])) bad = 1;
        }
        checks++;
        if (bad) {
            failures++;
            printf("FAIL case %u: GNU kind %d, ours %d\n", t, c->kind, r);
            show("line", c->line, c->linelen);
            show("GNU name", c->name, c->namelen);
            show("our name", name, namelen);
        }
        free(line);
        free(name);
    }
    // Which names have to be written in backslash form.
    struct { const char* s; int esc; } esc[] = { {"a", 0}, {"", 0}, {"a b", 0}, {"a\\b", 1}, {"a\nb", 1}, {"a\rb", 1}, {"a\tb", 0}, {"\\", 1}, {"*", 0} };
    for (unsigned i = 0; i < sizeof esc / sizeof esc[0]; i++) {
        checks++;
        if (shasum_needs_escape(esc[i].s, (uint32_t)strlen(esc[i].s)) != esc[i].esc) {
            failures++;
            printf("FAIL shasum_needs_escape \"%s\"\n", esc[i].s);
        }
    }
    printf("test_shasum: %d passed, %d failed\n", checks - failures, failures);
    return failures != 0;
}
