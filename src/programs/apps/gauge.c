// Gauge, the system monitor: memory, disk and every task, redrawn on a
// timer. q or Esc quits, p pauses, + and - change how often it updates.
//
//   gauge       the live display
//   gauge -1    one snapshot as plain text, for a pipe or a script
//
// It reads what the kernel reports through SYS_KINFO (the same text
// `mem`, `disk` and `tasks` print) and waits with SYS_POLL, so a key
// is answered at once even in the middle of a refresh interval.
#include "ulib.h"

#define ROWS 25
#define COLS 80
#define BAR 30            // width of a usage bar
#define MAX_TASKS 32      // what the kernel's task table reports
#define TICKS_PER_SEC 100  // what SYS_TICKS counts in
#define LIST_TOP 9        // first screen row of the task list
#define LIST_ROWS (ROWS - 1 - LIST_TOP + 1)

typedef struct {
    int id, job, user;
    char state[12];
    unsigned mem_kb;
    char name[32];
} task_row_t;

typedef struct {
    unsigned frames_free, frames_total;  // KB
    unsigned heap_free, heap_total;      // KB
    int disk_ok;
    unsigned disk_free, disk_total;      // KB
    int ntasks;
    task_row_t tasks[MAX_TASKS];
    uint32_t ticks;
    rtc_time_t clock;
    char version[48];
} snap_t;

// ---- Reading the kernel's reports ----

static const char* find(const char* hay, const char* needle) {
    size_t n = strlen(needle);
    for (; *hay; hay++)
        if (strncmp(hay, needle, n) == 0) return hay;
    return 0;
}

// The numbers on the line that starts at p, up to max of them.
static int line_nums(const char* p, unsigned* out, int max) {
    int n = 0;
    while (*p && *p != '\n' && n < max) {
        if (*p >= '0' && *p <= '9') {
            unsigned v = 0;
            while (*p >= '0' && *p <= '9') v = v * 10 + (unsigned)(*p++ - '0');
            out[n++] = v;
        } else {
            p++;
        }
    }
    return n;
}

// Next blank-separated word of a line into out; returns where it stopped.
static const char* word(const char* p, char* out, size_t size) {
    while (*p == ' ') p++;
    size_t n = 0;
    while (*p && *p != ' ' && *p != '\n') {
        if (n + 1 < size) out[n++] = *p;
        p++;
    }
    out[n] = 0;
    return p;
}

static char info[4096];

static int fetch(int what) {
    int n = sys_kinfo(what, info, sizeof info);
    if (n < 0) info[0] = 0;
    return n;
}

// "ID JOB KIND STATE MEM NAME", one task per line after the header.
static void parse_tasks(snap_t* s) {
    s->ntasks = 0;
    const char* p = strchr(info, '\n');  // skip the header
    while (p && *p == '\n' && s->ntasks < MAX_TASKS) {
        p++;
        if (!*p) break;
        task_row_t* t = &s->tasks[s->ntasks];
        char w[32];
        uint32_t v;
        p = word(p, w, sizeof w);
        if (parse_uint(w, &v)) break;
        t->id = (int)v;
        p = word(p, w, sizeof w);
        t->job = parse_uint(w, &v) ? 0 : (int)v;
        p = word(p, w, sizeof w);
        t->user = strcmp(w, "user") == 0;
        p = word(p, t->state, sizeof t->state);
        p = word(p, w, sizeof w);
        unsigned kb[1];  // "312K", or "-" for a kernel task
        t->mem_kb = line_nums(w, kb, 1) ? kb[0] : 0;
        p = word(p, t->name, sizeof t->name);
        s->ntasks++;
        p = strchr(p, '\n');
    }
}

static void take(snap_t* s) {
    memset(s, 0, sizeof *s);
    s->ticks = sys_ticks();
    sys_time(&s->clock);

    if (fetch(KINFO_VERSION) >= 0) {
        size_t n = 0;
        while (info[n] && info[n] != '\n' && n + 1 < sizeof s->version) { s->version[n] = info[n]; n++; }
    }
    unsigned v[2];
    if (fetch(KINFO_MEM) >= 0) {
        const char* f = find(info, "Frames:");
        const char* h = find(info, "Heap:");
        if (f && line_nums(f, v, 2) == 2) { s->frames_free = v[0]; s->frames_total = v[1]; }
        if (h && line_nums(h, v, 2) == 2) { s->heap_free = v[0]; s->heap_total = v[1]; }
    }
    if (fetch(KINFO_DISK) >= 0 && line_nums(info, v, 2) == 2) {
        s->disk_ok = 1;
        s->disk_free = v[0];
        s->disk_total = v[1];
    }
    if (fetch(KINFO_TASKS) >= 0) parse_tasks(s);
}

// ---- Drawing ----
//
// A frame is built whole and written with one call. Every row is
// positioned with an escape and padded to the screen's width instead
// of erased first, so a redraw does not flicker.

#define A_RESET "\x1b[0m"
#define A_TITLE "\x1b[0;30;47m"
#define A_HEAD  "\x1b[0;97m"
#define A_DIM   "\x1b[0;90m"
#define A_GOOD  "\x1b[0;92m"
#define A_WARN  "\x1b[0;93m"
#define A_BAD   "\x1b[0;91m"
#define A_INFO  "\x1b[0;96m"

static char frame[8192];

typedef struct { sbuf_t sb; int ansi, col; } view_t;

static void row_begin(view_t* v, int row) {
    v->col = 0;
    if (v->ansi) sbprintf(&v->sb, "\x1b[%d;1H", row);
}

static void put(view_t* v, const char* attr, const char* text) {
    if (v->ansi) sbprintf(&v->sb, "%s%s", attr, text);
    else sbprintf(&v->sb, "%s", text);
    v->col += (int)strlen(text);
}

static void put_n(view_t* v, const char* attr, char ch, int n) {
    char s[COLS + 1];
    if (n > COLS) n = COLS;
    if (n < 0) n = 0;
    memset(s, ch, (size_t)n);
    s[n] = 0;
    put(v, attr, s);
}

// Finish the row: pad it out to the screen's width, in `attr`.
static void row_end(view_t* v, const char* attr, int last) {
    if (v->ansi) {
        put_n(v, attr, ' ', COLS - (last ? 1 : 0) - v->col);
        sbprintf(&v->sb, A_RESET);
    } else {
        sbprintf(&v->sb, "\n");
    }
}

static const char* level(unsigned pct) { return pct < 70 ? A_GOOD : pct < 90 ? A_WARN : A_BAD; }

// "  Name    [######....]  42%  1234 KB used of 4096 KB"
static void usage_row(view_t* v, int row, const char* name, unsigned free_kb, unsigned total_kb) {
    char s[64];
    unsigned used = total_kb > free_kb ? total_kb - free_kb : 0;
    unsigned pct = total_kb ? (unsigned)((uint64_t)used * 100 / total_kb) : 0;
    int fill = (int)((uint64_t)BAR * pct / 100);
    row_begin(v, row);
    snprintf(s, sizeof s, "  %-8s", name);
    put(v, A_HEAD, s);
    if (v->ansi) {
        put_n(v, level(pct), '\xDB', fill);
        put_n(v, A_DIM, '\xB0', BAR - fill);
    } else {
        put(v, "", "[");
        put_n(v, "", '#', fill);
        put_n(v, "", '.', BAR - fill);
        put(v, "", "]");
    }
    snprintf(s, sizeof s, " %3u%%  %u KB used of %u KB", pct, used, total_kb);
    put(v, A_RESET, s);
    row_end(v, A_RESET, 0);
}

static const char* state_attr(const char* state) {
    if (strcmp(state, "running") == 0) return A_GOOD;
    if (strcmp(state, "ready") == 0) return A_WARN;
    if (strcmp(state, "blocked") == 0) return A_INFO;
    return A_RESET;
}

static const char* const rate_name[] = { "0.25 s", "0.5 s", "1 s", "2 s", "5 s" };
static const uint32_t rate_ticks[] = { 25, 50, 100, 200, 500 };
#define RATES ((int)ARRAY_SIZE(rate_ticks))

static void draw(view_t* v, const snap_t* s, int paused, int rate) {
    char a[96], b[96];
    sbuf_t* sb = &v->sb;
    sb->len = 0;
    sb->buf[0] = 0;

    // Title: the version on the left, uptime and the clock on the right.
    uint32_t up = s->ticks / TICKS_PER_SEC;
    snprintf(a, sizeof a, " Gauge  %s", s->version);
    snprintf(b, sizeof b, "up %u:%02u:%02u   %04u-%02u-%02u %02u:%02u:%02u ",
             up / 3600, up / 60 % 60, up % 60, s->clock.year, s->clock.month, s->clock.day,
             s->clock.hour, s->clock.min, s->clock.sec);
    row_begin(v, 1);
    put(v, A_TITLE, a);
    put_n(v, A_TITLE, ' ', COLS - v->col - (int)strlen(b));
    put(v, A_TITLE, b);
    row_end(v, A_TITLE, 0);

    row_begin(v, 2);
    row_end(v, A_RESET, 0);

    usage_row(v, 3, "Frames", s->frames_free, s->frames_total);
    usage_row(v, 4, "Heap", s->heap_free, s->heap_total);
    if (s->disk_ok) {
        usage_row(v, 5, "Disk", s->disk_free, s->disk_total);
    } else {
        row_begin(v, 5);
        put(v, A_HEAD, "  Disk    ");
        put(v, A_DIM, "none");
        row_end(v, A_RESET, 0);
    }

    row_begin(v, 6);
    row_end(v, A_RESET, 0);

    // Task summary.
    int running = 0, ready = 0, sleeping = 0, blocked = 0;
    unsigned user_kb = 0;
    for (int i = 0; i < s->ntasks; i++) {
        const task_row_t* t = &s->tasks[i];
        if (strcmp(t->state, "running") == 0) running++;
        else if (strcmp(t->state, "ready") == 0) ready++;
        else if (strcmp(t->state, "sleeping") == 0) sleeping++;
        else blocked++;
        user_kb += t->mem_kb;
    }
    row_begin(v, 7);
    put(v, A_HEAD, "  Tasks   ");
    snprintf(a, sizeof a, "%d: %d running, %d ready, %d sleeping, %d blocked", s->ntasks, running,
             ready, sleeping, blocked);
    put(v, A_RESET, a);
    snprintf(b, sizeof b, "user memory %u KB", user_kb);
    put_n(v, A_RESET, ' ', COLS - v->col - (int)strlen(b) - 2);
    put(v, A_DIM, b);
    row_end(v, A_RESET, 0);

    row_begin(v, 8);
    put(v, A_HEAD, "  ID  JOB  KIND    STATE     MEM   NAME");
    row_end(v, A_RESET, 0);

    int shown = s->ntasks;
    if (v->ansi && shown > LIST_ROWS) shown = LIST_ROWS - 1;
    for (int i = 0; i < shown; i++) {
        const task_row_t* t = &s->tasks[i];
        char mem[12] = "-";
        if (t->user) snprintf(mem, sizeof mem, "%uK", t->mem_kb);
        snprintf(a, sizeof a, "%4d %4d  %-6s  ", t->id, t->job, t->user ? "user" : "kernel");
        row_begin(v, LIST_TOP + i);
        put(v, A_RESET, a);
        snprintf(a, sizeof a, "%-8s", t->state);
        put(v, state_attr(t->state), a);
        snprintf(a, sizeof a, " %5s   %s", mem, t->name);
        put(v, A_RESET, a);
        row_end(v, A_RESET, 0);
    }
    if (shown < s->ntasks) {
        row_begin(v, LIST_TOP + shown);
        snprintf(a, sizeof a, "  ... and %d more", s->ntasks - shown);
        put(v, A_DIM, a);
        row_end(v, A_RESET, 0);
    }

    if (v->ansi) {
        row_begin(v, ROWS);
        if (paused) snprintf(a, sizeof a, " PAUSED   p resume   q quit");
        else snprintf(a, sizeof a, " q quit   p pause   +/- speed (every %s)", rate_name[rate]);
        put(v, A_TITLE, a);
        row_end(v, A_TITLE, 1);
    }
}

static void show(const char* s, size_t n) {
    while (n > 0) {
        int r = sys_write(STDOUT_FILENO, s, (uint32_t)n);
        if (r <= 0) return;
        s += r;
        n -= (size_t)r;
    }
}

static void render(view_t* v, const snap_t* s, int paused, int rate) {
    draw(v, s, paused, rate);
    show(frame, v->sb.len < sizeof frame ? v->sb.len : sizeof frame - 1);
}

// ---- The live display ----

static int live(void) {
    static snap_t snap;
    view_t v = { .sb = { frame, sizeof frame, 0 }, .ansi = 1 };
    int old = sys_tty_mode(TTY_RAW | TTY_NOSIG);
    if (old < 0) {
        printf("gauge: cannot use the terminal: %s\n", strerror(old));
        return 1;
    }
    print("\x1b[0m\x1b[2J\x1b[?25l");

    int paused = 0, rate = 2;
    take(&snap);
    for (int done = 0; !done;) {
        render(&v, &snap, paused, rate);
        uint32_t until = sys_ticks() + rate_ticks[rate];
        int redraw = 0;
        while (!redraw) {
            int left = (int)(until - sys_ticks());
            if (left <= 0) break;
            int r = sys_poll((uint32_t)left);
            if (r < 0) { done = 1; break; }
            if (r == 0) break;
            unsigned char k[16];
            int n = sys_read(STDIN_FILENO, k, sizeof k);
            if (n <= 0) { done = 1; break; }
            if (k[0] == 27 && n > 1) continue;  // an arrow key or another escape sequence
            for (int i = 0; i < n && !done; i++) {  // keys typed together arrive in one read
                switch (k[i]) {
                case 'q': case 'Q': case 27: case 3: done = 1; break;
                case 'p': case 'P': paused = !paused; redraw = 1; break;
                case '+': case '=': if (rate > 0) rate--; redraw = 1; break;
                case '-': case '_': if (rate < RATES - 1) rate++; redraw = 1; break;
                }
            }
            if (done) break;
        }
        if (!paused) take(&snap);
    }
    print("\x1b[0m\x1b[2J\x1b[H\x1b[?25h");
    sys_tty_mode(0);
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 2 && strcmp(argv[1], "-1") == 0) {
        static snap_t snap;
        view_t v = { .sb = { frame, sizeof frame, 0 }, .ansi = 0 };
        take(&snap);
        render(&v, &snap, 0, 2);
        return 0;
    }
    if (argc != 1) {
        dprintf(STDERR_FILENO, "usage: gauge [-1]\n");
        return 1;
    }
    return live();
}
