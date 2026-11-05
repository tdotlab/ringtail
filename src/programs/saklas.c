// Saklas, the shell: an ordinary program. init starts it at boot and starts
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
static void prompt(void) {
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

// "2026-11-05 14:32" from the FAT words in a dirent, or dashes for an
// entry nothing ever stamped.
static void fmt_stamp(char* buf, size_t size, unsigned date, unsigned time) {
    if (!date) { strlcpy(buf, "-", size); return; }
    snprintf(buf, size, "%04u-%02u-%02u %02u:%02u", 1980 + (date >> 9), (date >> 5) & 15, date & 31,
             time >> 11, (time >> 5) & 63);
}

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
        char stamp[24];
        fmt_stamp(stamp, sizeof stamp, d.mdate, d.mtime);
        if (d.flags & DIRENT_DIR) {
            char name[sizeof d.name + 1];
            snprintf(name, sizeof name, "%s/", d.name);
            printf("  %s%-*s%s %8s  %s\n", tty_out ? BLU : "", width, name, tty_out ? NORM : "", "<dir>", stamp);
            dirs++;
        } else {
            printf("  %-*s %8u  %s\n", width, d.name, d.size, stamp);
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

static void cmd_date(cmd_t* c) {
    (void)c;
    rtc_time_t t;
    int r = sys_time(&t);
    if (r < 0) { err("date", r); return; }
    printf("%04u-%02u-%02u %02u:%02u:%02u\n", t.year, t.month, t.day, t.hour, t.min, t.sec);
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
    { "date", 0, "", "the date and time", cmd_date },
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
    if (cmd)
        for (uint32_t i = 0; i < ARRAY_SIZE(builtins); i++)
            if (prefix_ci(builtins[i].name, prefix, plen)) cand_add(builtins[i].name, 0, 0);
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
        int special = !q && strchr("|<>&", c) != 0;
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
                if (c == '|' || c == '&') { argidx = 0; first[0] = 0; redir = 0; }
                else redir = 1;
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
        if (strchr(" \t|<>&'\"", neww[i])) needq = 1;
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
