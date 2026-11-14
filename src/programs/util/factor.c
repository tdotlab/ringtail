// factor [-h | --exponents] [NUMBER...]
//
// Writes each number and its prime factors, smallest first, repeated as
// often as they divide it:  12: 2 2 3.  0 and 1 have none ("1:"). With no
// NUMBER the numbers are read from standard input, separated by spaces,
// tabs or newlines. A number is digits with an optional + in front, up to
// 2^64-1 (GNU factor goes on to bigger ones; this one stops there and
// says so). Anything else is reported as "'TOKEN' is not a valid positive
// integer", and the work goes on with the next number, ending with status 1.
//   -h, --exponents   write a repeated factor as p^e (360: 2^3 3^2 5)
// Options may come after the numbers, and a long option may be shortened as
// long as it stays unique. -- ends the options. As in GNU factor, the answers
// are held back until the program ends (or has to wait for input), so a
// message about a bad number comes out before the answers that preceded it.
// A usage error is status 2. --version is not supported.
#include "ulib.h"
#include "lineio.h"
#include "factorcore.h"
#include "quotearg.h"

static int usage(void) {
    dprintf(STDERR_FILENO, "Try 'factor --help' for more information.\n");
    return 2;
}

static lio_out_t out;
static int exponents;
static int status;

static void complain(const char* tok, const char* what) {
    char* q = malloc(QA_SPACE(strlen(tok)));
    if (!q) {
        dprintf(STDERR_FILENO, "factor: memory exhausted\n");
        sys_exit(1);
    }
    qa_quote(tok, QA_C, q);
    dprintf(STDERR_FILENO, "factor: %s %s\n", q, what);
    free(q);
    status = 1;
}

static void do_number(const char* tok) {
    uint64_t v;
    int r = fc_parse(tok, &v);
    if (r == FC_INVALID) {
        complain(tok, "is not a valid positive integer");
        return;
    }
    if (r == FC_TOO_BIG) {
        complain(tok, "is too large (the most is 18446744073709551615)");
        return;
    }
    uint64_t f[64];
    uint32_t n = fc_factor(v, f);
    char t[24];
    fc_u64_text(v, t);
    lio_puts(&out, t);
    lio_putc(&out, ':');
    for (uint32_t i = 0; i < n;) {
        uint32_t run = 1;
        while (i + run < n && f[i + run] == f[i]) run++;
        fc_u64_text(f[i], t);
        if (exponents) {
            lio_putc(&out, ' ');
            lio_puts(&out, t);
            if (run > 1) {
                char e[24];
                lio_putc(&out, '^');
                fc_u64_text(run, e);
                lio_puts(&out, e);
            }
        } else {
            for (uint32_t k = 0; k < run; k++) {
                lio_putc(&out, ' ');
                lio_puts(&out, t);
            }
        }
        i += run;
    }
    lio_putc(&out, '\n');
}

// Which long option `s` (the text after the dashes, up to any '=') names:
// 0 exponents, 1 help, 2 version, -1 none, -2 more than one.
static int long_option(const char* s, uint32_t n) {
    static const char* names[] = { "exponents", "help", "version" };
    int hit = -1;
    for (int i = 0; i < 3; i++) {
        if (strncmp(s, names[i], n)) continue;
        hit = hit == -1 ? i : -2;
    }
    return hit;
}

int main(int argc, char** argv) {
    int nops = 0, only_ops = 0;
    for (int a = 1; a < argc; a++) {
        const char* s = argv[a];
        if (only_ops || s[0] != '-' || !s[1]) {
            argv[1 + nops++] = argv[a];
            continue;
        }
        if (!strcmp(s, "--")) {
            only_ops = 1;
            continue;
        }
        if (s[1] == '-') {
            const char* eq = strchr(s + 2, '=');
            uint32_t n = eq ? (uint32_t)(eq - (s + 2)) : (uint32_t)strlen(s + 2);
            int o = long_option(s + 2, n);
            if (o == -1) {
                dprintf(STDERR_FILENO, "factor: unrecognized option '%s'\n", s);
                return usage();
            }
            if (o == -2) {
                dprintf(STDERR_FILENO, "factor: option '%s' is ambiguous; possibilities: '--exponents' '--help' '--version'\n", s);
                return usage();
            }
            if (o == 2) {
                dprintf(STDERR_FILENO, "factor: option '--version' is not supported\n");
                return usage();
            }
            if (eq) {
                dprintf(STDERR_FILENO, "factor: option '--%s' doesn't allow an argument\n", o ? "help" : "exponents");
                return usage();
            }
            if (o == 1) {
                printf("usage: factor [-h | --exponents] [NUMBER...]\n"
                       "Prints each NUMBER (up to 18446744073709551615) and its prime factors.\n"
                       "With no NUMBER, reads them from standard input.\n"
                       "  -h, --exponents   write a repeated factor as p^e\n");
                return 0;
            }
            exponents = 1;
            continue;
        }
        for (const char* o = s + 1; *o; o++) {
            if (*o != 'h') {
                dprintf(STDERR_FILENO, "factor: invalid option -- '%c'\n", *o);
                return usage();
            }
            exponents = 1;
        }
    }

    lio_out_init(&out, STDOUT_FILENO, "factor");
    if (nops) {
        for (int i = 0; i < nops; i++) do_number(argv[1 + i]);
        lio_flush(&out);
        return status;
    }

    lio_in_t in;
    lio_in_init(&in, STDIN_FILENO);
    lio_line_t tok = { 0, 0, 0, 0 };
    for (;;) {
        int c = lio_getc(&in, &out);
        if (c == ' ' || c == '\t' || c == '\n' || c < 0) {
            if (tok.n) {
                lio_line_addc(&tok, 0, "factor");
                do_number(tok.s);
                tok.n = 0;
            }
            if (c < 0) break;
        } else {
            lio_line_addc(&tok, (char)c, "factor");
        }
    }
    lio_flush(&out);
    if (in.err) {
        dprintf(STDERR_FILENO, "factor: standard input: %s\n", strerror(in.err));
        status = 1;
    }
    return status;
}
