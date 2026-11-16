// Host test for the shell's core (src/programs/lib/shcore.c): some 500
// scripts, against what bash --posix (or dash, where they differ on what
// POSIX leaves open) wrote on standard output and its exit status
// (tests/gen_sh_cases.py makes sh_cases.h). Each runs in an empty
// directory of its own with sh_host, the core over POSIX: as `sh -c`, fed
// on standard input, and fed a line at a time through sh_feed (-i). Both
// are built with the address and undefined-behaviour sanitizers, which
// fail a case by its status.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "sh_cases.h"

typedef struct {
    char* s;
    size_t n, cap;
} buf_t;

static void add(buf_t* b, const char* s, size_t n) {
    if (b->n + n + 1 > b->cap) {
        b->cap = (b->n + n) * 2 + 64;
        b->s = realloc(b->s, b->cap);
    }
    memcpy(b->s + b->n, s, n);
    b->n += n;
    b->s[b->n] = 0;
}

static void show(const char* what, const char* s, size_t n) {
    printf("    %s (%zu): \"", what, n);
    for (size_t i = 0; i < n && i < 300; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 32 && c < 127 && c != '"' && c != '\\') putchar(c);
        else printf("\\x%02x", c);
    }
    printf("%s\"\n", n > 300 ? "..." : "");
}

static char sh[PATH_MAX];

// Runs the shell in dir: its exit status (-1: something else ended it).
static int run(const char* dir, char** argv, const char* in, size_t inn, buf_t* out, buf_t* err) {
    int pin[2], pout[2], perr[2];
    if (pipe(pin) || pipe(pout) || pipe(perr)) return -2;
    pid_t pid = fork();
    if (pid == 0) {
        dup2(pin[0], 0);
        dup2(pout[1], 1);
        dup2(perr[1], 2);
        for (int fd = 3; fd < 1024; fd++) close(fd);
        if (chdir(dir) < 0) _exit(126);
        alarm(30);
        execv(sh, argv);
        _exit(127);
    }
    close(pin[0]);
    close(pout[1]);
    close(perr[1]);
    fcntl(pin[1], F_SETFL, O_NONBLOCK);
    size_t sent = 0;
    if (!inn) {
        close(pin[1]);
        pin[1] = -1;
    }
    int open_out = 1, open_err = 1;
    char tmp[4096];
    while (open_out || open_err) {
        struct pollfd p[3] = { { pout[0], POLLIN, 0 }, { perr[0], POLLIN, 0 }, { pin[1], POLLOUT, 0 } };
        if (poll(p, pin[1] >= 0 ? 3 : 2, -1) < 0 && errno != EINTR) break;
        for (int k = 0; k < 2; k++) {
            if (!(p[k].revents & (POLLIN | POLLHUP))) continue;
            ssize_t r = read(p[k].fd, tmp, sizeof tmp);
            if (r > 0) {
                add(k ? err : out, tmp, (size_t)r);
            } else {
                if (k) open_err = 0;
                else open_out = 0;
                p[k].fd = -1;
            }
        }
        if (!open_out) pout[0] = -1;
        if (!open_err) perr[0] = -1;
        if (pin[1] >= 0 && p[2].revents) {
            ssize_t w = (p[2].revents & POLLOUT) ? write(pin[1], in + sent, inn - sent) : -1;
            if (w > 0) sent += (size_t)w;
            if (w < 0 || sent == inn) {
                close(pin[1]);
                pin[1] = -1;
            }
        }
    }
    if (pin[1] >= 0) close(pin[1]);
    close(pout[0] < 0 ? -1 : pout[0]);
    close(perr[0] < 0 ? -1 : perr[0]);
    int st;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static int rm_tree(const char* dir) {
    pid_t pid = fork();
    if (pid == 0) {
        execl("/bin/rm", "rm", "-rf", dir, (char*)0);
        _exit(1);
    }
    int st;
    waitpid(pid, &st, 0);
    return st;
}

int main(int argc, char** argv) {
    if (!realpath(argc > 1 ? argv[1] : "./sh_host", sh)) {
        perror("sh_host");
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    int failures = 0, runs = 0;
    int n = (int)(sizeof sh_cases / sizeof *sh_cases);
    for (int i = 0; i < n; i++) {
        const sh_case_t* c = &sh_cases[i];
        static const struct { int mode; const char* name; } modes[] = { { MODE_C, "-c" }, { MODE_S, "stdin" }, { MODE_I, "-i" } };
        for (int m = 0; m < 3; m++) {
            if (!(c->modes & modes[m].mode)) continue;
            char dir[] = "/tmp/test_sh.XXXXXX";
            if (!mkdtemp(dir)) {
                perror("mkdtemp");
                return 1;
            }
            char a0[] = "sh", ac[] = "-c", as[] = "-s", ai[] = "-i";
            char* av[4] = { a0, 0, 0, 0 };
            const char* in;
            buf_t text = { 0, 0, 0 };
            if (modes[m].mode == MODE_C) {
                av[1] = ac;
                av[2] = (char*)c->script;
                in = c->has_in ? c->in : "";
                add(&text, in, strlen(in));
            } else {
                av[1] = modes[m].mode == MODE_S ? as : ai;
                add(&text, c->script, strlen(c->script));
                if (!text.n || text.s[text.n - 1] != '\n') add(&text, "\n", 1);
            }
            buf_t out = { 0, 0, 0 }, err = { 0, 0, 0 };
            int st = run(dir, av, text.s ? text.s : "", text.n, &out, &err);
            rm_tree(dir);
            runs++;
            int bad = st != c->status || out.n != (size_t)c->outn || (out.n && memcmp(out.s, c->out, out.n)) ||
                      (modes[m].mode == MODE_C && !!err.n != c->err) || strstr(err.s ? err.s : "", "Sanitizer") ||
                      strstr(err.s ? err.s : "", "runtime error");
            if (bad) {
                failures++;
                printf("FAIL case %d (%s)\n", i, modes[m].name);
                show("script", c->script, strlen(c->script));
                printf("    status: want %d, got %d\n", c->status, st);
                show("want", c->out, (size_t)c->outn);
                show("got", out.s ? out.s : "", out.n);
                show("stderr", err.s ? err.s : "", err.n);
                printf("    (stderr expected: %s)\n", c->err ? "yes" : "no");
            }
            free(out.s);
            free(err.s);
            free(text.s);
        }
    }
    printf("test_sh: %d of %d runs (%d scripts) passed\n", runs - failures, runs, n);
    return failures ? 1 : 0;
}
