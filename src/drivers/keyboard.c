// PS/2 keyboard, scancode set 1. The interrupt handler translates
// scancodes to characters with keymap.c (shared with the window
// server) and hands them to the terminal. While a program owns the
// graphics display it gets the raw scancodes instead, through a
// separate queue, because games and GUIs want key-up events too.
#include "kernel.h"
#include "arch.h"
#include "interrupt.h"
#include "drivers.h"
#include "tty.h"
#include "task.h"
#include "keymap.h"
#include "fs.h"  // poll_notify

#define RAW_SIZE 256
static volatile uint8_t raw_queue[RAW_SIZE];
static volatile uint32_t raw_head, raw_tail;

static keymap_t keys;

static void translate(uint8_t sc) {
    keyev_t e;
    if (!keymap_feed(&keys, sc, &e) || !e.code) return;
    if (e.esc) tty_input(0x1B);
    tty_input(e.code);
}

static void keyboard_irq(void) {
    uint8_t sc = inb(0x60);
    if (gfx_owner() >= 0) {
        if (raw_head - raw_tail < RAW_SIZE) {
            raw_queue[raw_head % RAW_SIZE] = sc;
            raw_head++;
        }
        task_wakeup(&input_chan);
        poll_notify();  // POLLFD_INPUT
    }
    translate(sc);
}

void keyboard_init(void) {
    while (inb(0x64) & 0x01) (void)inb(0x60);  // drop anything stale
    irq_register(IRQ_KEYBOARD, keyboard_irq);
}

int keyboard_raw_get(void) {
    uint32_t f = irq_save();
    int r = -1;
    if (raw_tail != raw_head) {
        r = raw_queue[raw_tail % RAW_SIZE];
        raw_tail++;
    }
    irq_restore(f);
    return r;
}

int keyboard_raw_pending(void) { return raw_head != raw_tail; }

void keyboard_raw_flush(void) {
    uint32_t f = irq_save();
    raw_tail = raw_head;
    irq_restore(f);
}
