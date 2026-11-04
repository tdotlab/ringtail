// PS/2 keyboard, scancode set 1. The interrupt handler translates
// scancodes to characters (shift, caps lock, ctrl, the E0-prefixed
// keys) and hands them to the terminal. While a program owns the
// graphics display it gets the raw scancodes instead, through a
// separate queue, because games and GUIs want key-up events too.
#include "kernel.h"
#include "arch.h"
#include "interrupt.h"
#include "drivers.h"
#include "tty.h"
#include "task.h"

static const char normal_map[128] = {
    0, 0x1B, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0,
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0,
    '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*', 0, ' ',
};
static const char shift_map[128] = {
    0, 0x1B, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0,
    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0,
    '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
    '*', 0, ' ',
};

#define SC_LCTRL  0x1D
#define SC_LSHIFT 0x2A
#define SC_RSHIFT 0x36
#define SC_CAPS   0x3A
#define SC_BACKSPACE 0x0E
#define SC_ALT    0x38

// Navigation keys. The arrows answer with or without the E0 prefix
// (the keypad sends them without, while Num Lock is off, which is
// all this driver knows). The rest need E0, so the keypad keeps its
// digits there.
static int nav_key(uint8_t code, int ext) {
    switch (code) {
    case 0x48: return NAV_UP;
    case 0x50: return NAV_DOWN;
    case 0x4B: return NAV_LEFT;
    case 0x4D: return NAV_RIGHT;
    }
    if (!ext) {
        if (code >= 0x3B && code <= 0x44) return NAV_F1 + (code - 0x3B);  // F1-F10
        if (code == 0x57) return NAV_F1 + 10;
        if (code == 0x58) return NAV_F1 + 11;
        return -1;
    }
    switch (code) {
    case 0x47: return NAV_HOME;
    case 0x4F: return NAV_END;
    case 0x52: return NAV_INSERT;
    case 0x53: return NAV_DELETE;
    case 0x49: return NAV_PGUP;
    case 0x51: return NAV_PGDN;
    }
    return -1;
}

#define RAW_SIZE 256
static volatile uint8_t raw_queue[RAW_SIZE];
static volatile uint32_t raw_head, raw_tail;

static int ext, lshift, rshift, lctrl, rctrl, lalt, ralt, caps;

static void translate(uint8_t sc) {
    if (sc == 0xE0) { ext = 1; return; }
    int was_ext = ext;
    ext = 0;
    int release = sc & 0x80;
    uint8_t code = sc & 0x7F;

    if (code == SC_LCTRL) {
        if (was_ext) rctrl = !release; else lctrl = !release;
        return;
    }
    if (code == SC_ALT) {
        if (was_ext) ralt = !release; else lalt = !release;
        return;
    }
    if (code == SC_LSHIFT || code == SC_RSHIFT) {
        // E0 2A / E0 AA are fake shifts some keyboards send around
        // the navigation keys; they are not the user's shift.
        if (!was_ext) {
            if (code == SC_LSHIFT) lshift = !release; else rshift = !release;
        }
        return;
    }
    if (release) return;
    if (code == SC_CAPS) { caps = !caps; return; }
    int shift = lshift || rshift;
    int ctrl = lctrl || rctrl;
    int nav = nav_key(code, was_ext);
    if (nav >= 0) {
        tty_input(KEY_NAV(nav, (shift ? KEY_MOD_SHIFT : 0) | (ctrl ? KEY_MOD_CTRL : 0)));
        return;
    }
    if (was_ext) {
        if (code == 0x1C) tty_input('\n');      // keypad Enter
        else if (code == 0x35) tty_input('/');  // keypad slash
        return;
    }
    if (ctrl && code == SC_BACKSPACE) { tty_input(KEY_CTRL_W); return; }
    char c = shift ? shift_map[code] : normal_map[code];
    if (!c) return;
    if (c >= 'a' && c <= 'z' && caps && !shift) c = (char)(c - 'a' + 'A');
    else if (c >= 'A' && c <= 'Z' && caps && shift) c = (char)(c - 'A' + 'a');
    if (ctrl) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) c = (char)(c & 0x1F);
        else return;
    }
    if (lalt || ralt) tty_input(0x1B);  // Alt+key: ESC, then the key
    tty_input((uint8_t)c);
}

static void keyboard_irq(void) {
    uint8_t sc = inb(0x60);
    if (gfx_owner() >= 0) {
        if (raw_head - raw_tail < RAW_SIZE) {
            raw_queue[raw_head % RAW_SIZE] = sc;
            raw_head++;
        }
        task_wakeup(&input_chan);
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
