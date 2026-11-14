// printf FORMAT [ARG...]    write the arguments the way FORMAT says
//
// Nothing is added: no newline unless the format has \n. In the format, text
// is written as it is, with the escapes of echo -e (\n \t \r \a \b \e \f \v \\
// \" \NNN in octal, \xHH in hex, \c to stop there) and \uHHHH \UHHHHHHHH
// (written back as typed when the character is not ASCII). A % starts a
// directive, which takes the next argument:
//   %s  a string          %b  a string with its escapes read (\0NNN, \c too)
//   %c  its first byte    %d %i  a signed number     %u  unsigned
//   %o %x %X  unsigned, in octal and in hex          %%  a %
// between the % and the letter: the flags - + space # 0, a width, a . and a
// precision (* takes either from an argument). A number may be written as
// 0x1f, 010 or 0b101, and 'c is the code of the character c. Numbers are 64
// bits. When the format has been used and arguments are left, it is used
// again; a directive with no argument left writes "" or 0. An argument that
// is not a number says so and the status is 1, and the writing goes on.
// The floating point directives (%e %f %g %a) and %q are not here: they stop
// with a message. A bad directive stops there with a message and status 1.
// printf --help writes the usage; a usage error is status 2.
#include "ulib.h"
#include "pfcore.h"

static char ob[1024];
static uint32_t on;

// A reader that has gone away ends printf quietly.
static void flush(void) {
    const char* p = ob;
    while (on) {
        int w = sys_write(STDOUT_FILENO, p, on);
        if (w <= 0) {
            if (w != -EPIPE) dprintf(STDERR_FILENO, "printf: write error: %s\n", w ? strerror(w) : "no progress");
            sys_exit(1);
        }
        p += w;
        on -= (uint32_t)w;
    }
}

static void out(void* ctx, const char* s, uint32_t n) {
    (void)ctx;
    while (n--) {
        if (on == sizeof ob) flush();
        ob[on++] = *s++;
    }
}

static void err(void* ctx, const char* s, uint32_t n) {
    (void)ctx;
    flush();
    while (n) {
        int w = sys_write(STDERR_FILENO, s, n);
        if (w <= 0) return;
        s += w;
        n -= (uint32_t)w;
    }
}

int main(int argc, char** argv) {
    static const char usage[] = "usage: printf FORMAT [ARG...]\n";
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        out(0, usage, sizeof usage - 1);
        flush();
        return 0;
    }
    int a = 1;
    if (a < argc && !strcmp(argv[a], "--")) a++;
    if (a >= argc) {
        dprintf(STDERR_FILENO, "printf: missing operand\n%s", usage);
        return 2;
    }
    pf_io_t io = {out, err, 0};
    int status = pf_run(&io, argv[a], argc - a - 1, argv + a + 1);
    flush();
    return status;
}
