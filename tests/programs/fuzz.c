// A syscall fuzzer: calls the system calls with hostile arguments (kernel
// addresses, unmapped pages, read-only code, wrap-arounds, huge lengths,
// bad descriptors and flags) and checks that nothing goes wrong. The
// kernel must not crash or hang, and no kernel heap may leak.
//
// usage: fuzz [seed [iterations [verbose-from]]]
// The run is deterministic for a given seed. If the machine dies, rerun
// with the last "iter" printed as verbose-from to see the call that did it.
//
// Calls that block or end the program (exit, power, sleep, join, wait
// for input, pipe, spawning real programs) are kept out of the random
// mix, with a few exceptions that take only harmless arguments.
#include <stdarg.h>
#include "ulib.h"

int main(int argc, char** argv);
static uint32_t rng_state = 1;
static uint32_t rnd(void) {  // xorshift32
    uint32_t x = rng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return rng_state = x;
}

// Output goes through sys_print: the fuzzer closes and redirects its own
// descriptors, so printf could end up in a file.
static void out(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void out(const char* fmt, ...) {
    char line[200];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n >= (int)sizeof line) n = sizeof line - 1;
    sys_print(line, (uint32_t)n);
}

static char scratch[8192] __attribute__((aligned(4096)));  // two pages, all spaces
static char longname[400];

static const char* const strings[] = {
    "a", "b", "a/b", "b/a", "FZ", "..", ".", "/", "", "a/../b", "../FZ", "a/", "a b", "x.y.z",
    "con:", "A?", "a\\b", " lead", "trail ", "dot.", longname,
};
#define NSTRINGS ((int)(sizeof strings / sizeof strings[0]))

static uint32_t interesting(void) {
    static const uint32_t v[] = {
        0, 1, 2, 3, 4, 7, 8, 15, 16, 255, 256, 4095, 4096, 4097, 8191, 8192, 0x10000, 0x100000,
        0x7FFFFFFF, 0x80000000, 0x80000001, 0xFFFFFFFF, 0xFFFFFFFE, 0xFFFFF000, 0xFFFFFFF0,
        0x3FFFFFFF, 0x3FFFFFFC, 0x40000000, 0xBFFFFFFF, 0xBFFFFFFC, 0xC0000000, 0xC0000004,
        0x00100000, 0x00110000, 0x50000000, 0x0B8000, 0x0A0000, 0x1000, 0x400000,
        3600 * 100, 3600 * 100 + 1, 1u << 20, (1u << 20) + 1,
    };
    return v[rnd() % (sizeof v / sizeof v[0])];
}

// A pointer the call may legitimately use, or one placed to hurt.
static uint32_t pointer(void) {
    switch (rnd() % 10) {
    case 0: return (uint32_t)scratch;
    case 1: return (uint32_t)scratch + 4096 - (rnd() % 20);          // straddles the page
    case 2: return (uint32_t)scratch + sizeof scratch - (rnd() % 20); // straddles the end
    case 3: return (uint32_t)strings[rnd() % NSTRINGS];
    case 4: return (uint32_t)main;                                    // read-only code
    case 5: return (uint32_t)scratch + 4096 + (rnd() % 64);
    case 6: return 0;
    case 7: return (uint32_t)scratch + (rnd() % 8192);
    case 8: return (uint32_t)scratch + 128;                           // a plausible struct
    default: return interesting();
    }
}

static uint32_t value(void) {
    switch (rnd() % 10) {
    case 0: case 1: case 2: return interesting();
    case 3: case 4: case 5: return pointer();
    case 6: case 7: return rnd() % 40;
    case 8: return rnd() % 4096;
    default: return rnd();
    }
}

static uint32_t fd_value(void) {
    switch (rnd() % 8) {
    case 0: return 0xFFFFFFFF;
    case 1: return MAX_FDS + (rnd() % 3);
    case 2: return rnd();
    case 3: case 4: return 3 + rnd() % 3;
    default: return rnd() % MAX_FDS;
    }
}

// Ids that name no task. Real ones are left alone: joining the shell
// would wait for it forever, and killing it would end the run.
static uint32_t odd_id(void) {
    static const uint32_t v[] = { 0, 0xFFFFFFFF, 0x7FFFFFFF, 0x80000000, 99999, 0x10000, 0xFFFFFFFE };
    return v[rnd() % (sizeof v / sizeof v[0])];
}

typedef struct { int n; const char* name; } call_t;

// What the random loop may call. Argument 1 is mostly a descriptor for
// the calls that take one; the rest are anything.
static const call_t calls[] = {
    { SYS_PRINT, "print" }, { SYS_OPEN, "open" }, { SYS_READ, "read" }, { SYS_WRITE, "write" },
    { SYS_CLOSE, "close" }, { SYS_DELETE, "delete" }, { SYS_MKDIR, "mkdir" }, { SYS_RMDIR, "rmdir" },
    { SYS_CHDIR, "chdir" }, { SYS_GETCWD, "getcwd" }, { SYS_RENAME, "rename" }, { SYS_STAT, "stat" },
    { SYS_TIME, "time" }, { SYS_SAVE, "save" }, { SYS_GFX_BEGIN, "gfx_begin" }, { SYS_GFX_END, "gfx_end" },
    { SYS_GFX_PIXEL, "gfx_pixel" }, { SYS_GFX_CLEAR, "gfx_clear" }, { SYS_GFX_RECT, "gfx_rect" },
    { SYS_GFX_LINE, "gfx_line" }, { SYS_GFX_TEXT, "gfx_text" }, { SYS_GFX_BLIT, "gfx_blit" },
    { SYS_GFX_FONT, "gfx_font" }, { SYS_MOUSE_STATE, "mouse_state" }, { SYS_KEY_GET, "key_get" },
    { SYS_SEEK, "seek" }, { SYS_LIST, "list" }, { SYS_SPAWN, "spawn" }, { SYS_WAIT, "wait" },
    { SYS_TICKS, "ticks" }, { SYS_GETPID, "getpid" }, { SYS_JOIN, "join" }, { SYS_SBRK, "sbrk" },
    { SYS_TTY_MODE, "tty_mode" }, { SYS_TTY_FG, "tty_fg" }, { SYS_WAIT_JOB, "wait_job" },
    { SYS_KILL, "kill" }, { SYS_READDIR, "readdir" }, { SYS_KINFO, "kinfo" }, { SYS_SET_COLOR, "set_color" },
    { SYS_DUP2, "dup2" }, { SYS_POLL, "poll" }, { SYS_SLEEP, "sleep" }, { 0, "unknown" },
};
#define NCALLS ((int)(sizeof calls / sizeof calls[0]))

static uint32_t stat_ok[64], stat_fault[64], stat_other[64];
static int gfx_on;  // the fuzzer asked for the display and has not let go

// Words at scratch+128 that look like the argument structs: small
// coordinates and sizes, a valid string, now and then something nasty.
static void shuffle_struct(void) {
    uint32_t* w = (uint32_t*)(scratch + 128);
    for (int i = 0; i < 8; i++) {
        switch (rnd() % 6) {
        case 0: w[i] = (uint32_t)strings[rnd() % NSTRINGS]; break;
        case 1: w[i] = interesting(); break;
        case 2: w[i] = rnd(); break;
        default: w[i] = rnd() % 330; break;
        }
    }
}

static int one_call(uint32_t iter, uint32_t verbose_from, int force_gfx) {
    shuffle_struct();
    if (rnd() % 6 == 0) sys_open("a", O_RDWR | O_CREAT);
    const call_t* c = &calls[rnd() % NCALLS];
    int n = c->n;
    if (n == 0) n = (rnd() & 1) ? (int)rnd() : NUM_SYSCALLS + (int)(rnd() % 100);
    if (force_gfx) n = (int[]){ SYS_GFX_PIXEL, SYS_GFX_CLEAR, SYS_GFX_RECT, SYS_GFX_LINE, SYS_GFX_TEXT,
                                SYS_GFX_BLIT, SYS_GFX_FONT, SYS_MOUSE_STATE, SYS_KEY_GET }[rnd() % 9];
    uint32_t a = value(), b = value(), d = value();

    switch (n) {
    case SYS_READ: case SYS_WRITE: case SYS_SEEK: case SYS_CLOSE: case SYS_DUP2:
        a = fd_value();
        if (n == SYS_DUP2) b = fd_value();
        break;
    case SYS_OPEN: a = pointer(); b = (rnd() & 1) ? (rnd() % 0x800) : (rnd() % 4) | ((rnd() & 1) ? O_CREAT : 0); break;
    case SYS_DELETE: case SYS_MKDIR: case SYS_RMDIR: case SYS_CHDIR: case SYS_STAT:
        a = (rnd() % 3) ? (uint32_t)strings[rnd() % NSTRINGS] : pointer();
        if (n == SYS_STAT && (rnd() & 1)) b = (uint32_t)scratch + 1024;
        break;
    case SYS_RENAME:
        a = (rnd() % 4) ? (uint32_t)strings[rnd() % NSTRINGS] : pointer();
        b = (rnd() % 4) ? (uint32_t)strings[rnd() % NSTRINGS] : pointer();
        break;
    case SYS_SAVE:
        a = (rnd() % 3) ? (uint32_t)strings[rnd() % NSTRINGS] : pointer();
        if (rnd() % 3) b = pointer();
        if (rnd() % 2) d = rnd() % 9000;
        break;
    case SYS_SPAWN:
        // Only names that are not programs, so nothing real starts.
        a = (rnd() % 3) ? (uint32_t)strings[rnd() % NSTRINGS] : pointer();
        break;
    case SYS_GFX_RECT: case SYS_GFX_LINE: case SYS_GFX_TEXT:
        if (rnd() & 1) a = (uint32_t)scratch + 128;
        break;
    case SYS_READDIR:
        a = (rnd() % 4) ? (uint32_t)strings[rnd() % NSTRINGS] : pointer();
        if (rnd() & 1) b = (uint32_t)scratch + 128;
        if (rnd() & 1) d = (uint32_t)scratch + 1024;
        break;
    case SYS_SLEEP: a = 0; break;
    case SYS_POLL: a = rnd() % 3; break;
    case SYS_WAIT: a = 0x7FFF1234; break;  // differs from any mouse position, so it returns
    case SYS_TTY_MODE: a &= ~3u; break;     // no raw mode: it would swallow Ctrl+C
    case SYS_JOIN: case SYS_WAIT_JOB: case SYS_KILL: a = odd_id(); break;
    case SYS_TTY_FG: a = rnd() % 4 ? odd_id() : rnd() % 8; break;
    case SYS_SBRK:
        // Mostly small steps; the huge ones must fail cleanly.
        if (rnd() % 3 == 0) a = (rnd() % 0x20000) - 0x8000;
        break;
    case SYS_KINFO: a = rnd() % 10; if (rnd() % 2) d = rnd() % 5000; break;
    case SYS_SET_COLOR: a = rnd() % 300; break;
    default: break;
    }

    if (iter >= verbose_from) out("  #%u %s(%x, %x, %x)\n", iter, c->name, a, b, d);
    int r = syscall3(n, a, b, d);

    if ((unsigned)n < 64) {
        if (r >= 0) stat_ok[n]++;
        else if (r == -EFAULT) stat_fault[n]++;
        else stat_other[n]++;
    }
    if (n == SYS_GFX_BEGIN && r == 0) gfx_on = 1;
    if (n == SYS_GFX_END && r == 0) gfx_on = 0;
    // A killed or interrupted call is not a kernel bug, but this program
    // is not supposed to be killed by its own system calls.
    if (r == -EINTR) { out("fuzz: interrupted in %s\n", c->name); return 1; }
    return 0;
}

static void drop_extra_fds(void) {
    for (int fd = 3; fd < MAX_FDS; fd++) sys_close(fd);
}

// Remove everything the fuzzer may have made in /FZ, two levels deep.
static void clean_scratch(void) {
    static const char* const names[] = { "a/b", "b/a", "a/x", "b/x", "a", "b", "a b", "x.y.z", " lead", "dot", "A?" };
    sys_chdir("/");
    for (int pass = 0; pass < 3; pass++) {
        char p[PATH_MAX];
        for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++) {
            snprintf(p, sizeof p, "/FZ/%s", names[i]);
            sys_delete(p);
            sys_rmdir(p);
        }
        // Whatever else: list /FZ and remove each entry.
        int cursor = 0;
        dirent_t d;
        while (sys_readdir("/FZ", &cursor, &d) > 0) {
            snprintf(p, sizeof p, "/FZ/%s", d.name);
            if (d.flags & DIRENT_DIR) {
                // Empty a nested directory, then remove it.
                char q[PATH_MAX];
                int c2 = 0;
                dirent_t e;
                while (sys_readdir(p, &c2, &e) > 0) {
                    snprintf(q, sizeof q, "%s/%s", p, e.name);
                    sys_delete(q);
                    sys_rmdir(q);
                }
                sys_rmdir(p);
            } else {
                sys_delete(p);
            }
        }
    }
}

static int heap_free_kb(void) {
    char text[256];
    sys_kinfo(KINFO_MEM, text, sizeof text);
    for (const char* h = text; *h; h++)
        if (h[0] == 'H' && h[1] == 'e' && h[2] == 'a' && h[3] == 'p' && h[4] == ':') {
            h += 5;
            while (*h == ' ') h++;
            int v = 0;
            while (*h >= '0' && *h <= '9') v = v * 10 + (*h++ - '0');
            return v;
        }
    return -1;
}

int main(int argc, char** argv) {
    uint32_t seed = 1, iters = 3000, verbose_from = 0xFFFFFFFF;
    if (argc > 1) parse_uint(argv[1], &seed);
    if (argc > 2) parse_uint(argv[2], &iters);
    if (argc > 3) parse_uint(argv[3], &verbose_from);
    if (!seed) seed = 1;
    rng_state = seed;

    memset(scratch, ' ', sizeof scratch);
    memset(longname, 'n', sizeof longname - 1);

    // Reading the console would wait for a line; put a file in its place.
    int f = sys_open("WELCOME.TXT", O_RDONLY);
    if (f >= 0) { sys_dup2(f, 0); sys_close(f); }
    sys_mkdir("/FZ");
    sys_chdir("/FZ");

    int heap_before = heap_free_kb();
    out("fuzz: seed %u, %u calls\n", seed, iters);
    for (uint32_t i = 0; i < iters; i++) {
        if (i % 500 == 0) out("iter %u\n", i);
        if (i % 40 == 39) drop_extra_fds();
        if (one_call(i, verbose_from, 0)) return 1;
    }

    // The display calls only get past the owner check while we hold it.
    out("fuzz: graphics, %u calls\n", iters / 2);
    sys_gfx_begin();
    gfx_on = 1;
    for (uint32_t i = 0; i < iters / 2; i++) {
        if (i % 500 == 0) out("giter %u\n", i);
        if (!gfx_on) sys_gfx_begin(), gfx_on = 1;
        // Rects and lines this large would take forever, honestly. Keep
        // the sizes to what a screen could hold times a bit.
        if (one_call(0x80000000u + i, verbose_from, 1)) break;
    }
    if (gfx_on) sys_gfx_end();

    drop_extra_fds();
    clean_scratch();
    int heap_after = heap_free_kb();

    if (verbose_from != 0xFFFFFFFF || iters >= 1000) {
        for (int i = 0; i < NCALLS; i++) {
            int n = calls[i].n;
            if (n) out("  %-12s ok %5u  efault %5u  other %5u\n", calls[i].name, stat_ok[n], stat_fault[n], stat_other[n]);
        }
    }
    check(1, "the kernel survived the fuzzing");
    check(sys_getpid() > 0, "this program still runs");
    check(heap_before > 0 && heap_after >= heap_before - 4, "no kernel heap was lost");
    out("heap free: %d KB before, %d KB after\n", heap_before, heap_after);
    return check_summary("fuzz");
}
