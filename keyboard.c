// RingTail - Piece 6.5: Keyboard with history navigation

#include <stdint.h>

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static char scancode_to_char[128] = {
    0,  27, '1','2','3','4','5','6','7','8','9','0','-','=', '\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n', 0,
    'a','s','d','f','g','h','j','k','l',';','\'','`', 0,
    '\\','z','x','c','v','b','n','m',',','.','/', 0,
    '*', 0, ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

extern void shell_putchar(char c);
extern void shell_backspace(void);
extern void shell_enter(void);
extern void shell_history_prev(void);
extern void shell_history_next(void);

void keyboard_handler(void) {
    uint8_t scancode = inb(0x60);
    if (scancode & 0x80) {
        // key release
    } else {
        if (scancode == 72) { // up arrow
            shell_history_prev();
        } else if (scancode == 80) { // down arrow
            shell_history_next();
        } else {
            char c = scancode_to_char[scancode];
            if (c == '\n') {
                shell_enter();
            } else if (c == '\b') {
                shell_backspace();
            } else if (c != 0) {
                shell_putchar(c);
            }
        }
    }
    outb(0x20, 0x20);
}
