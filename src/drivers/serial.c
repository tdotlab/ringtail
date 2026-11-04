// COM1 serial port: a second console. Output mirrors the screen;
// input feeds the same terminal queue as the keyboard, translating
// what terminals send (CR for Enter, DEL for backspace, ANSI escape
// sequences for the navigation keys).
#include "kernel.h"
#include "arch.h"
#include "interrupt.h"
#include "drivers.h"
#include "tty.h"

#define COM1 0x3F8

static int present;

int serial_present(void) { return present; }

// Escape sequences from the terminal: CSI (ESC [ params final) and
// SS3 (ESC O final), as xterm sends them for the navigation and
// function keys,
// with an optional modifier parameter (2 Shift, 5 Ctrl, 6 both).
static int nav_mods(int p) {
    if (p < 2) return 0;
    p--;
    return ((p & 1) ? KEY_MOD_SHIFT : 0) | ((p & 4) ? KEY_MOD_CTRL : 0);
}

static void csi_key(int final, const int* params, int n) {
    int nav = -1, mod = n >= 2 ? params[1] : 0;
    switch (final) {
    case 'A': nav = NAV_UP; break;
    case 'B': nav = NAV_DOWN; break;
    case 'C': nav = NAV_RIGHT; break;
    case 'D': nav = NAV_LEFT; break;
    case 'H': nav = NAV_HOME; break;
    case 'F': nav = NAV_END; break;
    case 'P': case 'Q': case 'R': case 'S': nav = NAV_F1 + (final - 'P'); break;
    case '~':
        switch (n ? params[0] : 0) {
        case 1: case 7: nav = NAV_HOME; break;
        case 4: case 8: nav = NAV_END; break;
        case 2: nav = NAV_INSERT; break;
        case 3: nav = NAV_DELETE; break;
        case 5: nav = NAV_PGUP; break;
        case 6: nav = NAV_PGDN; break;
        case 11: case 12: case 13: case 14: case 15:
            nav = NAV_F1 + params[0] - 11; break;
        case 17: case 18: case 19: case 20: case 21:
            nav = NAV_F1 + 5 + params[0] - 17; break;
        case 23: case 24:
            nav = NAV_F1 + 10 + params[0] - 23; break;
        }
        break;
    }
    if (nav >= 0) tty_input(KEY_NAV(nav, nav_mods(mod)));
}

static void serial_irq(void) {
    static int state;     // 0 plain, 1 after ESC, 2 in CSI, 3 after ESC O
    static int params[4], nparams;
    static int last_cr;
    while (inb(COM1 + 5) & 0x01) {
        int c = inb(COM1);
        if (state == 1) {
            if (c == '[') { state = 2; nparams = 0; params[0] = 0; continue; }
            if (c == 'O') { state = 3; continue; }
            state = 0;
            tty_input(0x1B);  // a lone Escape (or Alt), then this byte as usual
        } else if (state == 2) {
            if (c >= '0' && c <= '9') {
                if (nparams == 0) nparams = 1;
                if (nparams <= 4 && params[nparams - 1] < 1000)
                    params[nparams - 1] = params[nparams - 1] * 10 + (c - '0');
            } else if (c == ';') {
                if (nparams == 0) nparams = 1;
                if (nparams < 4) params[nparams++] = 0; else nparams = 5;
            } else if (c >= 0x40 && c <= 0x7E) {
                state = 0;
                csi_key(c, params, nparams > 4 ? 4 : nparams);
            } else if (c < 0x20 || c > 0x3F) {
                state = 0;  // not a sequence we understand: drop it
            }
            continue;
        } else if (state == 3) {
            state = 0;
            csi_key(c, params, 0);
            continue;
        }
        if (c == 0x1B) { state = 1; continue; }
        if (c >= 0x80) continue;  // ASCII only; high bytes would look like keys
        if (c == '\n' && last_cr) { last_cr = 0; continue; }
        last_cr = (c == '\r');
        if (c == '\r') c = '\n';
        if (c == 0x7F) c = '\b';
        tty_input(c);
    }
}

void serial_init(void) {
    outb(COM1 + 1, 0x00);  // interrupts off while configuring
    outb(COM1 + 3, 0x80);  // divisor latch
    outb(COM1 + 0, 0x01);  // 115200 baud
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);  // 8 data bits, no parity, 1 stop bit
    outb(COM1 + 2, 0xC7);  // FIFO on, cleared, 14-byte threshold
    // Loopback self-test: a missing port reads back 0xFF.
    outb(COM1 + 4, 0x1E);
    outb(COM1 + 0, 0xAE);
    if (inb(COM1 + 0) != 0xAE) return;
    outb(COM1 + 4, 0x0B);  // normal mode: DTR, RTS, OUT2 (IRQ gate)
    present = 1;
}

void serial_enable_input(void) {
    if (!present) return;
    irq_register(IRQ_SERIAL1, serial_irq);
    outb(COM1 + 1, 0x01);  // interrupt on received data
}

void serial_putc(char c) {
    if (!present) return;
    for (int i = 0; i < 100000 && !(inb(COM1 + 5) & 0x20); i++) { }
    outb(COM1, (uint8_t)c);
}

void serial_write(const char* s, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) serial_putc(s[i]);
}
