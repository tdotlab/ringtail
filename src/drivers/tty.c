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
int input_chan;

// Part of a line a program read with a buffer too small for it.
static char pending[LINE_MAX];
static uint32_t pending_len, pending_pos;

void tty_input(int c) {
    if (c == KEY_CTRL_C && fg_job) {
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

int tty_pending(void) { return head != tail; }

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

void tty_set_foreground(int job) {
    uint32_t f = irq_save();
    fg_job = job;
    pending_pos = pending_len = 0;  // a half-read line belongs to the old job
    irq_restore(f);
}
int tty_foreground(void) { return fg_job; }

int tty_read(char* buf, uint32_t len) {
    if (len == 0) return 0;
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
    uint32_t avail = pending_len - pending_pos;
    uint32_t take = avail < len ? avail : len;
    memcpy(buf, pending + pending_pos, take);
    pending_pos += take;
    return (int)take;
}
