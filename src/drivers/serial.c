// COM1 serial port: a second console. Output mirrors the screen;
// input feeds the same terminal queue as the keyboard, translating
// what terminals send (CR for Enter, DEL for backspace, ANSI escape
// sequences for the arrow keys).
#include "kernel.h"
#include "arch.h"
#include "interrupt.h"
#include "drivers.h"
#include "tty.h"

#define COM1 0x3F8

static int present;

int serial_present(void) { return present; }

static void serial_irq(void) {
    static int esc;       // 0 none, 1 after ESC, 2 after ESC [
    static int last_cr;
    while (inb(COM1 + 5) & 0x01) {
        int c = inb(COM1);
        if (esc == 1) {
            if (c == '[') { esc = 2; continue; }
            esc = 0;
            tty_input(0x1B);
        } else if (esc == 2) {
            esc = 0;
            if (c == 'A') tty_input(KEY_UP);
            else if (c == 'B') tty_input(KEY_DOWN);
            else if (c == 'C') tty_input(KEY_RIGHT);
            else if (c == 'D') tty_input(KEY_LEFT);
            continue;
        }
        if (c == 0x1B) { esc = 1; continue; }
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
