// Terminals: the console, fed by the keyboard and serial interrupt
// handlers and shown by console.c, and pseudo-terminals, fed and read
// by a program (a terminal window) through the pty's master side.
//
// Each has its own input queue, line discipline, mode, foreground job
// and handovers. Programs reach theirs through their controlling
// terminal (task_t.tty; 0 is the console), which a program gets from
// its parent, or from its standard input when that is a terminal.
//
// A pty also has an output ring: what its programs write, and the
// echo of what is typed, waiting for the master to read it. Kernel-made
// output (echo, the reset after a raw program) never waits for room; it
// is dropped if the master falls 8 KB behind. When the master closes,
// the pty hangs up: every task whose terminal it is gets SIGHUP, reads
// return end of file and writes EIO. When the last terminal descriptor
// closes, the master reads end of file. The pty is freed with its last
// reference: the master, its terminal descriptors, and the tasks that
// have it as their terminal.
#include "kernel.h"
#include "arch.h"
#include "console.h"
#include "drivers.h"
#include "task.h"
#include "tty.h"
#include "mm.h"
#include "ringtail/abi.h"
#include "fs.h"

#define QUEUE_SIZE 256
#define LINE_MAX 256
#define OUT_SIZE 8192u
#define HANDOVER_MAX 8
#define QUIT_TICKS 200  // two seconds for three Ctrl+backslashes

struct tty {
    volatile uint8_t queue[QUEUE_SIZE];
    volatile uint32_t head, tail;
    volatile int fg_job;
    volatile int mode;
    // Bytes already produced for a read but not yet taken: the rest of
    // a cooked line read with a small buffer, or the rest of an escape
    // sequence in raw mode.
    char pending[LINE_MAX];
    uint32_t pending_len, pending_pos;
    // Ctrl+backslash presses at the job in the foreground, for the
    // third one that sends SIGKILL.
    int quit_job, quits;
    uint32_t quit_tick;
    // Handovers in progress, innermost last: the job that has the
    // terminal, the job that gave it, and that giver's mode.
    struct { int to, from, mode; } handover[HANDOVER_MAX];
    int handovers;
    mutex_t handover_lock;

    // A pty's own; the console has none of this.
    int pty;
    int cols, rows;
    int refs;     // the master, terminal descriptors, tasks
    int slaves;   // terminal descriptors open
    int hung;     // the master has closed
    uint8_t out[OUT_SIZE];  // waiting for the master; it waits on the tty itself
    uint32_t out_head, out_len;
    mutex_t out_lock;
};

static struct tty console = { .handover_lock = MUTEX_INIT, .out_lock = MUTEX_INIT };
struct tty* const console_tty = &console;
int input_chan;

struct tty* tty_current(void) { return current->tty ? current->tty : &console; }
int tty_is_console(const struct tty* t) { return !t->pty; }

// ---- Input ----

static void input(struct tty* t, int c) {
    // Ctrl+C sends SIGINT to the foreground job unless it asked for
    // TTY_NOSIG. Ctrl+\ sends SIGQUIT in any mode, and a third within
    // two seconds SIGKILL, so a program that catches both still cannot
    // lock you out.
    if (t->fg_job && c == KEY_CTRL_C && !(t->mode & TTY_NOSIG)) {
        task_signal_job(t->fg_job, SIGINT);
        return;
    }
    if (t->fg_job && c == KEY_CTRL_BACKSLASH) {
        uint32_t now = timer_ticks();
        if (t->quit_job != t->fg_job || now - t->quit_tick > QUIT_TICKS) {
            t->quit_job = t->fg_job;
            t->quit_tick = now;
            t->quits = 0;
        }
        task_signal_job(t->fg_job, ++t->quits >= 3 ? SIGKILL : SIGQUIT);
        return;
    }
    if (!t->pty && gfx_owner() >= 0) return;  // the display owner reads raw scancodes
    uint32_t f = irq_save();
    if (t->head - t->tail < QUEUE_SIZE) {
        t->queue[t->head % QUEUE_SIZE] = (uint8_t)c;
        t->head++;
    }
    irq_restore(f);
    task_wakeup(&input_chan);
    poll_notify();
}

void tty_input(int c) { input(&console, c); }

int tty_pending(struct tty* t) {
    return t->head != t->tail || t->pending_pos != t->pending_len || t->hung;
}

int tty_has_input(struct tty* t) { return current->job == t->fg_job && tty_pending(t); }

// One key: blocks for it. -EINTR if the task is signalled, -EPIPE once
// the pty has hung up.
static int getc_wait(struct tty* t) {
    uint32_t f = irq_save();
    int c;
    for (;;) {
        if (t->head != t->tail) {
            c = t->queue[t->tail % QUEUE_SIZE];
            t->tail++;
            break;
        }
        if (t->hung) { c = -EPIPE; break; }
        if (task_signalled(current)) { c = -EINTR; break; }
        task_block(&input_chan);
    }
    irq_restore(f);
    return c;
}

static int try_getc(struct tty* t) {
    uint32_t f = irq_save();
    int c = -1;
    if (t->head != t->tail) {
        c = t->queue[t->tail % QUEUE_SIZE];
        t->tail++;
    }
    irq_restore(f);
    return c;
}

// ---- Output ----

static void out_changed(struct tty* t) {
    task_wakeup(t);
    poll_notify();
}

// Kernel-made output: never waits.
static void out_put(struct tty* t, const char* s, uint32_t n) {
    if (!t->pty) {
        console_write(s, n);
        return;
    }
    uint32_t f = irq_save();
    for (uint32_t i = 0; i < n && t->out_len < OUT_SIZE && !t->hung; i++)
        t->out[(t->out_head + t->out_len++) % OUT_SIZE] = (uint8_t)s[i];
    irq_restore(f);
    out_changed(t);
}

void tty_lock_output(struct tty* t) {
    if (t->pty) mutex_lock(&t->out_lock);
    else console_lock();
}

void tty_unlock_output(struct tty* t) {
    if (t->pty) mutex_unlock(&t->out_lock);
    else console_unlock();
}

int tty_write(struct tty* t, const char* s, uint32_t len) {
    if (!t->pty) {
        console_write(s, len);
        return (int)len;
    }
    uint32_t f = irq_save();
    uint32_t done = 0;
    int err = 0;
    while (done < len) {
        if (t->hung) { err = -EIO; break; }
        uint32_t room = OUT_SIZE - t->out_len;
        if (room) {
            uint32_t n = MIN(len - done, room);
            for (uint32_t i = 0; i < n; i++) t->out[(t->out_head + t->out_len + i) % OUT_SIZE] = (uint8_t)s[done + i];
            t->out_len += n;
            done += n;
            out_changed(t);
            continue;
        }
        if (task_signalled(current)) { err = -EINTR; break; }
        task_block(t);
    }
    irq_restore(f);
    return done ? (int)done : err;
}

// ANSI colour numbers run black, red, green, yellow, blue, magenta,
// cyan, white; VGA puts blue first. The table works both ways.
static const uint8_t vga_ansi[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };

void tty_set_color(struct tty* t, uint8_t color) {
    if (!t->pty) {
        console_set_color(color);
        return;
    }
    int fg = color & 0x0F, bg = color >> 4;
    char seq[24];
    int n = snprintf(seq, sizeof seq, "\x1b[0;%d;%dm", (fg & 8 ? 90 : 30) + vga_ansi[fg & 7],
                     (bg & 8 ? 100 : 40) + vga_ansi[bg & 7]);
    tty_write(t, seq, (uint32_t)n);
}

int tty_size(struct tty* t, int* cols, int* rows) {
    if (!t->pty) return console_size(cols, rows);
    *cols = t->cols;
    *rows = t->rows;
    return 0;
}

static void reset_screen(struct tty* t) {
    if (!t->pty) {
        console_reset();
        return;
    }
    static const char seq[] = "\x1b[0m\x1b[?25h\x1b[2J\x1b[H";
    out_put(t, seq, sizeof seq - 1);
}

// ---- The foreground ----

// cleanup: the job losing the terminal is finished with it, so if it
// was in raw mode, whatever it left on the screen is cleared. Not when
// a job lends the terminal out: the shell's line editor is raw, and
// running a program must not wipe the screen.
static void switch_to_job(struct tty* t, int job, int new_mode, int cleanup) {
    uint32_t f = irq_save();
    int was_raw = (t->mode & TTY_RAW) != 0;
    t->fg_job = job;
    t->mode = new_mode;
    t->pending_pos = t->pending_len = 0;  // half-read input belongs to the old job
    irq_restore(f);
    if (was_raw && cleanup) reset_screen(t);
}

void tty_set_foreground(struct tty* t, int job) {
    mutex_lock(&t->handover_lock);
    t->handovers = 0;
    switch_to_job(t, job, 0, 1);
    mutex_unlock(&t->handover_lock);
}

static int handover_locked(struct tty* t, int job) {
    int me = current->job;
    if (job == 0) job = me;
    if (job == t->fg_job && me == t->fg_job) return 0;
    if (me == t->fg_job) {
        if (!task_job_mine(job)) return -EPERM;
        if (!task_job_alive(job)) return -ESRCH;
        if (t->handovers == HANDOVER_MAX) return -EBUSY;
        t->handover[t->handovers].to = job;
        t->handover[t->handovers].from = me;
        t->handover[t->handovers].mode = t->mode;
        t->handovers++;
        switch_to_job(t, job, 0, 0);
        return 0;
    }
    // Taking it back: only by a job that gave it away, and only once
    // everything it was passed on to since is gone.
    if (job != me) return -EPERM;
    for (int i = t->handovers - 1; i >= 0; i--) {
        if (t->handover[i].from != me) continue;
        for (int k = i; k < t->handovers; k++)
            if (task_job_alive(t->handover[k].to)) return -EBUSY;
        int m = t->handover[i].mode;
        t->handovers = i;
        switch_to_job(t, me, m, 1);
        return 0;
    }
    return -EPERM;
}

int tty_handover(struct tty* t, int job) {
    mutex_lock(&t->handover_lock);
    int r = handover_locked(t, job);
    mutex_unlock(&t->handover_lock);
    return r;
}

int tty_foreground(struct tty* t) { return t->fg_job; }

int tty_unowned(struct tty* t) { return t->pty && !t->hung && (!t->fg_job || !task_job_alive(t->fg_job)); }

int tty_set_mode(struct tty* t, int m) {
    if (m & ~(TTY_RAW | TTY_NOSIG)) return -EINVAL;
    if (current->job != t->fg_job) return -EPERM;
    uint32_t f = irq_save();
    int old = t->mode;
    t->mode = m;
    t->pending_pos = t->pending_len = 0;
    irq_restore(f);
    return old;
}

// ---- Reading ----

// The bytes a terminal would send for a key, as xterm does it:
// ESC [ A for Up, ESC [ 1 ; 5 D for Ctrl+Left, ESC [ 3 ~ for Delete,
// ESC O R for F3 and ESC [ 1 ; 2 R for Shift+F3.
static uint32_t encode_key(int c, char out[8]) {
    if (c == '\b') { out[0] = 0x7F; return 1; }
    if (!KEY_IS_NAV(c)) { out[0] = (char)c; return 1; }
    static const char final[] = { 'A', 'B', 'C', 'D', 'H', 'F' };
    static const uint8_t tilde[] = { 0, 0, 0, 0, 0, 0, 2, 3, 5, 6,
                                     0, 0, 0, 0, 15, 17, 18, 19, 20, 21, 23, 24 };
    int key = KEY_NAV_KEY(c), mods = KEY_NAV_MODS(c);
    int xmod = 1 + ((mods & KEY_MOD_SHIFT) ? 1 : 0) + ((mods & KEY_MOD_CTRL) ? 4 : 0);
    uint32_t n = 0;
    out[n++] = 0x1B;
    if (key >= NAV_F1 && key < NAV_F1 + 4) {
        // F1-F4: ESC O P..S, or ESC [ 1 ; mod P..S with modifiers
        if (mods) { out[n++] = '['; out[n++] = '1'; out[n++] = ';'; out[n++] = (char)('0' + xmod); }
        else out[n++] = 'O';
        out[n++] = (char)('P' + key - NAV_F1);
    } else if (key < NAV_INSERT) {
        out[n++] = '[';
        if (mods) { out[n++] = '1'; out[n++] = ';'; out[n++] = (char)('0' + xmod); }
        out[n++] = final[key];
    } else {
        out[n++] = '[';
        if (tilde[key] >= 10) out[n++] = (char)('0' + tilde[key] / 10);
        out[n++] = (char)('0' + tilde[key] % 10);
        if (mods) { out[n++] = ';'; out[n++] = (char)('0' + xmod); }
        out[n++] = '~';
    }
    return n;
}

static int take_pending(struct tty* t, char* buf, uint32_t len) {
    uint32_t avail = t->pending_len - t->pending_pos;
    uint32_t take = avail < len ? avail : len;
    memcpy(buf, t->pending + t->pending_pos, take);
    t->pending_pos += take;
    return (int)take;
}

static int raw_read(struct tty* t, char* buf, uint32_t len) {
    if (t->pending_pos != t->pending_len) return take_pending(t, buf, len);
    int c = getc_wait(t);  // block for the first key only
    if (c < 0) return c;
    uint32_t n = 0;
    for (;;) {
        char seq[8];
        uint32_t k = encode_key(c, seq);
        if (n + k > len) {
            // Keep a sequence whole: what does not fit waits for the
            // next read.
            uint32_t fit = len - n;
            memcpy(buf + n, seq, fit);
            memcpy(t->pending, seq + fit, k - fit);
            t->pending_pos = 0;
            t->pending_len = k - fit;
            return (int)len;
        }
        memcpy(buf + n, seq, k);
        n += k;
        if (n == len || (c = try_getc(t)) < 0) return (int)n;
    }
}

static int cooked_read(struct tty* t, char* buf, uint32_t len) {
    if (t->pending_pos == t->pending_len) {
        char line[LINE_MAX];
        uint32_t n = 0;
        int eof = 0;
        for (;;) {
            int c = getc_wait(t);
            if (c < 0) return c;
            if (c == '\n') {
                out_put(t, "\n", 1);
                line[n++] = '\n';
                break;
            }
            if (c == KEY_CTRL_D) {
                if (n == 0) eof = 1;
                break;
            }
            if (c == '\b') {
                if (n > 0) {
                    n--;
                    if (t->pty) out_put(t, "\b \b", 3);
                    else console_backspace();
                }
                continue;
            }
            if ((c >= 0x20 && c < 0x7F) || c == '\t') {
                if (n < LINE_MAX - 1) {
                    line[n++] = (char)c;
                    char ch = (char)c;
                    out_put(t, &ch, 1);
                }
            }
        }
        if (eof) return 0;
        memcpy(t->pending, line, n);
        t->pending_len = n;
        t->pending_pos = 0;
    }
    return take_pending(t, buf, len);
}

int tty_read(struct tty* t, char* buf, uint32_t len) {
    if (len == 0) return 0;
    int n = (t->mode & TTY_RAW) ? raw_read(t, buf, len) : cooked_read(t, buf, len);
    return n == -EPIPE ? 0 : n;  // hung up: end of file
}

// ---- Ptys ----

struct tty* pty_new(int cols, int rows) {
    if (cols < 1 || rows < 1 || cols > 1000 || rows > 1000) return 0;
    struct tty* t = kzalloc(sizeof *t);
    if (!t) return 0;
    t->pty = 1;
    t->cols = cols;
    t->rows = rows;
    t->refs = 1;  // the master
    return t;
}

static void unref(struct tty* t, int slave) {
    if (!t || !t->pty) return;
    uint32_t f = irq_save();
    int last_slave = slave && --t->slaves == 0;
    int gone = --t->refs == 0;
    irq_restore(f);
    if (last_slave) out_changed(t);  // the master reads end of file
    if (gone) kfree(t);
}

void tty_ref(struct tty* t) {
    if (!t || !t->pty) return;
    uint32_t f = irq_save();
    t->refs++;
    irq_restore(f);
}

void tty_unref(struct tty* t) { unref(t, 0); }

void tty_slave_ref(struct tty* t) {
    if (!t->pty) return;
    uint32_t f = irq_save();
    t->refs++;
    t->slaves++;
    irq_restore(f);
}

void tty_slave_unref(struct tty* t) { unref(t, 1); }

int pty_master_read(struct tty* t, uint8_t* buf, uint32_t len) {
    uint32_t f = irq_save();
    int r;
    for (;;) {
        if (t->out_len) {
            uint32_t n = MIN(len, t->out_len);
            for (uint32_t i = 0; i < n; i++) buf[i] = t->out[(t->out_head + i) % OUT_SIZE];
            t->out_head = (t->out_head + n) % OUT_SIZE;
            t->out_len -= n;
            out_changed(t);
            r = (int)n;
            break;
        }
        if (t->slaves == 0) { r = 0; break; }
        if (task_signalled(current)) { r = -EINTR; break; }
        task_block(t);
    }
    irq_restore(f);
    return r;
}

int pty_master_write(struct tty* t, const uint8_t* buf, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) input(t, buf[i]);
    return (int)len;
}

int pty_master_events(struct tty* t) {
    uint32_t f = irq_save();
    int ev = POLLOUT | (t->out_len ? POLLIN : 0) | (t->slaves == 0 ? POLLIN | POLLHUP : 0);
    irq_restore(f);
    return ev;
}

int tty_out_events(struct tty* t) {
    if (!t->pty) return POLLOUT;
    uint32_t f = irq_save();
    int ev = t->hung ? POLLERR : t->out_len < OUT_SIZE ? POLLOUT : 0;
    irq_restore(f);
    return ev;
}

void pty_master_close(struct tty* t) {
    uint32_t f = irq_save();
    t->hung = 1;
    irq_restore(f);
    task_wakeup(&input_chan);
    out_changed(t);
    task_signal_tty(t, SIGHUP);
    tty_unref(t);
}
