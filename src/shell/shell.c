// The shell: a kernel task with a line editor and a command table.
// It reads characters from the terminal (keyboard or serial), so it
// works the same on the screen and over a serial line.
#include "kernel.h"
#include "arch.h"
#include "console.h"
#include "drivers.h"
#include "task.h"
#include "tty.h"
#include "mm.h"
#include "fs.h"
#include "ringtail/abi.h"

#define LINE_MAX 128
#define HISTORY 10
#define MAX_ARGS (ARGV_MAX + 2)
#define PROMPT "ringtail> "

static char history[HISTORY][LINE_MAX];
static int hist_count;  // total lines ever added; the ring holds the last HISTORY

static void err(const char* what, int e) {
    kprintf_color(COLOR_ERROR, "%s: %s\n", what, strerror(e));
}

static void usage(const char* u) {
    kprintf_color(COLOR_ERROR, "Usage: %s\n", u);
}

// ---- Line editor ----

static void erase(int n) {
    while (n-- > 0) console_backspace();
}

// Returns the line length, or -1 if the line was cancelled.
static int read_line(char* buf) {
    int n = 0;
    int browse = hist_count;  // history index being shown; hist_count = the live line
    for (;;) {
        int c = tty_getc();
        if (c < 0) continue;
        if (c == '\n') {
            console_putc('\n');
            buf[n] = 0;
            return n;
        }
        if (c == KEY_CTRL_C) {
            console_puts("^C\n");
            return -1;
        }
        if (c == KEY_CTRL_L) {
            console_clear();
            console_puts_color(PROMPT, COLOR_PROMPT);
            console_write(buf, (size_t)n);
            continue;
        }
        if (c == '\b') {
            if (n > 0) { n--; console_backspace(); }
            continue;
        }
        if (c == KEY_UP || c == KEY_DOWN) {
            int oldest = hist_count > HISTORY ? hist_count - HISTORY : 0;
            int next = browse + (c == KEY_UP ? -1 : 1);
            if (next < oldest || next > hist_count) continue;
            browse = next;
            erase(n);
            if (browse == hist_count) n = 0;
            else n = (int)strlcpy(buf, history[browse % HISTORY], LINE_MAX);
            console_write(buf, (size_t)n);
            continue;
        }
        if (c >= 0x20 && c < 0x7F && n < LINE_MAX - 1) {
            buf[n++] = (char)c;
            console_putc((char)c);
        }
    }
}

static void history_add(const char* line) {
    if (hist_count && strcmp(history[(hist_count - 1) % HISTORY], line) == 0) return;
    strlcpy(history[hist_count % HISTORY], line, LINE_MAX);
    hist_count++;
}

static int split(char* s, char** argv, int max) {
    int argc = 0;
    for (;;) {
        while (*s == ' ' || *s == '\t') *s++ = 0;
        if (!*s) break;
        if (argc == max) return -1;
        argv[argc++] = s;
        while (*s && *s != ' ' && *s != '\t') s++;
    }
    return argc;
}

// ---- Commands ----

typedef struct {
    const char* name;
    int min_args;    // not counting the command itself
    const char* args;
    const char* help;
    void (*fn)(int argc, char** argv, char* rest);
} command_t;

static const command_t commands[];

static void cmd_help(int argc, char** argv, char* rest);

static void cmd_clear(int argc, char** argv, char* rest) {
    (void)argc; (void)argv; (void)rest;
    console_clear();
}

static void cmd_version(int argc, char** argv, char* rest) {
    (void)argc; (void)argv; (void)rest;
    kprintf("RingTail OS v%s \"%s\"\n", RINGTAIL_VERSION, RINGTAIL_CODENAME);
}

static void cmd_echo(int argc, char** argv, char* rest) {
    (void)argc; (void)argv;
    kprintf("%s\n", rest);
}

static void cmd_heap(int argc, char** argv, char* rest) {
    (void)argc; (void)argv; (void)rest;
    heap_stats_t h;
    heap_get_stats(&h);
    kprintf("Heap at %p: %u KB total, %u bytes used, %u free in %u blocks (largest %u)\n",
            (void*)h.start, h.total / 1024, h.used, h.free, h.free_blocks, h.largest_free);
}

static void cmd_mem(int argc, char** argv, char* rest) {
    (void)argc; (void)argv; (void)rest;
    uint32_t total, free;
    frame_stats(&total, &free);
    heap_stats_t h;
    heap_get_stats(&h);
    kprintf("Frames: %u KB free of %u KB\n", free * 4, total * 4);
    kprintf("Heap:   %u KB free of %u KB\n", h.free / 1024, h.total / 1024);
}

static void cmd_history(int argc, char** argv, char* rest) {
    (void)argc; (void)argv; (void)rest;
    int oldest = hist_count > HISTORY ? hist_count - HISTORY : 0;
    for (int i = oldest; i < hist_count; i++) kprintf("%3d  %s\n", i + 1, history[i % HISTORY]);
}

static void cmd_ls(int argc, char** argv, char* rest) {
    (void)argc; (void)argv; (void)rest;
    if (!diskfs_ready()) { err("ls", -EIO); return; }
    int cursor = 0, n = 0;
    char name[FS_NAME_MAX];
    uint32_t size;
    while (diskfs_readdir(&cursor, name, &size) == 0) {
        kprintf("  %-12s %8u\n", name, size);
        n++;
    }
    uint32_t clusters, free, cb;
    diskfs_stats(&clusters, &free, &cb);
    kprintf_color(COLOR_DIM, "%d files, %u KB free of %u KB\n", n, free * (cb / 512) / 2,
                  clusters * (cb / 512) / 2);
}

static void cmd_touch(int argc, char** argv, char* rest) {
    (void)argc; (void)rest;
    int r = diskfs_create(argv[1]);
    if (r < 0) err(argv[1], r);
}

static void cmd_cat(int argc, char** argv, char* rest) {
    (void)argc; (void)rest;
    int node = diskfs_lookup(argv[1]);
    if (node < 0) { err(argv[1], node); return; }
    char buf[512];
    uint32_t off = 0;
    int n, last = '\n';
    while ((n = diskfs_pread(node, buf, sizeof buf, off)) > 0) {
        console_write(buf, (size_t)n);
        last = buf[n - 1];
        off += (uint32_t)n;
    }
    if (n < 0) err(argv[1], n);
    if (last != '\n') console_putc('\n');
}

static void cmd_write(int argc, char** argv, char* rest) {
    (void)argc;
    // rest is "<file> <text>"; keep the text's own spacing.
    const char* text = rest + strlen(argv[1]);
    while (*text == ' ') text++;
    char buf[LINE_MAX + 1];
    uint32_t len = (uint32_t)strlcpy(buf, text, LINE_MAX);
    buf[len++] = '\n';
    int r = diskfs_write_file(argv[1], buf, len);
    if (r < 0) err(argv[1], r);
}

static void cmd_rm(int argc, char** argv, char* rest) {
    (void)argc; (void)rest;
    int r = diskfs_unlink(argv[1]);
    if (r < 0) err(argv[1], r);
}

static const struct { const char* name; uint8_t color; } palette[] = {
    { "blue", 0x01 }, { "green", 0x02 }, { "cyan", 0x03 }, { "red", 0x04 },
    { "magenta", 0x05 }, { "yellow", 0x0E }, { "gray", 0x07 }, { "lightgreen", 0x0A },
    { "lightred", 0x0C }, { "lightcyan", 0x0B }, { "white", 0x0F },
};

static void cmd_color(int argc, char** argv, char* rest) {
    (void)argc; (void)rest;
    for (uint32_t i = 0; i < ARRAY_SIZE(palette); i++) {
        if (strcmp(argv[1], palette[i].name) == 0) {
            console_set_color(palette[i].color);
            return;
        }
    }
    kprintf_color(COLOR_ERROR, "Unknown color. Try one of:");
    for (uint32_t i = 0; i < ARRAY_SIZE(palette); i++) kprintf(" %s", palette[i].name);
    kprintf("\n");
}

static void cmd_uptime(int argc, char** argv, char* rest) {
    (void)argc; (void)argv; (void)rest;
    uint32_t t = timer_ticks();
    kprintf("Up %u.%02u seconds (%u ticks at %d Hz)\n", t / TIMER_HZ, t % TIMER_HZ, t, TIMER_HZ);
}

static void cmd_sleep(int argc, char** argv, char* rest) {
    (void)argc; (void)rest;
    uint32_t s;
    if (parse_uint(argv[1], &s) || s > 3600) { usage("sleep <seconds, at most 3600>"); return; }
    task_sleep_ticks(s * TIMER_HZ);
}

static void cmd_tasks(int argc, char** argv, char* rest) {
    (void)argc; (void)argv; (void)rest;
    task_list();
}

static void cmd_faults(int argc, char** argv, char* rest) {
    (void)argc; (void)argv; (void)rest;
    faultlog_dump();
}

static void cmd_mmap(int argc, char** argv, char* rest) {
    (void)argc; (void)argv; (void)rest;
    for (uint32_t i = 0; i < boot_info.mmap_count; i++) {
        const mmap_entry_t* e = &boot_info.mmap[i];
        uint64_t end = e->base + e->len;
        kprintf("  %08x-%08x  %s\n", (uint32_t)e->base, (uint32_t)(end - 1),
                e->type == 1 ? "usable" : "reserved");
    }
}

static void cmd_kill(int argc, char** argv, char* rest) {
    (void)argc; (void)rest;
    uint32_t id;
    if (parse_uint(argv[1], &id)) { usage("kill <task id>"); return; }
    if ((int)id == current->id) { kprintf_color(COLOR_ERROR, "The shell will not kill itself\n"); return; }
    int r = task_kill((int)id);
    if (r < 0) { err("kill", r); return; }
    kprintf("Killed task %u\n", id);
}

static void cmd_run(int argc, char** argv, char* rest) {
    (void)rest;
    int background = 0;
    if (strcmp(argv[argc - 1], "&") == 0) {
        background = 1;
        argc--;
        if (argc < 2) { usage("run <program> [args] [&]"); return; }
    }
    int id = loader_spawn(argv[1], argc - 1, (const char* const*)&argv[1], 0);
    if (id < 0) { err(argv[1], id); return; }
    if (background) {
        kprintf("[%d] started in the background\n", id);
        return;
    }
    // A new program is its own job; give it the terminal until it and
    // anything it spawned have exited.
    tty_set_foreground(id);
    int status = task_join(id);
    task_wait_job(id);
    tty_set_foreground(0);
    if (status == EXIT_KILLED) kprintf("^C\n");
    else if (status != 0 && status != EXIT_FAULTED) kprintf_color(COLOR_DIM, "[exit status %d]\n", status);
}

static void cmd_reboot(int argc, char** argv, char* rest) {
    (void)argc; (void)argv; (void)rest;
    kprintf("Rebooting...\n");
    machine_reboot();
}

static void cmd_poweroff(int argc, char** argv, char* rest) {
    (void)argc; (void)argv; (void)rest;
    kprintf("Powering off...\n");
    machine_poweroff();
}

static const command_t commands[] = {
    { "help", 0, "", "this list", cmd_help },
    { "clear", 0, "", "clear the screen (also Ctrl+L)", cmd_clear },
    { "version", 0, "", "show the version", cmd_version },
    { "echo", 0, "<text>", "print text", cmd_echo },
    { "history", 0, "", "recent commands (Up/Down recall them)", cmd_history },
    { "ls", 0, "", "list files", cmd_ls },
    { "cat", 1, "<file>", "show a file", cmd_cat },
    { "touch", 1, "<file>", "create an empty file", cmd_touch },
    { "write", 2, "<file> <text>", "replace a file's contents", cmd_write },
    { "rm", 1, "<file>", "delete a file", cmd_rm },
    { "run", 1, "<prog> [args] [&]", "run a program from disk", cmd_run },
    { "tasks", 0, "", "list tasks", cmd_tasks },
    { "kill", 1, "<id>", "stop a task", cmd_kill },
    { "faults", 0, "", "recent faults", cmd_faults },
    { "mem", 0, "", "memory summary", cmd_mem },
    { "heap", 0, "", "kernel heap details", cmd_heap },
    { "mmap", 0, "", "boot memory map", cmd_mmap },
    { "uptime", 0, "", "time since boot", cmd_uptime },
    { "sleep", 1, "<seconds>", "wait", cmd_sleep },
    { "color", 1, "<name>", "set the text color", cmd_color },
    { "reboot", 0, "", "restart the machine", cmd_reboot },
    { "poweroff", 0, "", "turn the machine off", cmd_poweroff },
};

static void cmd_help(int argc, char** argv, char* rest) {
    (void)argc; (void)argv; (void)rest;
    for (uint32_t i = 0; i < ARRAY_SIZE(commands); i++) {
        char left[32];
        snprintf(left, sizeof left, "%s %s", commands[i].name, commands[i].args);
        kprintf("  %-26s %s\n", left, commands[i].help);
    }
    kprintf_color(COLOR_DIM, "Ctrl+C stops the running program.\n");
}

static void execute(char* line) {
    // Keep the text after the command word for echo and write.
    char* rest = line;
    while (*rest == ' ') rest++;
    while (*rest && *rest != ' ') rest++;
    char copy[LINE_MAX];
    strlcpy(copy, rest, sizeof copy);
    char* r = copy;
    while (*r == ' ') r++;

    char* argv[MAX_ARGS];
    int argc = split(line, argv, MAX_ARGS);
    if (argc < 0) { err("shell", -E2BIG); return; }
    if (argc == 0) return;
    for (uint32_t i = 0; i < ARRAY_SIZE(commands); i++) {
        const command_t* c = &commands[i];
        if (strcmp(argv[0], c->name) != 0) continue;
        if (argc - 1 < c->min_args) {
            char u[48];
            snprintf(u, sizeof u, "%s %s", c->name, c->args);
            usage(u);
            return;
        }
        c->fn(argc, argv, r);
        return;
    }
    kprintf_color(COLOR_ERROR, "Unknown command '%s'; try 'help'\n", argv[0]);
}

void shell_main(void* arg) {
    (void)arg;
    kprintf("Type 'help' for commands.\n");
    char line[LINE_MAX];
    for (;;) {
        console_puts_color(PROMPT, COLOR_PROMPT);
        int n = read_line(line);
        if (n <= 0) continue;
        history_add(line);
        execute(line);
    }
}
