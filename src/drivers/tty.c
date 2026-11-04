// The terminal: one input queue fed by the keyboard and serial
// interrupt handlers, read by the shell's line editor and by
// programs through standard input.
#include "kernel.h"
#include "arch.h"
#include "console.h"
#include "drivers.h"
#include "task.h"
#include "tty.h"
#include "ringtail/abi.h"

#define QUEUE_SIZE 256
#define LINE_MAX 256

static volatile uint8_t queue[QUEUE_SIZE];
static volatile uint32_t head, tail;
static volatile int fg_job;
static volatile int mode;
int input_chan;

// Bytes already produced for a read but not yet taken: the rest of a
// cooked line read with a small buffer, or the rest of an escape
// sequence in raw mode.
static char pending[LINE_MAX];
static uint32_t pending_len, pending_pos;

// With TTY_NOSIG, Ctrl+C is an ordinary key, but three presses within
// a second still kill the job, so a stuck program cannot lock you out.
static uint32_t ctrl_c_ticks[3];

static int force_kill(void) {
    ctrl_c_ticks[0] = ctrl_c_ticks[1];
    ctrl_c_ticks[1] = ctrl_c_ticks[2];
    ctrl_c_ticks[2] = timer_ticks();
    return ctrl_c_ticks[0] && ctrl_c_ticks[2] - ctrl_c_ticks[0] < TIMER_HZ;
}

void tty_input(int c) {
    if (c == KEY_CTRL_C && fg_job && (!(mode & TTY_NOSIG) || force_kill())) {
        task_kill_job(fg_job);
        return;
    }
    if (gfx_owner() >= 0) return;  // the display owner reads raw scancodes
    if (head - tail < QUEUE_SIZE) {
        queue[head % QUEUE_SIZE] = (uint8_t)c;
        head++;
    }
    task_wakeup(&input_chan);
}

int tty_pending(void) { return head != tail || pending_pos != pending_len; }

int tty_getc(void) {
    uint32_t f = irq_save();
    int c;
    for (;;) {
        if (head != tail) {
            c = queue[tail % QUEUE_SIZE];
            tail++;
            break;
        }
        if (current->killed) { c = -EINTR; break; }
        task_block(&input_chan);
    }
    irq_restore(f);
    return c;
}

static int try_getc(void) {
    uint32_t f = irq_save();
    int c = -1;
    if (head != tail) {
        c = queue[tail % QUEUE_SIZE];
        tail++;
    }
    irq_restore(f);
    return c;
}

void tty_set_foreground(int job) {
    uint32_t f = irq_save();
    int was_raw = mode != 0;
    fg_job = job;
    mode = 0;
    pending_pos = pending_len = 0;  // half-read input belongs to the old job
    ctrl_c_ticks[0] = ctrl_c_ticks[1] = ctrl_c_ticks[2] = 0;
    irq_restore(f);
    // A raw-mode program may have died with the screen in any state.
    if (was_raw) console_reset();
}

int tty_foreground(void) { return fg_job; }

int tty_set_mode(int m) {
    if (m & ~(TTY_RAW | TTY_NOSIG)) return -EINVAL;
    if (current->job != fg_job) return -EPERM;
    uint32_t f = irq_save();
    int old = mode;
    mode = m;
    pending_pos = pending_len = 0;
    irq_restore(f);
    return old;
}

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

static int take_pending(char* buf, uint32_t len) {
    uint32_t avail = pending_len - pending_pos;
    uint32_t take = avail < len ? avail : len;
    memcpy(buf, pending + pending_pos, take);
    pending_pos += take;
    return (int)take;
}

static int raw_read(char* buf, uint32_t len) {
    if (pending_pos != pending_len) return take_pending(buf, len);
    int c = tty_getc();  // block for the first key only
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
            memcpy(pending, seq + fit, k - fit);
            pending_pos = 0;
            pending_len = k - fit;
            return (int)len;
        }
        memcpy(buf + n, seq, k);
        n += k;
        if (n == len || (c = try_getc()) < 0) return (int)n;
    }
}

static int cooked_read(char* buf, uint32_t len) {
    if (pending_pos == pending_len) {
        char line[LINE_MAX];
        uint32_t n = 0;
        int eof = 0;
        for (;;) {
            int c = tty_getc();
            if (c < 0) return c;
            if (c == '\n') {
                console_putc('\n');
                line[n++] = '\n';
                break;
            }
            if (c == KEY_CTRL_D) {
                if (n == 0) eof = 1;
                break;
            }
            if (c == '\b') {
                if (n > 0) { n--; console_backspace(); }
                continue;
            }
            if ((c >= 0x20 && c < 0x7F) || c == '\t') {
                if (n < LINE_MAX - 1) {
                    line[n++] = (char)c;
                    console_putc((char)c);
                }
            }
        }
        if (eof) return 0;
        memcpy(pending, line, n);
        pending_len = n;
        pending_pos = 0;
    }
    return take_pending(buf, len);
}

int tty_read(char* buf, uint32_t len) {
    if (len == 0) return 0;
    return (mode & TTY_RAW) ? raw_read(buf, len) : cooked_read(buf, len);
}
