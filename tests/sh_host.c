// The shell's core as a plain host program (shos_host.c underneath): what
// test_sh runs its cases with. -i reads standard input through sh_feed, a
// line at a time, the way the prompt does.
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include "sh.h"

static const sh_front_t front = { 0, 0, 0, 0, 0 };

int main(int argc, char** argv) {
    signal(SIGPIPE, SIG_IGN);
    int r = sh_main(&front, argc, argv);
    if (r != SH_INTERACTIVE) return r;
    static char text[1 << 16];
    size_t n = 0;
    int st;
    for (;;) {
        if (n + 2 >= sizeof text) n = 0;
        if (!fgets(text + n, (int)(sizeof text - n), stdin)) break;
        n += strlen(text + n);
        if (sh_feed(text) == SH_DONE) n = 0;
        if (sh_exit_asked(&st)) return st;
        text[n] = 0;
    }
    sh_feed(0);  // what is left unfinished at the end is dropped
    sh_feed("exit\n");
    return sh_exit_asked(&st) ? st : 0;
}
