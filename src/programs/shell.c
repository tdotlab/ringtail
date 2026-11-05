// The shell: an ordinary program. init starts it at boot and starts
// a new one whenever it ends.
//
// It edits the line itself in raw mode, with Ctrl+C as a plain key,
// so Ctrl+C at the prompt cancels the line instead of killing the
// shell. A program it runs gets a job of its own and the terminal,
// and the shell takes the terminal back once the whole job is gone.
//
// A command line is words separated by blanks. Quotes ("..." or
// '...') keep blanks and special characters inside a word. The
// built-in commands come first; any other word names a program on
// the disk, and `run` says so explicitly. A bare program name is
// looked up in the root, a path (./PROG, BIN/PROG) as written.
//
// `a | b | c` connects programs with pipes, and all of them share one
// job, so Ctrl+C stops the lot. `< FILE` feeds the first command,
// `> FILE` and `>> FILE` take the last one's output. A built-in can
// start a pipeline or have its output redirected: the shell points
// its own standard output at the pipe or file while the built-in runs.
#include "ulib.h"

#define LINE_MAX 256
#define HISTORY 10
#define MAX_WORDS 24

#define RED  "\x1b[91m"
#define DIM  "\x1b[90m"
#define GRN  "\x1b[92m"
#define BLU  "\x1b[94m"
#define NORM "\x1b[0m"

static char history[HISTORY][LINE_MAX];
static int hist_count;  // total lines ever added; the ring holds the last HISTORY

// Whether the last name in path follows the disk's rules (abi.h):
// printable, none of \" * : < > ? \\ |, no space first or space or
// dot last. "." and ".." count as fine.
static int name_ok(const char* path) {
    const char* n = strrchr(path, '/');
    n = n ? n + 1 : path;
    size_t len = strlen(n);
    if (!len || strcmp(n, ".") == 0 || strcmp(n, "..") == 0) return 1;
    if (len > NAME_MAX || n[0] == ' ' || n[len - 1] == ' ' || n[len - 1] == '.') return 0;
    for (const char* p = n; *p; p++)
        if (*p < 0x20 || *p > 0x7E || strchr("\"*:<>?\\|", *p)) return 0;
    return 1;
}

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

enum { K_UP = 0x100, K_DOWN, K_OTHER };

// One key: a byte, or an arrow decoded from its escape sequence.
// Other sequences, and Alt+key (ESC then the key), are swallowed.
static int getkey(void) {
    int c = getbyte();
    if (c != 0x1B) return c;
    c = getbyte();
    if (c != '[' && c != 'O') return c < 0 ? -1 : K_OTHER;
    int final;
    do final = getbyte(); while (final >= 0 && !(final >= 0x40 && final <= 0x7E));
    if (final == 'A') return K_UP;
    if (final == 'B') return K_DOWN;
    return final < 0 ? -1 : K_OTHER;
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
static void prompt(void) {
    char cwd[PATH_MAX];
    if (sys_getcwd(cwd, sizeof cwd) < 0) strlcpy(cwd, "?", sizeof cwd);
    printf(GRN "ringtail:%s> " NORM, cwd);
}

// Returns the line length, -1 if the line was cancelled, -2 when the
// terminal is gone.
static int read_line(char* buf) {
    sys_tty_mode(TTY_RAW | TTY_NOSIG);
    int n = 0;
    int browse = hist_count;  // history entry shown; hist_count is the line being typed
    for (;;) {
        int c = getkey();
        if (c < 0) return -2;
        if (c == '\n' || c == '\r') {
            print("\n");
            buf[n] = 0;
            return n;
        }
        if (c == 0x03) {  // Ctrl+C
            print("^C\n");
            return -1;
        }
        if (c == 0x0C) {  // Ctrl+L
            print("\x1b[2J\x1b[H");
            prompt();
            out(buf, n);
            continue;
        }
        if (c == 0x7F || c == '\b') {
            if (n > 0) { n--; erase(1); }
            continue;
        }
        if (c == 0x15) {  // Ctrl+U: the whole line
            erase(n);
            n = 0;
            continue;
        }
        if (c == 0x17) {  // Ctrl+W, Ctrl+Backspace: the word before the cursor
            int k = n;
            while (k > 0 && buf[k - 1] == ' ') k--;
            while (k > 0 && buf[k - 1] != ' ') k--;
            erase(n - k);
            n = k;
            continue;
        }
        if (c == K_UP || c == K_DOWN) {
            int oldest = hist_count > HISTORY ? hist_count - HISTORY : 0;
            int next = browse + (c == K_UP ? -1 : 1);
            if (next < oldest || next > hist_count) continue;
            browse = next;
            erase(n);
            if (browse == hist_count) n = 0;
            else n = (int)strlcpy(buf, history[browse % HISTORY], LINE_MAX);
            out(buf, n);
            continue;
        }
        if (c >= 0x20 && c < 0x7F && n < LINE_MAX - 1) {
            buf[n++] = (char)c;
            out(&buf[n - 1], 1);
        }
    }
}

static void history_add(const char* line) {
    if (hist_count && strcmp(history[(hist_count - 1) % HISTORY], line) == 0) return;
    strlcpy(history[hist_count % HISTORY], line, LINE_MAX);
    hist_count++;
}

// ---- Parsing ----

// A word, or one of the operators | < > >> &. start and end are where
// it was in the line, so a command can get its arguments back with the
// spacing they were typed with.
enum { W_WORD, W_PIPE, W_IN, W_OUT, W_APPEND, W_BG };

typedef struct {
    int kind;
    char* text;  // the word without its quotes
    int start, end;
    int spaced;  // set on a word whose typed predecessor was removed (a redirection)
} word_t;

static char wordbuf[LINE_MAX * 2];

// Returns the number of words, or -1 with an error printed.
static int split(const char* line, word_t* w, int max) {
    int n = 0;
    char* o = wordbuf;
    int i = 0;
    for (;;) {
        while (line[i] == ' ' || line[i] == '\t') i++;
        if (!line[i]) return n;
        if (n == max) { complain("Too many words on the line"); return -1; }
        w[n].start = i;
        w[n].spaced = 0;
        char c = line[i];
        if (c == '|' || c == '<' || c == '>' || c == '&') {
            w[n].kind = c == '|' ? W_PIPE : c == '<' ? W_IN : c == '&' ? W_BG : W_OUT;
            i++;
            if (c == '>' && line[i] == '>') { w[n].kind = W_APPEND; i++; }
            w[n].text = "";
        } else {
            w[n].kind = W_WORD;
            w[n].text = o;
            while (line[i] && line[i] != ' ' && line[i] != '\t' && !strchr("|<>&", line[i])) {
                if (line[i] == '"' || line[i] == '\'') {
                    char q = line[i++];
                    while (line[i] && line[i] != q) *o++ = line[i++];
                    if (!line[i]) { complain("A quote is not closed"); return -1; }
                    i++;
                } else {
                    *o++ = line[i++];
                }
            }
            *o++ = 0;
        }
        w[n].end = i;
        n++;
    }
}

// ---- Built-in commands ----

typedef struct {
    const char* line;   // the whole line, for args_text
    word_t* w;          // the command's words: w[0] is its name
    int argc;
    char* argv[MAX_WORDS + 1];
} cmd_t;

// Arguments from..argc-1 as one string, with the blanks between them
// as typed (echo and write keep "a   b" as it is).
static void args_text(const cmd_t* c, int from, char* buf, size_t size) {
    size_t n = 0;
    for (int i = from; i < c->argc; i++) {
        if (i > from && c->w[i].spaced) {
            if (n + 1 < size) buf[n++] = ' ';
        } else if (i > from) {
            for (int k = c->w[i - 1].end; k < c->w[i].start && n + 1 < size; k++) buf[n++] = c->line[k];
        }
        for (const char* s = c->argv[i]; *s && n + 1 < size; s++) buf[n++] = *s;
    }
    buf[n] = 0;
}

// 0 while a built-in's output goes to a file or a pipe: no colours
// then, and nothing meant only for a person reading the screen.
static int tty_out = 1;

static void show_kinfo(int what) {
    static char buf[4096];
    int r = sys_kinfo(what, buf, sizeof buf);
    if (r < 0) { err("kinfo", r); return; }
    print(buf);
}

static void cmd_help(cmd_t* c);

static void cmd_clear(cmd_t* c) { (void)c; print("\x1b[2J\x1b[H"); }
static void cmd_version(cmd_t* c) { (void)c; show_kinfo(KINFO_VERSION); }
static void cmd_tasks(cmd_t* c) { (void)c; show_kinfo(KINFO_TASKS); }
static void cmd_faults(cmd_t* c) { (void)c; show_kinfo(KINFO_FAULTS); }
static void cmd_mem(cmd_t* c) { (void)c; show_kinfo(KINFO_MEM); }
static void cmd_heap(cmd_t* c) { (void)c; show_kinfo(KINFO_HEAP); }
static void cmd_mmap(cmd_t* c) { (void)c; show_kinfo(KINFO_MMAP); }

static void cmd_echo(cmd_t* c) {
    char buf[LINE_MAX];
    args_text(c, 1, buf, sizeof buf);
    puts(buf);
}

static void cmd_history(cmd_t* c) {
    (void)c;
    int oldest = hist_count > HISTORY ? hist_count - HISTORY : 0;
    for (int i = oldest; i < hist_count; i++) printf("%3d  %s\n", i + 1, history[i % HISTORY]);
}

// The name column fits the longest name up to LS_WIDE; a longer one
// pushes its size along rather than widening every line.
#define LS_NARROW 13
#define LS_WIDE 32

static void cmd_ls(cmd_t* c) {
    const char* dir = c->argc > 1 ? c->argv[1] : ".";
    int cursor = 0, files = 0, dirs = 0, width = LS_NARROW, r;
    dirent_t d;
    while ((r = sys_readdir(dir, &cursor, &d)) > 0) {
        int n = (int)strlen(d.name) + ((d.flags & DIRENT_DIR) ? 1 : 0);
        if (n > width) width = n < LS_WIDE ? n : LS_WIDE;
    }
    cursor = 0;
    while (r >= 0 && (r = sys_readdir(dir, &cursor, &d)) > 0) {
        if (d.flags & DIRENT_DIR) {
            char name[sizeof d.name + 1];
            snprintf(name, sizeof name, "%s/", d.name);
            printf("  %s%-*s%s %8s\n", tty_out ? BLU : "", width, name, tty_out ? NORM : "", "<dir>");
            dirs++;
        } else {
            printf("  %-*s %8u\n", width, d.name, d.size);
            files++;
        }
    }
    if (r < 0) { err(dir, r); return; }
    if (!tty_out) return;  // one line per entry, for wc and grep
    char disk[64];
    if (sys_kinfo(KINFO_DISK, disk, sizeof disk) < 0) { err("ls", -EIO); return; }
    printf(DIM "%d file%s, ", files, files == 1 ? "" : "s");
    if (dirs) printf("%d director%s, ", dirs, dirs == 1 ? "y" : "ies");
    printf("%s" NORM, disk);
}

static void cmd_cd(cmd_t* c) {
    const char* dir = c->argc > 1 ? c->argv[1] : "/";
    int r = sys_chdir(dir);
    if (r < 0) err(dir, r);
}

static void cmd_pwd(cmd_t* c) {
    (void)c;
    char cwd[PATH_MAX];
    int r = sys_getcwd(cwd, sizeof cwd);
    if (r < 0) err("pwd", r);
    else puts(cwd);
}

static void cmd_mkdir(cmd_t* c) {
    int r = sys_mkdir(c->argv[1]);
    if (r < 0) err(c->argv[1], r);
}

static void cmd_rmdir(cmd_t* c) {
    int r = sys_rmdir(c->argv[1]);
    if (r == -EBUSY) dprintf(STDERR_FILENO, RED "%s: in use (someone's current directory)" NORM "\n", c->argv[1]);
    else if (r < 0) err(c->argv[1], r);
}

static void cmd_cat(cmd_t* c) {
    int fd = sys_open(c->argv[1], O_RDONLY);
    if (fd < 0) { err(c->argv[1], fd); return; }
    char buf[512];
    int n, last = '\n';
    while ((n = sys_read(fd, buf, sizeof buf)) > 0) {
        if (sys_write(STDOUT_FILENO, buf, (uint32_t)n) < 0) break;
        last = buf[n - 1];
    }
    if (n < 0) err(c->argv[1], n);
    sys_close(fd);
    if (last != '\n') print("\n");
}

static void cmd_touch(cmd_t* c) {
    int fd = sys_open(c->argv[1], O_WRONLY | O_CREAT | O_EXCL);
    if (fd < 0) err(c->argv[1], fd);
    else sys_close(fd);
}

static void cmd_write(cmd_t* c) {
    char buf[LINE_MAX + 1];
    args_text(c, 2, buf, LINE_MAX);
    uint32_t len = (uint32_t)strlen(buf);
    buf[len++] = '\n';
    int r = sys_save(c->argv[1], buf, len);
    if (r < 0) err(c->argv[1], r);
}

static void cmd_rm(cmd_t* c) {
    int r = sys_delete(c->argv[1]);
    if (r == -EISDIR) dprintf(STDERR_FILENO, RED "%s: is a directory (rmdir removes an empty one)" NORM "\n", c->argv[1]);
    else if (r < 0) err(c->argv[1], r);
}

// mv SRC DST, or mv SRC... DIR.
static void cmd_mv(cmd_t* c) {
    const char* dst = c->argv[c->argc - 1];
    if (c->argc > 3) {
        dirent_t d;
        if (sys_stat(dst, &d) < 0 || !(d.flags & DIRENT_DIR)) {
            complain("mv: moving several things needs a directory last");
            return;
        }
    }
    for (int i = 1; i < c->argc - 1; i++) {
        const char* src = c->argv[i];
        char to[PATH_MAX];
        int r = dest_path(src, dst, to, sizeof to);
        if (!r) r = sys_rename(src, to);
        if (r == -EINVAL && !name_ok(to)) err(to, r);
        else if (r == -EINVAL) dprintf(STDERR_FILENO, RED "%s: cannot go inside itself" NORM "\n", src);
        else if (r == -EBUSY) dprintf(STDERR_FILENO, RED "%s: give it by name to move it" NORM "\n", src);
        else if (r == -EEXIST) dprintf(STDERR_FILENO, RED "%s: already exists (rm it first)" NORM "\n", to);
        else if (r < 0) err(src, r);
    }
}

static const struct { const char* name; uint8_t color; } palette[] = {
    { "blue", 0x01 }, { "green", 0x02 }, { "cyan", 0x03 }, { "red", 0x04 },
    { "magenta", 0x05 }, { "yellow", 0x0E }, { "gray", 0x07 }, { "lightgreen", 0x0A },
    { "lightred", 0x0C }, { "lightcyan", 0x0B }, { "white", 0x0F },
};

static void cmd_color(cmd_t* c) {
    for (uint32_t i = 0; i < ARRAY_SIZE(palette); i++) {
        if (strcmp(c->argv[1], palette[i].name) == 0) {
            sys_set_color(palette[i].color);
            print(NORM);
            return;
        }
    }
    dprintf(STDERR_FILENO, RED "Unknown color." NORM " Try one of:");
    for (uint32_t i = 0; i < ARRAY_SIZE(palette); i++) dprintf(STDERR_FILENO, " %s", palette[i].name);
    dprintf(STDERR_FILENO, "\n");
}

static void cmd_uptime(cmd_t* c) {
    (void)c;
    uint32_t t = sys_ticks();
    printf("Up %u.%02u seconds (%u ticks at 100 Hz)\n", t / 100, t % 100, t);
}

static void cmd_kill(cmd_t* c) {
    uint32_t id;
    if (parse_uint(c->argv[1], &id)) { complain("Usage: kill <task id>"); return; }
    if ((int)id == sys_getpid()) { complain("The shell will not kill itself"); return; }
    int r = sys_kill((int)id);
    if (r < 0) { err("kill", r); return; }
    printf("Killed task %u\n", id);
}

static void cmd_reboot(cmd_t* c) { (void)c; sys_power(POWER_REBOOT); }
static void cmd_poweroff(cmd_t* c) { (void)c; sys_power(POWER_OFF); }
static void cmd_exit(cmd_t* c) { (void)c; sys_exit(0); }

typedef struct {
    const char* name;
    int min_args;  // not counting the command itself
    const char* args;
    const char* help;
    void (*fn)(cmd_t* c);
} builtin_t;

static const builtin_t builtins[] = {
    { "help", 0, "", "this list", cmd_help },
    { "clear", 0, "", "clear the screen (also Ctrl+L)", cmd_clear },
    { "version", 0, "", "show the version", cmd_version },
    { "echo", 0, "<text>", "print text", cmd_echo },
    { "history", 0, "", "recent commands (Up/Down recall them)", cmd_history },
    { "ls", 0, "[dir]", "list a directory", cmd_ls },
    { "cd", 0, "[dir]", "change directory (alone: the root)", cmd_cd },
    { "pwd", 0, "", "show the current directory", cmd_pwd },
    { "mkdir", 1, "<dir>", "make a directory", cmd_mkdir },
    { "rmdir", 1, "<dir>", "remove an empty directory", cmd_rmdir },
    { "cat", 1, "<file>", "show a file", cmd_cat },
    { "touch", 1, "<file>", "create an empty file", cmd_touch },
    { "write", 2, "<file> <text>", "replace a file's contents", cmd_write },
    { "rm", 1, "<file>", "delete a file", cmd_rm },
    { "mv", 2, "<from> <to>", "move or rename (to a directory: into it)", cmd_mv },
    { "run", 1, "<prog> [args] [&]", "run a program (or just type its name)", 0 },
    { "tasks", 0, "", "list tasks", cmd_tasks },
    { "kill", 1, "<id>", "stop a task", cmd_kill },
    { "faults", 0, "", "recent faults", cmd_faults },
    { "mem", 0, "", "memory summary", cmd_mem },
    { "heap", 0, "", "kernel heap details", cmd_heap },
    { "mmap", 0, "", "boot memory map", cmd_mmap },
    { "uptime", 0, "", "time since boot", cmd_uptime },
    { "color", 1, "<name>", "set the text color", cmd_color },
    { "exit", 0, "", "end this shell (init starts a fresh one)", cmd_exit },
    { "reboot", 0, "", "restart the machine", cmd_reboot },
    { "poweroff", 0, "", "turn the machine off", cmd_poweroff },
};

static void cmd_help(cmd_t* c) {
    (void)c;
    for (uint32_t i = 0; i < ARRAY_SIZE(builtins); i++) {
        char left[32];
        snprintf(left, sizeof left, "%s %s", builtins[i].name, builtins[i].args);
        printf("  %-26s %s\n", left, builtins[i].help);
    }
    if (!tty_out) return;
    print(DIM "Paths: DOCS/A.TXT, /ROOT.TXT, .. for the parent. Programs come from /.\n"
              "a | b pipes a's output into b; < FILE, > FILE and >> FILE redirect.\n"
              "Ctrl+C stops the running program; Ctrl+\\ stops one that ignores Ctrl+C.\n" NORM);
}

static const builtin_t* find_builtin(const char* name) {
    for (uint32_t i = 0; i < ARRAY_SIZE(builtins); i++)
        if (strcmp(name, builtins[i].name) == 0) return &builtins[i];
    return 0;
}

// ---- Running commands ----

#define MAX_STAGES 8
#define SAVED_OUT (MAX_FDS - 1)  // the shell's own stdout while a built-in is redirected

typedef struct {
    cmd_t cmd;
    word_t w[MAX_WORDS];
    int first;                 // 1 when the command starts with "run"
    const builtin_t* builtin;  // 0 for a program
} stage_t;

typedef struct {
    stage_t st[MAX_STAGES];
    int n;
    const char* in;   // < FILE, for the first stage
    const char* out;  // > FILE or >> FILE, for the last stage
    int out_stage, append, background;
} pipeline_t;

static pipeline_t pl;

// Split the words into stages and pull out the redirections.
// Returns 0, or -1 with an error printed.
static int parse(const char* line, word_t* w, int n) {
    memset(&pl, 0, sizeof pl);
    pl.n = 1;
    int nw = 0, skipped = 0;
    for (int i = 0; i < n; i++) {
        stage_t* st = &pl.st[pl.n - 1];
        int k = w[i].kind;
        if (k == W_BG) {
            if (i != n - 1) { complain("'&' goes at the end of the line"); return -1; }
            pl.background = 1;
        } else if (k == W_PIPE) {
            if (nw == 0) { complain("Missing command before '|'"); return -1; }
            if (pl.n == MAX_STAGES) { complain("Too many commands in the pipeline"); return -1; }
            st->cmd.argc = nw;
            pl.n++;
            nw = skipped = 0;
        } else if (k != W_WORD) {
            const char* op = k == W_IN ? "<" : k == W_OUT ? ">" : ">>";
            if (i + 1 == n || w[i + 1].kind != W_WORD) {
                dprintf(STDERR_FILENO, RED "Missing file name after '%s'" NORM "\n", op);
                return -1;
            }
            if (k == W_IN) {
                if (pl.n != 1) { complain("Only the first command can read a file with '<'"); return -1; }
                if (pl.in) { complain("Only one '<' per line"); return -1; }
                pl.in = w[i + 1].text;
            } else {
                if (pl.out) { complain("Only one '>' or '>>' per line"); return -1; }
                pl.out = w[i + 1].text;
                pl.append = k == W_APPEND;
                pl.out_stage = pl.n - 1;
            }
            i++;
            skipped = 1;
        } else {
            st->w[nw] = w[i];
            st->w[nw].spaced = skipped && nw > 0;
            st->cmd.argv[nw] = w[i].text;
            nw++;
            skipped = 0;
        }
    }
    pl.st[pl.n - 1].cmd.argc = nw;
    if (nw == 0) {
        if (pl.n > 1) complain("Missing command after '|'");
        else if (pl.in || pl.out) complain("Nothing to run");
        else if (pl.background) complain("Nothing to run in the background");
        return -1;
    }
    if (pl.out && pl.out_stage != pl.n - 1) {
        complain("Only the last command can write a file with '>'");
        return -1;
    }
    for (int s = 0; s < pl.n; s++) {
        stage_t* st = &pl.st[s];
        st->cmd.line = line;
        st->cmd.w = st->w;
        st->cmd.argv[st->cmd.argc] = 0;
        const builtin_t* b = find_builtin(st->cmd.argv[0]);
        if (b && !b->fn) {  // run
            if (st->cmd.argc < 2) { complain("Usage: run <prog> [args] [&]"); return -1; }
            st->first = 1;
        } else if (b) {
            if (s > 0) {
                dprintf(STDERR_FILENO, RED "'%s' is built into the shell, so it can only come first"
                        NORM "\n", b->name);
                return -1;
            }
            if (st->cmd.argc - 1 < b->min_args) {
                dprintf(STDERR_FILENO, RED "Usage: %s %s" NORM "\n", b->name, b->args);
                return -1;
            }
            if (pl.in) { complain("Built-in commands do not read input"); return -1; }
            st->builtin = b;
        }
    }
    if (pl.n == 1 && pl.st[0].builtin && pl.background) {
        complain("Built-in commands cannot run in the background");
        return -1;
    }
    return 0;
}

// Run a built-in with its output on fd (or the terminal when fd < 0).
static void run_builtin(stage_t* st, int fd) {
    if (fd >= 0) {
        sys_dup2(STDOUT_FILENO, SAVED_OUT);
        sys_dup2(fd, STDOUT_FILENO);
        tty_out = 0;
    }
    st->builtin->fn(&st->cmd);
    if (fd >= 0) {
        sys_dup2(SAVED_OUT, STDOUT_FILENO);
        sys_close(SAVED_OUT);
        tty_out = 1;
    }
}

static void spawn_error(const char* name, int e, int explicit_run) {
    if ((e == -ENOENT || e == -EINVAL) && !explicit_run && !strchr(name, '.') && !strchr(name, '/'))
        dprintf(STDERR_FILENO, RED "Unknown command '%s'; try 'help'" NORM "\n", name);
    else
        err(name, e);
}

static void close_fd(int* fd) {
    if (*fd >= 0) sys_close(*fd);
    *fd = -1;
}

static void run(void) {
    int in = -1, out = -1, prev = -1, feed = -1, r;
    if (pl.in && (in = sys_open(pl.in, O_RDONLY)) < 0) { err(pl.in, in); return; }
    if (pl.out) {
        out = sys_open(pl.out, O_WRONLY | O_CREAT | (pl.append ? O_APPEND : O_TRUNC));
        if (out < 0) { err(pl.out, out); close_fd(&in); return; }
    }
    stage_t* b = pl.st[0].builtin ? &pl.st[0] : 0;
    if (b && pl.n == 1) {
        run_builtin(b, out);
        close_fd(&out);
        return;
    }

    // Each program reads what the one before it writes. The shell
    // keeps no pipe end it does not need, or readers would never see
    // the end of their input.
    int ids[MAX_STAGES], nids = 0, lead = 0;
    prev = in;
    in = -1;
    if (b) {
        int p[2];
        if ((r = sys_pipe(p)) < 0) { err("pipe", r); goto fail; }
        prev = p[0];
        feed = p[1];
    }
    for (int s = b ? 1 : 0; s < pl.n; s++) {
        stage_t* st = &pl.st[s];
        int last = s == pl.n - 1;
        int p[2] = { -1, -1 };
        if (!last && (r = sys_pipe(p)) < 0) { err("pipe", r); goto fail; }
        spawn_opts_t o = { lead ? SPAWN_JOIN : SPAWN_NEW_JOB, lead, { prev, last ? out : p[1], -1 } };
        const char* name = st->cmd.argv[st->first];
        int id = sys_spawn_opts(name, (const char* const*)&st->cmd.argv[st->first], &o);
        close_fd(&prev);
        close_fd(&p[1]);
        prev = p[0];
        if (id < 0) { spawn_error(name, id, st->first); goto fail; }
        ids[nids++] = id;
        if (!lead) lead = id;
    }
    close_fd(&out);

    if (pl.background) {
        printf("[%d] started in the background\n", lead);
        if (b) run_builtin(b, feed);
        close_fd(&feed);
        return;
    }
    // The first program leads the job; it has the terminal until
    // every program in it, and everything they started, is gone.
    sys_tty_fg(lead);
    if (b) run_builtin(b, feed);
    close_fd(&feed);
    int status = sys_join(ids[nids - 1]);
    sys_wait_job(lead);
    sys_tty_fg(0);
    if (status == EXIT_KILLED) print("^C\n");
    else if (status != 0 && status != EXIT_FAULTED) printf(DIM "[exit status %d]" NORM "\n", status);
    return;

fail:
    for (int i = 0; i < nids; i++) sys_kill(ids[i]);
    close_fd(&prev);
    close_fd(&feed);
    close_fd(&out);
}

static void execute(const char* line) {
    word_t w[MAX_WORDS];
    int n = split(line, w, MAX_WORDS);
    if (n <= 0 || parse(line, w, n) < 0) return;
    run();
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    print("Type 'help' for commands.\n");
    char line[LINE_MAX];
    for (;;) {
        prompt();
        int n = read_line(line);
        if (n == -2) return 1;  // no terminal: nothing more to do
        if (n <= 0) continue;
        history_add(line);
        execute(line);
    }
}
