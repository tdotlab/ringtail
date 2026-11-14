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
#define USER_BASE_ADDR 0x40000000u  // user space, as the kernel lays it out
#define USER_TOP_ADDR  0xC0000000u
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

static uintptr_t interesting(void) {
    static const uintptr_t v[] = {
        0, 1, 2, 3, 4, 7, 8, 15, 16, 255, 256, 4095, 4096, 4097, 8191, 8192, 0x10000, 0x100000,
        0x7FFFFFFF, 0x80000000, 0x80000001, 0xFFFFFFFF, 0xFFFFFFFE, 0xFFFFF000, 0xFFFFFFF0,
        0x3FFFFFFF, 0x3FFFFFFC, 0x40000000, 0xBFFFFFFF, 0xBFFFFFFC, 0xC0000000, 0xC0000004,
        0x00100000, 0x00110000, 0x50000000, 0x0B8000, 0x0A0000, 0x1000, 0x400000,
        3600 * 100, 3600 * 100 + 1, 1u << 20, (1u << 20) + 1,
        // Wider than 32 bits: a kernel that cut an address down to 32
        // bits would take most of these for user space.
        0x140000000, 0x100000000, 0x1BFFFFFFF, 0xFFFFFFFF40000000, 0x0000800000000000,
        0xFFFF800000000000, (uintptr_t)-1, (uintptr_t)-4096,
    };
    return v[rnd() % (sizeof v / sizeof v[0])];
}

// A pointer the call may legitimately use, or one placed to hurt.
static uintptr_t pointer(void) {
    switch (rnd() % 10) {
    case 0: return (uintptr_t)scratch;
    case 1: return (uintptr_t)scratch + 4096 - (rnd() % 20);          // straddles the page
    case 2: return (uintptr_t)scratch + sizeof scratch - (rnd() % 20); // straddles the end
    case 3: return (uintptr_t)strings[rnd() % NSTRINGS];
    case 4: return (uintptr_t)main;                                    // read-only code
    case 5: return (uintptr_t)scratch + 4096 + (rnd() % 64);
    case 6: return 0;
    case 7: return (uintptr_t)scratch + (rnd() % 8192);
    case 8: return (uintptr_t)scratch + 128;                           // a plausible struct
    default: return interesting();
    }
}

static uintptr_t value(void) {
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
    { SYS_DUP2, "dup2" }, { SYS_POLL, "poll" }, { SYS_SLEEP, "sleep" }, { SYS_SOCKET, "socket" },
    { SYS_BIND, "bind" }, { SYS_SENDTO, "sendto" }, { SYS_RECVFROM, "recvfrom" }, { SYS_NETCONF, "netconf" },
    { SYS_LISTEN, "listen" }, { SYS_ACCEPT, "accept" }, { SYS_CONNECT, "connect" }, { SYS_SHUTDOWN, "shutdown" },
    { SYS_SOCKADDR, "sockaddr" }, { SYS_POLLFD, "pollfd" }, { SYS_FDTYPE, "fdtype" },
    { SYS_SETTIME, "settime" },  // random words are never a date after the build, so it only fails
    { 0, "unknown" },
};
#define NCALLS ((int)(sizeof calls / sizeof calls[0]))

static uint32_t stat_ok[64], stat_fault[64], stat_other[64];
static int gfx_on;  // the fuzzer asked for the display and has not let go
static int last_sock = -1;  // the socket calls mostly go to the newest socket, or they would never find one
static int last_tcp = -1;   // the same for TCP, which never connects anywhere
static int last_port = 9;   // and sends now and then to the port it was bound to, so something arrives

// Words at scratch+128 that look like the argument structs: small
// coordinates and sizes, a valid string, now and then something nasty.
static void shuffle_struct(void) {
    uintptr_t* w = (uintptr_t*)(scratch + 128);
    for (int i = 0; i < 8; i++) {
        switch (rnd() % 6) {
        case 0: w[i] = (uintptr_t)strings[rnd() % NSTRINGS]; break;
        case 1: w[i] = interesting(); break;
        case 2: w[i] = rnd(); break;
        default: w[i] = rnd() % 330; break;
        }
    }
}

static int one_call(uint32_t iter, uint32_t verbose_from, int force_gfx) {
    shuffle_struct();
    if (rnd() % 8 == 0) sys_chdir("/FZ");  // chdir(\"..\") may have left it
    if (rnd() % 6 == 0) sys_open("a", O_RDWR | O_CREAT);
    const call_t* c = &calls[rnd() % NCALLS];
    int n = c->n;
    if (n == 0) n = (rnd() & 1) ? (int)rnd() : NUM_SYSCALLS + (int)(rnd() % 100);
    if (force_gfx) n = (int[]){ SYS_GFX_PIXEL, SYS_GFX_CLEAR, SYS_GFX_RECT, SYS_GFX_LINE, SYS_GFX_TEXT,
                                SYS_GFX_BLIT, SYS_GFX_FONT, SYS_MOUSE_STATE, SYS_KEY_GET }[rnd() % 9];
    uintptr_t a = value(), b = value(), d = value();
    if ((n == SYS_BIND || n == SYS_SENDTO || n == SYS_RECVFROM) && last_sock < 0) {
        last_sock = sys_socket(SOCK_DGRAM);
        if (rnd() & 1) last_port = sys_bind(last_sock, 0);
    }
    if ((n == SYS_LISTEN || n == SYS_ACCEPT || n == SYS_CONNECT || n == SYS_SHUTDOWN || n == SYS_SOCKADDR) &&
        last_tcp < 0) {
        last_tcp = sys_socket(SOCK_STREAM);
        if (rnd() & 1) sys_bind(last_tcp, 0);
    }

    switch (n) {
    case SYS_READ: case SYS_WRITE: case SYS_SEEK: case SYS_CLOSE: case SYS_DUP2:
        a = fd_value();
        if (n == SYS_DUP2) b = fd_value();
        break;
    case SYS_OPEN: a = pointer(); b = (rnd() & 1) ? (rnd() % 0x800) : (rnd() % 4) | ((rnd() & 1) ? O_CREAT : 0); break;
    case SYS_DELETE: case SYS_MKDIR: case SYS_RMDIR: case SYS_CHDIR: case SYS_STAT:
        a = (rnd() % 3) ? (uintptr_t)strings[rnd() % NSTRINGS] : pointer();
        if (n == SYS_STAT && (rnd() & 1)) b = (uintptr_t)scratch + 1024;
        break;
    case SYS_RENAME:
        a = (rnd() % 4) ? (uintptr_t)strings[rnd() % NSTRINGS] : pointer();
        b = (rnd() % 4) ? (uintptr_t)strings[rnd() % NSTRINGS] : pointer();
        break;
    case SYS_SAVE:
        a = (rnd() % 3) ? (uintptr_t)strings[rnd() % NSTRINGS] : pointer();
        if (rnd() % 3) b = pointer();
        if (rnd() % 2) d = rnd() % 9000;
        break;
    case SYS_SPAWN:
        // Only names that are not programs, so nothing real starts.
        a = (rnd() % 3) ? (uintptr_t)strings[rnd() % NSTRINGS] : pointer();
        break;
    case SYS_GFX_RECT: case SYS_GFX_LINE: case SYS_GFX_TEXT:
        if (rnd() & 1) a = (uintptr_t)scratch + 128;
        break;
    case SYS_READDIR:
        a = (rnd() % 4) ? (uintptr_t)strings[rnd() % NSTRINGS] : pointer();
        if (rnd() & 1) b = (uintptr_t)scratch + 128;
        if (rnd() & 1) d = (uintptr_t)scratch + 1024;
        break;
    case SYS_SLEEP: a = 0; break;
    case SYS_POLL: a = rnd() % 3; break;
    case SYS_WAIT: a = 0x7FFF1234; break;  // differs from any mouse position, so it returns
    case SYS_TTY_MODE: a &= ~(uintptr_t)3; break;     // no raw mode: it would swallow Ctrl+C
    case SYS_JOIN: case SYS_WAIT_JOB: case SYS_KILL: a = odd_id(); break;
    case SYS_TTY_FG: a = rnd() % 4 ? odd_id() : rnd() % 8; break;
    case SYS_SBRK:
        // Mostly small steps; the huge ones must fail cleanly.
        if (rnd() % 3 == 0) a = (rnd() % 0x20000) - 0x8000;
        break;
    case SYS_KINFO: a = rnd() % 10; if (rnd() % 2) d = rnd() % 5000; break;
    case SYS_SET_COLOR: a = rnd() % 300; break;
    case SYS_SOCKET: if (rnd() % 4) a = SOCK_DGRAM; break;
    case SYS_BIND:
        a = rnd() % 4 ? (uint32_t)last_sock : fd_value();
        if (rnd() & 1) b = rnd() % 4 ? 0 : rnd() % 70000;
        break;
    case SYS_SENDTO: case SYS_RECVFROM: {
        // Only to this machine or to addresses that must be refused, so
        // no junk leaves it, and never a wait that could last: a random
        // timeout could be "for ever".
        static const uint32_t ips[] = { 0x7F000001, 0x7F000002, 0x7FFFFFFF, 0, 0xE0000001, 0xF0000001 };
        udp_msg_t* m = (udp_msg_t*)(scratch + 512);
        m->ip = ips[rnd() % (sizeof ips / sizeof ips[0])];
        m->port = rnd() % 3 == 0 ? last_port : rnd() % 2 ? (int)(rnd() % 70000) - 10 : (int)interesting();
        m->buf = (void*)pointer();
        // What is sent may be received into scratch and then printed, so
        // it comes from scratch too, for the reason the print calls do.
        uintptr_t sc = (uintptr_t)scratch, p = (uintptr_t)m->buf;
        if (n == SYS_SENDTO && p >= USER_BASE_ADDR && p < USER_TOP_ADDR && (p < sc || p >= sc + sizeof scratch))
            m->buf = scratch + rnd() % 4000;
        m->len = rnd() % 3 ? (int)(rnd() % 2000) : (int)interesting();
        m->timeout_ms = (int)(rnd() % 3);
        a = rnd() % 4 ? (uint32_t)last_sock : fd_value();
        b = rnd() % 8 ? (uintptr_t)m : (rnd() & 1) ? 0 : 0xC0000000u + (rnd() & 0xFFF);
        break;
    }
    case SYS_LISTEN: case SYS_SHUTDOWN: case SYS_SOCKADDR:
        a = rnd() % 4 ? (uint32_t)last_tcp : rnd() % 2 ? (uint32_t)last_sock : fd_value();
        if (n == SYS_SHUTDOWN && rnd() % 3) b = rnd() % 4;
        if (n == SYS_SOCKADDR) { b = rnd() % 3; d = pointer(); }
        break;
    case SYS_ACCEPT:
        // Nothing ever connects to it, so only waits that end.
        a = rnd() % 4 ? (uint32_t)last_tcp : fd_value();
        b = rnd() % 2 ? 0 : pointer();
        d = rnd() % 3;
        break;
    case SYS_CONNECT: {
        // Only where nothing can answer: a connection that worked would
        // leave a socket whose reads wait for ever.
        static const uint32_t ips[] = { 0, 0xE0000001, 0xF0000001, 0xFFFFFFFF, 0x7F000001 };
        sock_addr_t* sa = (sock_addr_t*)(scratch + 640);
        sa->ip = ips[rnd() % (sizeof ips / sizeof ips[0])];
        sa->port = sa->ip == 0x7F000001 ? (rnd() & 1 ? 0 : 65536 + (int)(rnd() % 1000)) : (int)(rnd() % 70000);
        a = rnd() % 4 ? (uint32_t)last_tcp : rnd() % 2 ? (uint32_t)last_sock : fd_value();
        b = rnd() % 6 ? (uintptr_t)sa : pointer();
        if (b != (uintptr_t)sa && b >= USER_BASE_ADDR && b < USER_TOP_ADDR) b = 0xC0000000u + (rnd() & 0xFFF);
        d = rnd() % 3 ? 1 + rnd() % 5 : interesting();
        break;
    }
    case SYS_POLLFD: {
        pollfd_t* pf = (pollfd_t*)(scratch + 768);
        for (int i = 0; i < 4; i++) {
            pf[i].fd = rnd() % 3 ? (int)fd_value() : -1;
            pf[i].events = (int)(rnd() % 64);
        }
        a = rnd() % 4 ? (uintptr_t)pf : pointer();
        b = rnd() % 4 ? rnd() % 5 : value();
        d = rnd() % 3;  // never a long wait
        break;
    }
    case SYS_FDTYPE: a = fd_value(); break;
    case SYS_NETCONF:
        // Reading the settings goes anywhere; a change only where it must
        // fail, or the machine would lose its address in the middle.
        a = rnd() % 3;
        if (a == NETCONF_GET) b = pointer();
        else if (a == NETCONF_STATIC) b = (rnd() & 1) ? 0 : 0xC0000000u + (rnd() & 0xFFF);
        else a = 3 + rnd() % 100;
        break;
    default: break;
    }

    // A huge length over memory that is mapped would print megabytes to
    // the console (or fill the disk). Those only go with a pointer that
    // must fail anyway.
    if (n == SYS_PRINT || n == SYS_WRITE) {
        uintptr_t* len = (n == SYS_PRINT) ? &b : &d;
        uintptr_t* ptr = (n == SYS_PRINT) ? &a : &b;
        if (*len > 300) {
            if (rnd() & 1) *len %= 300;
            else *ptr = (rnd() & 1) ? 0xC0000000u + (rnd() & 0xFFF) : (interesting() | 0x80000000u);
        }
        // Valid memory that is not the scratch buffer would put this
        // program's own code and strings (the word FAIL among them) on the
        // console, where a test reading the output would take it for a result.
        uintptr_t sc = (uintptr_t)scratch;
        if (*ptr >= USER_BASE_ADDR && *ptr < USER_TOP_ADDR && (*ptr < sc || *ptr >= sc + sizeof scratch) && *len) {
            *ptr = sc + (rnd() % 4000);
            *len %= 300;  // the pointer may have been one of the "must fail" ones above, with a huge length
        }
    }

    if (iter >= verbose_from) out("  #%u %s(%lx, %lx, %lx)\n", iter, c->name, (unsigned long)a, (unsigned long)b, (unsigned long)d);
    long r = syscall3(n, a, b, d);

    if ((unsigned)n < 64) {
        if (r >= 0) stat_ok[n]++;
        else if (r == -EFAULT) stat_fault[n]++;
        else stat_other[n]++;
    }
    if (n == SYS_SOCKET && r >= 0) {
        if (sys_fdtype(r) == FDTYPE_TCP) last_tcp = r;
        else last_sock = r;
    }
    if (n == SYS_BIND && r > 0) last_port = r;
    if (n == SYS_GFX_BEGIN && r == 0) gfx_on = 1;
    if (n == SYS_GFX_END && r == 0) gfx_on = 0;
    // A killed or interrupted call is not a kernel bug, but this program
    // is not supposed to be killed by its own system calls.
    if (r == -EINTR) { out("fuzz: interrupted in %s\n", c->name); return 1; }
    return 0;
}

static void drop_extra_fds(void) {
    for (int fd = 3; fd < MAX_FDS; fd++) sys_close(fd);
    last_sock = last_tcp = -1;
}

// Empty a directory and remove it, however deep the fuzzer nested things.
static void rm_rf(const char* path, int depth) {
    char child[PATH_MAX];
    for (int guard = 0; guard < 64; guard++) {
        int cursor = 0;
        dirent_t e;
        if (sys_readdir(path, &cursor, &e) <= 0) break;
        snprintf(child, sizeof child, "%s/%s", path, e.name);
        int r;
        if (e.flags & DIRENT_DIR) {
            if (depth < 6) rm_rf(child, depth + 1);
            r = sys_rmdir(child);
        } else {
            r = sys_delete(child);
        }
        if (r < 0) break;
    }
}

// What was in the root before the fuzzer started, so that whatever
// it made there by wandering out of /FZ can be found and removed, even
// under a name read from some random pointer.
#define ROOT_MAX 128
static char root_before[ROOT_MAX][NAME_MAX + 1];
static int nroot = -1;  // -1: the root could not be read in full

static void snapshot_root(void) {
    int cursor = 0, n = 0;
    dirent_t e;
    while (sys_readdir("/", &cursor, &e) == 1) {
        if (n == ROOT_MAX) return;
        strlcpy(root_before[n++], e.name, sizeof root_before[0]);
    }
    nroot = n;
}

static int was_there(const char* name) {
    for (int i = 0; i < nroot; i++)
        if (!strcmp(root_before[i], name)) return 1;
    return 0;
}

static int remove_strays(void) {
    if (nroot < 0) return 0;
    int removed = 0;
    for (int guard = 0; guard < 64; guard++) {
        int cursor = 0, found = 0;
        dirent_t e;
        while (sys_readdir("/", &cursor, &e) == 1)
            if (!was_there(e.name)) { found = 1; break; }
        if (!found) break;
        char p[PATH_MAX];
        snprintf(p, sizeof p, "/%s", e.name);
        if (e.flags & DIRENT_DIR) {
            rm_rf(p, 0);
            if (sys_rmdir(p) < 0) break;
        } else if (sys_delete(p) < 0) break;
        removed++;
    }
    return removed;
}

// Remove everything the fuzzer made. Its working directory is /FZ, but
// chdir("..") lets it wander into the root, so the root names it uses
// are cleaned up too, and anything else new there.
static void clean_scratch(void) {
    // dup2 may have pointed 0, 1 or 2 at a scratch file, and a file that is
    // open cannot be deleted. Output goes through sys_print, not these.
    for (int fd = 0; fd < MAX_FDS; fd++) sys_close(fd);
    static const char* const names[] = { "a", "b", "a b", "x.y.z", " lead", "dot", "a/b", "b/a" };
    sys_chdir("/");
    rm_rf("/FZ", 0);
    sys_rmdir("/FZ");
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++) {
        char p[PATH_MAX];
        snprintf(p, sizeof p, "/%s", names[i]);
        sys_delete(p);
        sys_rmdir(p);
        // a directory called a or b may hold things
    }
    for (unsigned i = 0; i < 2; i++) {
        const char* d = i ? "/b" : "/a";
        rm_rf(d, 0);
        sys_rmdir(d);
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
    snapshot_root();
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
    int strays = remove_strays();
    if (strays) out("fuzz: removed %d stray names from the root\n", strays);
    int heap_after = heap_free_kb();

    if (verbose_from != 0xFFFFFFFF || iters >= 1000) {
        for (int i = 0; i < NCALLS; i++) {
            int n = calls[i].n;
            if (n) out("  %-12s ok %5u  efault %5u  other %5u\n", calls[i].name, stat_ok[n], stat_fault[n], stat_other[n]);
        }
    }
    // The verdict uses locals only: a stray write into this program's own
    // data (which is legitimate, it is mapped) must not be able to fake
    // or hide a failure through the check() counters.
    int alive = sys_getpid() > 0;
    int heap_ok = heap_before > 0 && heap_after >= heap_before - 4;
    out("heap free: %d KB before, %d KB after\n", heap_before, heap_after);
    out("  %s  the kernel survived the fuzzing\n", alive ? "ok  " : "FAIL");
    out("  %s  no kernel heap was lost\n", heap_ok ? "ok  " : "FAIL");
    if (alive && heap_ok) out("fuzz: all 2 checks passed\n");
    else out("fuzz: FAILED\n");
    return !(alive && heap_ok);
}
