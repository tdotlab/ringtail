// Saklas, the shell: an ordinary program. init starts it at boot and starts
// a new one whenever it ends.
//
// The language is the shell core's (lib/sh.h, lib/shcore.c): POSIX
// shell, with pipelines, && and ||, if, while, for, case, functions,
// redirections, here-documents, variables, $( ) and $(( )). This file is
// what sits around it at the console: the line editor, history, Tab,
// the commands that only make sense here (help, net, mem, color and the
// rest), and how things that went wrong are shown.
//
// `saklas FILE` or `sh FILE` runs a script, `sh -c TEXT` runs TEXT, and
// commands come from standard input when it is not the terminal. Only at
// the terminal does it edit lines and show a prompt.
//
// It edits the line itself in raw mode, with Ctrl+C as a plain key, so
// Ctrl+C at the prompt cancels the line instead of killing the shell.
// While commands run the terminal is back in its usual mode, and Ctrl+C
// stops them: a program in the foreground has a job and the terminal of
// its own and gets the signal; a loop the shell runs itself sees
// sh_interrupted. The shell takes the terminal back once the job is gone.
#include "ulib.h"
#include "sh.h"

#define LINE_MAX 256
#define HISTORY 10

#define RED  "\x1b[91m"
#define DIM  "\x1b[90m"
#define GRN  "\x1b[92m"
#define NORM "\x1b[0m"

static char history[HISTORY][LINE_MAX];
static int hist_count;  // total lines ever added; the ring holds the last HISTORY

static void err(const char* what, int e) {
    const char* why = e == -EINVAL && !name_ok(what) ? "not a valid file name" : strerror(e);
    dprintf(STDERR_FILENO, RED "%s: %s" NORM "\n", what, why);
}

static void complain(const char* msg) {
    dprintf(STDERR_FILENO, RED "%s" NORM "\n", msg);
}


// ---- Reading keys ----

static char inbuf[64];
static int in_pos, in_len;

// Next byte from the terminal, or -1 when there is no terminal to read.
static int getbyte(void) {
    while (in_pos == in_len) {
        int n = sys_read(STDIN_FILENO, inbuf, sizeof inbuf);
        if (n == -EINTR) continue;
        if (n <= 0) return -1;
        in_pos = 0;
        in_len = n;
    }
    return (uint8_t)inbuf[in_pos++];
}

enum { K_UP = 0x100, K_DOWN, K_LEFT, K_RIGHT, K_HOME, K_END, K_DEL, K_WLEFT, K_WRIGHT, K_OTHER };

// One key: a byte, or a cursor key decoded from its escape sequence
// (arrows, Home, End, Delete, and Ctrl+Left/Right or Alt+B/F for a
// word at a time). Other sequences are swallowed.
static int getkey(void) {
    int c = getbyte();
    if (c != 0x1B) return c;
    c = getbyte();
    if (c == 'b') return K_WLEFT;   // Alt+B and Alt+F
    if (c == 'f') return K_WRIGHT;
    if (c != '[' && c != 'O') return c < 0 ? -1 : K_OTHER;
    int param = 0, mod = 0, field = 0, final;
    for (;;) {  // ESC [ param ; modifier final
        final = getbyte();
        if (final < 0) return -1;
        if (final >= '0' && final <= '9') {
            if (field == 0) param = param * 10 + final - '0';
            else if (field == 1) mod = mod * 10 + final - '0';
        } else if (final == ';') {
            field++;
        } else if (final >= 0x40 && final <= 0x7E) {
            break;
        }
    }
    int ctrl = mod > 1 && ((mod - 1) & 4);
    switch (final) {
    case 'A': return K_UP;
    case 'B': return K_DOWN;
    case 'C': return ctrl ? K_WRIGHT : K_RIGHT;
    case 'D': return ctrl ? K_WLEFT : K_LEFT;
    case 'H': return K_HOME;
    case 'F': return K_END;
    case '~':
        return param == 1 || param == 7 ? K_HOME : param == 4 || param == 8 ? K_END : param == 3 ? K_DEL : K_OTHER;
    }
    return K_OTHER;
}

// ---- Line editor ----

static void out(const char* s, int n) { sys_write(STDOUT_FILENO, s, (uint32_t)n); }

static void erase(int n) {
    char buf[3 * 32];
    while (n > 0) {
        int k = n < 32 ? n : 32;
        for (int i = 0; i < k; i++) memcpy(buf + 3 * i, "\b \b", 3);
        out(buf, 3 * k);
        n -= k;
    }
}

// "ringtail:/DOCS> ": the current directory, looked up every time.
// "> " instead while a command goes on over more lines.
static int more;

static void prompt(void) {
    if (more) {
        print("> ");
        return;
    }
    char cwd[PATH_MAX];
    if (sys_getcwd(cwd, sizeof cwd) < 0) strlcpy(cwd, "?", sizeof cwd);
    printf(GRN "ringtail:%s> " NORM, cwd);
}

static int complete(char* buf, int n);  // Tab, defined after the built-ins

// Move the cursor left n cells. A line may wrap; the console's
// backspace goes back across the wrap.
static void back(int n) {
    char tmp[LINE_MAX];
    while (n > 0) {
        int k = n < (int)sizeof tmp ? n : (int)sizeof tmp;
        memset(tmp, '\b', (size_t)k);
        out(tmp, k);
        n -= k;
    }
}

// After an edit: rewrite buf[from..n), blank the `extra` cells the line
// no longer covers, and put the cursor at pos. The cursor is at the
// edit point on the way in.
static void repaint(const char* buf, int from, int n, int extra, int pos) {
    char tmp[LINE_MAX];
    out(buf + from, n - from);
    memset(tmp, ' ', sizeof tmp);
    for (int k = extra; k > 0; k -= (int)sizeof tmp) out(tmp, k < (int)sizeof tmp ? k : (int)sizeof tmp);
    back(n + extra - pos);
}

// Where the cursor ends up after one word left or right of pos.
static int word_left(const char* buf, int pos) {
    while (pos > 0 && buf[pos - 1] == ' ') pos--;
    while (pos > 0 && buf[pos - 1] != ' ') pos--;
    return pos;
}

static int word_right(const char* buf, int pos, int n) {
    while (pos < n && buf[pos] != ' ') pos++;
    while (pos < n && buf[pos] == ' ') pos++;
    return pos;
}

// Returns the line length, -1 if the line was cancelled, -2 when the
// terminal is gone. The cursor can be anywhere in the line: pos is
// where it is and n is how long the line is.
static int read_line(char* buf) {
    sys_tty_mode(TTY_RAW | TTY_NOSIG);
    int n = 0, pos = 0;
    int browse = hist_count;  // history entry shown; hist_count is the line being typed
    for (;;) {
        int c = getkey();
        if (c < 0) return -2;
        int target = pos;  // set by the keys that only move the cursor
        switch (c) {
        case '\n': case '\r':
            print("\n");
            buf[n] = 0;
            return n;
        case 0x03:  // Ctrl+C
            out(buf + pos, n - pos);
            print("^C\n");
            return -1;
        case 0x0C:  // Ctrl+L
            print("\x1b[2J\x1b[H");
            prompt();
            out(buf, n);
            back(n - pos);
            continue;
        case 0x7F: case '\b':  // the character before the cursor
            if (pos > 0) {
                memmove(buf + pos - 1, buf + pos, (size_t)(n - pos));
                pos--, n--;
                back(1);
                repaint(buf, pos, n, 1, pos);
            }
            continue;
        case K_DEL:  // the character under it
            if (pos < n) {
                memmove(buf + pos, buf + pos + 1, (size_t)(n - pos - 1));
                n--;
                repaint(buf, pos, n, 1, pos);
            }
            continue;
        case 0x15:  // Ctrl+U: the whole line
            out(buf + pos, n - pos);
            erase(n);
            n = pos = 0;
            continue;
        case 0x0B:  // Ctrl+K: from the cursor to the end
            repaint(buf, pos, pos, n - pos, pos);
            n = pos;
            continue;
        case 0x17: {  // Ctrl+W, Ctrl+Backspace: the word before the cursor
            int k = word_left(buf, pos), cut = pos - k;
            memmove(buf + k, buf + pos, (size_t)(n - pos));
            n -= cut;
            back(cut);
            pos = k;
            repaint(buf, pos, n, cut, pos);
            continue;
        }
        case K_UP: case K_DOWN: {
            int oldest = hist_count > HISTORY ? hist_count - HISTORY : 0;
            int next = browse + (c == K_UP ? -1 : 1);
            if (next < oldest || next > hist_count) continue;
            browse = next;
            out(buf + pos, n - pos);
            erase(n);
            if (browse == hist_count) n = 0;
            else n = (int)strlcpy(buf, history[browse % HISTORY], LINE_MAX);
            out(buf, n);
            pos = n;
            continue;
        }
        case '\t':  // completion works on the end of the line
            out(buf + pos, n - pos);
            n = complete(buf, n);
            pos = n;
            continue;
        case K_LEFT: case 0x02: target = pos > 0 ? pos - 1 : 0; break;
        case K_RIGHT: case 0x06: target = pos < n ? pos + 1 : n; break;
        case K_HOME: case 0x01: target = 0; break;
        case K_END: case 0x05: target = n; break;
        case K_WLEFT: target = word_left(buf, pos); break;
        case K_WRIGHT: target = word_right(buf, pos, n); break;
        default:
            if (c >= 0x20 && c < 0x7F && n < LINE_MAX - 1) {  // insert at the cursor
                memmove(buf + pos + 1, buf + pos, (size_t)(n - pos));
                buf[pos++] = (char)c;
                n++;
                repaint(buf, pos - 1, n, 0, pos);
            }
            continue;
        }
        if (target < pos) back(pos - target);
        else out(buf + pos, target - pos);
        pos = target;
    }
}

static void history_add(const char* line) {
    if (hist_count && strcmp(history[(hist_count - 1) % HISTORY], line) == 0) return;
    strlcpy(history[hist_count % HISTORY], line, LINE_MAX);
    hist_count++;
}

// ---- Built-in commands ----
//
// The shell core has the language's own (cd, echo, read, test, set and
// so on); these are the console's. Each gets argv like a program and
// returns an exit status. They write with printf, which goes straight to
// descriptor 1, so redirections and pipes work on them as on programs.

static int usage(const char* name);

static int show_kinfo(int what) {
    static char buf[4096];
    int r = sys_kinfo(what, buf, sizeof buf);
    if (r < 0) {
        err("kinfo", r);
        return 1;
    }
    print(buf);
    return 0;
}

static int cmd_help(int argc, char** argv);

static int cmd_clear(int argc, char** argv) { (void)argc; (void)argv; print("\x1b[2J\x1b[H"); return 0; }
static int cmd_version(int argc, char** argv) { (void)argc; (void)argv; return show_kinfo(KINFO_VERSION); }
static int cmd_tasks(int argc, char** argv) { (void)argc; (void)argv; return show_kinfo(KINFO_TASKS); }
static int cmd_cpus(int argc, char** argv) { (void)argc; (void)argv; return show_kinfo(KINFO_CPUS); }
static int cmd_faults(int argc, char** argv) { (void)argc; (void)argv; return show_kinfo(KINFO_FAULTS); }
static int cmd_mem(int argc, char** argv) { (void)argc; (void)argv; return show_kinfo(KINFO_MEM); }
static int cmd_heap(int argc, char** argv) { (void)argc; (void)argv; return show_kinfo(KINFO_HEAP); }
static int cmd_mmap(int argc, char** argv) { (void)argc; (void)argv; return show_kinfo(KINFO_MMAP); }
static int cmd_pci(int argc, char** argv) { (void)argc; (void)argv; return show_kinfo(KINFO_PCI); }

// net: what the network is doing. net dhcp: get an address from DHCP
// (again). net static ADDR/BITS [GATEWAY|none] [DNS]: set one by hand.
static int net_static(int argc, char** argv) {
    netconf_t n;
    memset(&n, 0, sizeof n);
    char addr[20];
    const char* slash = strchr(argv[2], '/');
    uint32_t bits;
    if (!slash || slash - argv[2] >= (int)sizeof addr || parse_uint(slash + 1, &bits) || bits < 1 || bits > 30)
        return usage("net");
    memcpy(addr, argv[2], (size_t)(slash - argv[2]));
    addr[slash - argv[2]] = 0;
    n.mask = 0xFFFFFFFFu << (32 - bits);
    int bad = parse_ipv4(addr, &n.ip);
    if (argc > 3 && strcmp(argv[3], "none")) bad |= parse_ipv4(argv[3], &n.gw);
    if (argc > 4) bad |= parse_ipv4(argv[4], &n.dns[0]);
    if (bad || argc > 5) return usage("net");
    int r = sys_netconf(NETCONF_STATIC, &n);
    if (r == -EINVAL) {
        complain("net: those addresses do not fit together");
        return 1;
    }
    if (r < 0) {
        err("net", r);
        return 1;
    }
    return 0;
}

static int net_dhcp(void) {
    int r = sys_netconf(NETCONF_DHCP, 0);
    if (r < 0) {
        err("net", r);
        return 1;
    }
    print("Asking DHCP... ");
    // Up to 10 seconds; a key pressed stops the wait, not the asking.
    uint32_t start = sys_ticks();
    while (sys_ticks() - start < 1000) {
        netconf_t n;
        if (sys_netconf(NETCONF_GET, &n) == 0 && n.ip) {
            printf(IPV4_FMT "/%d", IPV4_ARGS(n.ip), mask_bits(n.mask));
            if (n.gw) printf(", gateway " IPV4_FMT, IPV4_ARGS(n.gw));
            printf(" from " IPV4_FMT "\n", IPV4_ARGS(n.server));
            return 0;
        }
        if (sys_poll(10) == 1) break;
    }
    puts("no answer yet; it keeps asking (see `net`)");
    return 0;
}

static int cmd_net(int argc, char** argv) {
    if (argc == 1) return show_kinfo(KINFO_NET);
    if (!strcmp(argv[1], "dhcp") && argc == 2) return net_dhcp();
    if (!strcmp(argv[1], "static") && argc >= 3) return net_static(argc, argv);
    return usage("net");
}

static int cmd_history(int argc, char** argv) {
    (void)argc; (void)argv;
    int oldest = hist_count > HISTORY ? hist_count - HISTORY : 0;
    for (int i = oldest; i < hist_count; i++) printf("%3d  %s\n", i + 1, history[i % HISTORY]);
    return 0;
}

// write FILE TEXT...: the words, a blank between each, and a newline.
static int cmd_write(int argc, char** argv) {
    if (argc < 3) return usage("write");
    size_t len = 1;
    for (int i = 2; i < argc; i++) len += strlen(argv[i]) + 1;
    char* buf = malloc(len);
    if (!buf) {
        err("write", -ENOMEM);
        return 1;
    }
    size_t n = 0;
    for (int i = 2; i < argc; i++) {
        size_t k = strlen(argv[i]);
        memcpy(buf + n, argv[i], k);
        n += k;
        buf[n++] = i + 1 < argc ? ' ' : '\n';
    }
    int r = sys_save(argv[1], buf, (uint32_t)n);
    free(buf);
    if (r < 0) {
        err(argv[1], r);
        return 1;
    }
    return 0;
}

static const struct { const char* name; uint8_t color; } palette[] = {
    { "blue", 0x01 }, { "green", 0x02 }, { "cyan", 0x03 }, { "red", 0x04 },
    { "magenta", 0x05 }, { "yellow", 0x0E }, { "gray", 0x07 }, { "lightgreen", 0x0A },
    { "lightred", 0x0C }, { "lightcyan", 0x0B }, { "white", 0x0F },
};

static int cmd_color(int argc, char** argv) {
    if (argc != 2) return usage("color");
    for (uint32_t i = 0; i < ARRAY_SIZE(palette); i++) {
        if (strcmp(argv[1], palette[i].name) == 0) {
            sys_set_color(palette[i].color);
            print(NORM);
            return 0;
        }
    }
    dprintf(STDERR_FILENO, RED "Unknown color." NORM " Try one of:");
    for (uint32_t i = 0; i < ARRAY_SIZE(palette); i++) dprintf(STDERR_FILENO, " %s", palette[i].name);
    dprintf(STDERR_FILENO, "\n");
    return 1;
}

static int cmd_uptime(int argc, char** argv) {
    (void)argc; (void)argv;
    uint32_t t = sys_ticks();
    printf("Up %u.%02u seconds (%u ticks at 100 Hz)\n", t / 100, t % 100, t);
    return 0;
}

static int cmd_date(int argc, char** argv) {
    (void)argc; (void)argv;
    rtc_time_t t;
    int r = sys_time(&t);
    if (r < 0) {
        err("date", r);
        return 1;
    }
    printf("%04u-%02u-%02u %02u:%02u:%02u\n", t.year, t.month, t.day, t.hour, t.min, t.sec);
    return 0;
}

static int cmd_kill(int argc, char** argv) {
    if (argc == 2 && !strcmp(argv[1], "-l")) {
        for (int i = 1; i < NSIG; i++) printf("%2d %-5s %s\n", i, signame(i), sigdescr(i));
        return 0;
    }
    int sig = SIGTERM, a = 1;
    if (argc == 3 && argv[1][0] == '-') {
        sig = signum(argv[1] + 1);
        if (sig < 0) {
            complain("Unknown signal. kill -l lists them.");
            return 1;
        }
        a = 2;
    }
    uint32_t id;
    if (argc != a + 1 || parse_uint(argv[a], &id)) return usage("kill");
    if ((int)id == sys_getpid()) {
        complain("The shell will not kill itself");
        return 1;
    }
    int r = sys_kill((int)id, sig);
    if (r < 0) {
        err("kill", r);
        return 1;
    }
    if (sig) printf("Sent SIG%s to task %u\n", signame(sig), id);
    else printf("Task %u is there\n", id);
    return 0;
}

static int cmd_reboot(int argc, char** argv) { (void)argc; (void)argv; sys_power(POWER_REBOOT); return 1; }
static int cmd_poweroff(int argc, char** argv) { (void)argc; (void)argv; sys_power(POWER_OFF); return 1; }

// SHB_PURE: it only writes, so it may run inside the shell at the head
// of a pipeline (history | grep sees this shell's history). net waits
// on the keyboard, and the power ones are better off alone.
static const sh_builtin_t builtins[] = {
    { "help", SHB_PURE, cmd_help, "[command]", "this list, or what one command does" },
    { "clear", SHB_PURE, cmd_clear, "", "clear the screen (also Ctrl+L)" },
    { "version", SHB_PURE, cmd_version, "", "show the version" },
    { "history", SHB_PURE, cmd_history, "", "recent lines (Up/Down recall them)" },
    { "write", SHB_PURE, cmd_write, "<file> <text>", "replace a file's contents with a line" },
    { "tasks", SHB_PURE, cmd_tasks, "", "list tasks" },
    { "kill", SHB_PURE, cmd_kill, "[-SIG] <id>", "send a task a signal (TERM); -l lists them" },
    { "cpus", SHB_PURE, cmd_cpus, "", "the processors and what each one runs" },
    { "faults", SHB_PURE, cmd_faults, "", "recent faults" },
    { "mem", SHB_PURE, cmd_mem, "", "memory summary" },
    { "heap", SHB_PURE, cmd_heap, "", "kernel heap details" },
    { "mmap", SHB_PURE, cmd_mmap, "", "boot memory map" },
    { "pci", SHB_PURE, cmd_pci, "", "PCI devices" },
    { "net", 0, cmd_net, "[dhcp|static ...]", "network card, address and traffic; set the address" },
    { "uptime", SHB_PURE, cmd_uptime, "", "time since boot" },
    { "date", SHB_PURE, cmd_date, "", "the date and time" },
    { "color", SHB_PURE, cmd_color, "<name>", "set the text color" },
    { "reboot", 0, cmd_reboot, "", "restart the machine" },
    { "poweroff", 0, cmd_poweroff, "", "turn the machine off" },
};

static const sh_builtin_t* find_builtin(const char* name) {
    for (uint32_t i = 0; i < ARRAY_SIZE(builtins); i++)
        if (!strcmp(builtins[i].name, name)) return &builtins[i];
    int n;
    const sh_builtin_t* core = sh_core_builtins(&n);
    for (int i = 0; i < n; i++)
        if (!strcmp(core[i].name, name)) return &core[i];
    return 0;
}

static int usage(const char* name) {
    const sh_builtin_t* b = find_builtin(name);
    dprintf(STDERR_FILENO, RED "Usage: %s %s" NORM "\n", name, b ? b->args : "");
    return 2;
}

static void help_line(const sh_builtin_t* b) {
    char left[40];
    snprintf(left, sizeof left, "%s %s", b->name, b->args);
    printf("  %-26s %s\n", left, b->help);
}

// help: the console's commands one a line, the shell's by name only
// (there are many and they are the usual ones); help NAME for one.
static int cmd_help(int argc, char** argv) {
    if (argc > 1) {
        int st = 0;
        for (int i = 1; i < argc; i++) {
            const sh_builtin_t* b = find_builtin(argv[i]);
            if (b) help_line(b);
            else {
                dprintf(STDERR_FILENO, RED "help: no command '%s'; programs come from /" NORM "\n", argv[i]);
                st = 1;
            }
        }
        return st;
    }
    for (uint32_t i = 0; i < ARRAY_SIZE(builtins); i++) help_line(&builtins[i]);
    int n;
    const sh_builtin_t* core = sh_core_builtins(&n);
    print("Shell:");
    int col = 6;
    for (int i = 0; i < n; i++) {
        int w = (int)strlen(core[i].name) + 1;
        if (col + w > 78) {
            print("\n      ");
            col = 6;
        }
        printf(" %s", core[i].name);
        col += w;
    }
    print("\n");
    if (sys_fdtype(STDOUT_FILENO) != FDTYPE_TTY) return 0;
    print(DIM "Paths: DOCS/A.TXT, /ROOT.TXT, .. for the parent. Programs come from /.\n"
              "a | b pipes; < > >> 2> redirect; a && b, a || b and a; b chain; a & runs alone.\n"
              "if, while, for, case, f() { ... }, $VAR, $(cmd) and $((1+2)) work as in sh.\n"
              "sh FILE runs a script. Ctrl+C stops what runs; Ctrl+\\ one that ignores it.\n" NORM);
    return 0;
}

// ---- Tab completion ----
//
// Tab completes the word at the end of the line (the editor has no
// cursor in the middle). The first word of a command, and the one after
// `run`, is a command: a built-in or a program in the root. Any other
// word is a path, completed against the directory it names. Matching
// ignores case, like the disk does, and a completed name takes the
// disk's case. With several matches Tab extends the word as far as they
// agree; when it cannot go further it lists them. A name with a blank or
// another special character is wrapped in quotes as it is inserted.

#define MAX_CAND 128

static struct { char name[NAME_MAX + 1]; int dir; } cand[MAX_CAND];
static int ncand, cand_more;  // cand_more: there were more matches than fit

static int prefix_ci(const char* name, const char* prefix, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char a = name[i], b = prefix[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b || !a) return 0;
    }
    return 1;
}

static void cand_add(const char* name, int dir, int lower) {
    for (int i = 0; i < ncand; i++)
        if (strcasecmp(cand[i].name, name) == 0) return;
    if (ncand == MAX_CAND) { cand_more = 1; return; }
    char* s = cand[ncand].name;
    strlcpy(s, name, sizeof cand[ncand].name);
    cand[ncand].dir = dir;
    // Programs sit on the disk in upper case but are typed in lower case.
    for (char* p = s; lower && *p; p++)
        if (*p >= 'a' && *p <= 'z') lower = 0;
    for (char* p = s; lower && *p; p++)
        if (*p >= 'A' && *p <= 'Z') *p += 32;
    ncand++;
}

// What the typed word can be completed to.
enum { CTX_COMMAND, CTX_PATH, CTX_DIR };

static void gather(int ctx, const char* dirpart, const char* prefix) {
    size_t plen = strlen(prefix);
    int cmd = ctx == CTX_COMMAND && !*dirpart;
    ncand = cand_more = 0;
    if (cmd) {
        for (uint32_t i = 0; i < ARRAY_SIZE(builtins); i++)
            if (prefix_ci(builtins[i].name, prefix, plen)) cand_add(builtins[i].name, 0, 0);
        int n;
        const sh_builtin_t* core = sh_core_builtins(&n);
        for (int i = 0; i < n; i++)  // not : . [ which nobody completes
            if (core[i].name[0] >= 'a' && prefix_ci(core[i].name, prefix, plen)) cand_add(core[i].name, 0, 0);
    }
    int cursor = 0;
    dirent_t d;
    const char* where = cmd ? "/" : *dirpart ? dirpart : ".";
    while (sys_readdir(where, &cursor, &d) > 0) {
        int dir = d.flags & DIRENT_DIR ? 1 : 0;
        if (!prefix_ci(d.name, prefix, plen)) continue;
        if (cmd) {
            if (!dir && !strchr(d.name, '.')) cand_add(d.name, 0, 1);  // programs have no extension
        } else if (ctx != CTX_DIR || dir) {
            cand_add(d.name, dir, 0);
        }
    }
}

// Print the candidates in columns, then the prompt and line again.
static void list_cands(const char* buf, int n) {
    int order[MAX_CAND], width = 0;
    for (int i = 0; i < ncand; i++) {
        int j = i;
        while (j > 0 && strcasecmp(cand[order[j - 1]].name, cand[i].name) > 0) { order[j] = order[j - 1]; j--; }
        order[j] = i;
        int w = (int)strlen(cand[i].name) + cand[i].dir;
        if (w > width) width = w;
    }
    width += 2;
    int cols = 78 / width;
    if (cols < 1) cols = 1;
    print("\n");
    for (int i = 0; i < ncand; i++) {
        const char* name = cand[order[i]].name;
        int dir = cand[order[i]].dir;
        int last = (i + 1) % cols == 0 || i + 1 == ncand;
        printf("%s%s%*s", name, dir ? "/" : "", last ? 0 : width - (int)strlen(name) - dir, "");
        if (last) print("\n");
    }
    if (cand_more) print(DIM "(more matches than shown)\n" NORM);
    prompt();
    out(buf, n);
}

// Completes the word at the end of buf (n bytes) in place. Returns the
// new length.
static int complete(char* buf, int n) {
    // Walk the line the way split() does, to learn where the last word
    // starts and what kind of word it is.
    int wstart = n, inword = 0, argidx = 0, redir = 0;
    char q = 0, first[16] = "";
    for (int i = 0; i < n; i++) {
        char c = buf[i];
        int special = !q && strchr("|<>&;(", c) != 0;
        if (!q && (c == ' ' || c == '\t' || special)) {
            if (inword) {  // a word ended
                if (redir) redir = 0;
                else if (argidx++ == 0) {
                    int k = 0;
                    for (int j = wstart; j < i && k < (int)sizeof first - 1; j++)
                        if (buf[j] != '"' && buf[j] != '\'') first[k++] = buf[j];
                    first[k] = 0;
                }
                inword = 0;
            }
            if (special) {
                if (c == '<' || c == '>') redir = 1;
                else { argidx = 0; first[0] = 0; redir = 0; }  // | & ; ( start a command
            }
            continue;
        }
        if (!inword) { inword = 1; wstart = i; }
        if (q) { if (c == q) q = 0; }
        else if (c == '"' || c == '\'') q = c;
    }
    if (!inword) wstart = n;
    int ctx = redir ? CTX_PATH
            : argidx == 0 || (argidx == 1 && strcmp(first, "run") == 0) ? CTX_COMMAND
            : strcmp(first, "cd") == 0 || strcmp(first, "rmdir") == 0 ? CTX_DIR
            : CTX_PATH;

    // The word without its quotes, split after its last slash.
    char word[LINE_MAX];
    int wl = 0;
    char qq = 0;
    for (int i = wstart; i < n; i++) {
        if (qq) { if (buf[i] == qq) qq = 0; else word[wl++] = buf[i]; }
        else if (buf[i] == '"' || buf[i] == '\'') qq = buf[i];
        else word[wl++] = buf[i];
    }
    word[wl] = 0;
    char* slash = strrchr(word, '/');
    size_t dl = slash ? (size_t)(slash - word) + 1 : 0;
    char dirpart[LINE_MAX];
    memcpy(dirpart, word, dl);
    dirpart[dl] = 0;
    const char* prefix = word + dl;
    size_t plen = strlen(prefix);

    gather(ctx, dirpart, prefix);
    if (!ncand) return n;

    // How far the matches agree.
    size_t cl = strlen(cand[0].name);
    for (int i = 1; i < ncand; i++) {
        size_t k = 0;
        while (k < cl && prefix_ci(cand[i].name + k, cand[0].name + k, 1)) k++;
        cl = k;
    }
    if (ncand > 1 && cl <= plen) { list_cands(buf, n); return n; }

    // The new word: the directory part as typed, then the matched name.
    int unique = ncand == 1;
    char neww[LINE_MAX];
    int nl = (int)dl;
    int take = (int)(unique ? strlen(cand[0].name) : cl);
    if (nl + take >= (int)sizeof neww - 1) return n;
    memcpy(neww, dirpart, dl);
    memcpy(neww + nl, cand[0].name, (size_t)take);
    nl += take;
    neww[nl] = 0;
    int dir = unique && cand[0].dir;
    if (dir) { neww[nl++] = '/'; neww[nl] = 0; }

    int needq = q != 0;  // still inside a quote the user opened
    for (int i = 0; i < nl; i++)
        if (strchr(" \t|<>&;()$`'\"", neww[i])) needq = 1;
    char qc = strchr(neww, '"') ? '\'' : '"';
    char ow[LINE_MAX + 4];
    int ol = 0;
    if (needq) ow[ol++] = qc;
    memcpy(ow + ol, neww, (size_t)nl);
    ol += nl;
    if (unique && !dir) {  // a finished name: close the quote, leave a space
        if (needq) ow[ol++] = qc;
        ow[ol++] = ' ';
    }
    if (wstart + ol >= LINE_MAX) return n;

    erase(n - wstart);
    memcpy(buf + wstart, ow, (size_t)ol);
    out(buf + wstart, ol);
    return wstart + ol;
}


// ---- What the core shows through the console ----

// A command line ended with a status other than 0.
static void report_status(int status) { printf(DIM "[exit status %d]" NORM "\n", status); }

// Ctrl+C came while commands ran (got_intr), and something said so.
static volatile int got_intr;
static int said_intr;

// A job in the foreground ended with signal sig.
static void report_signal(int sig) {
    said_intr |= sig == SIGINT;
    if (sig == SIGINT) print("^C\n");
    else if (sig == SIGQUIT) print("^\\\n");
    else if (sig == SIGSEGV || sig == SIGILL || sig == SIGFPE || sig == SIGBUS || sig == SIGTRAP) return;  // the kernel said what happened
    else if (sig == SIGPIPE) return;  // the reader went first, which is no news (bash says nothing either)
    else printf(DIM "[%s]" NORM "\n", sigdescr(sig));
}

// "x: not found" and the like, a line ending in '\n'.
static void message(const char* s) {
    char buf[640];
    size_t n = strlcpy(buf, s, sizeof buf);
    if (n >= sizeof buf) n = sizeof buf - 1;
    if (n && buf[n - 1] == '\n') buf[n - 1] = 0;
    dprintf(STDERR_FILENO, RED "%s" NORM "\n", buf);
}

static const sh_front_t front = {
    builtins, (int)ARRAY_SIZE(builtins), report_status, report_signal, message,
};

// The memory a session needs, taken at the start: a heap that will not
// have to grow for an ordinary command, and stack pages touched as deep
// as a command goes. A shell waiting at the prompt then holds the same
// memory from one command to the next, so `mem` before and after
// something shows only what that something kept.
static void __attribute__((noinline)) touch_stack(void) {
    volatile char deep[32 * 1024];
    for (size_t i = 0; i < sizeof deep; i += 4096) deep[i] = 0;
}

static void on_interrupt(int sig) {
    (void)sig;
    sh_interrupted = 1;
    got_intr = 1;
}

int main(int argc, char** argv) {
    int st = sh_main(&front, argc, argv);
    if (st != SH_INTERACTIVE) return st;

    // Ctrl+\ is for the program in the foreground, not the shell; and a
    // built-in writing into a pipeline whose reader has gone should see
    // EPIPE, not end the shell. Ctrl+C stops what the shell itself runs.
    signal(SIGQUIT, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, on_interrupt);
    free(malloc(128 * 1024));
    touch_stack();
    print("Type 'help' for commands.\n");

    // A command can take several lines (an if, a quote, a here-document):
    // they gather in text, and the core says when it has all of it.
    char line[LINE_MAX];
    char* text = 0;
    size_t len = 0, cap = 0;
    for (;;) {
        more = len > 0;
        prompt();
        int n = read_line(line);
        if (n == -2) return 1;  // no terminal: nothing more to do
        if (n == -1) {          // Ctrl+C drops the command, however many lines it had
            if (len) sh_feed(0);
            len = 0;
            continue;
        }
        if (n == 0 && !len) continue;
        if (n > 0) history_add(line);
        if (len + (size_t)n + 2 > cap) {
            size_t want = cap ? cap * 2 : 512;
            while (want < len + (size_t)n + 2) want *= 2;
            char* t = realloc(text, want);
            if (!t) {
                complain("Out of memory; the command is dropped");
                sh_feed(0);
                len = 0;
                continue;
            }
            text = t;
            cap = want;
        }
        memcpy(text + len, line, (size_t)n);
        len += (size_t)n;
        text[len++] = '\n';
        text[len] = 0;
        sys_tty_mode(0);  // the usual terminal while commands run: Ctrl+C is a signal
        got_intr = said_intr = 0;
        if (sh_feed(text) == SH_DONE) len = 0;
        if (got_intr && !said_intr) print("^C\n");  // a loop of the shell's own was stopped
        if (sh_exit_asked(&st)) return st;
    }
}
