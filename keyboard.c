// RingTail - Piece 6.5: Keyboard with history navigation
// v0.2.3: real scancode state machine. The old code treated any byte
// with the high bit set as a release and everything else as a key,
// which mishandled the 0xE0 extended prefix (arrows and friends) and
// had no shift or caps lock at all.

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
extern volatile int timer_sleeping;

// Shifted symbol for a non-letter key (digits and punctuation).
static char shift_char(char c) {
    switch (c) {
        case '1': return '!'; case '2': return '@'; case '3': return '#';
        case '4': return '$'; case '5': return '%'; case '6': return '^';
        case '7': return '&'; case '8': return '*'; case '9': return '(';
        case '0': return ')'; case '-': return '_'; case '=': return '+';
        case '[': return '{'; case ']': return '}'; case ';': return ':';
        case '\'': return '"'; case '`': return '~'; case '\\': return '|';
        case ',': return '<'; case '.': return '>'; case '/': return '?';
        default: return c;
    }
}

void keyboard_handler(void) {
    static int ext = 0;         // previous byte was 0xE0
    static int shift_left = 0;  // left shift held
    static int shift_right = 0; // right shift held
    static int caps = 0;        // caps lock toggled

    uint8_t scancode = inb(0x60);
    if (timer_sleeping) {
        // Sleeping: drop the key so it cannot re-enter the shell,
        // which is already running inside this interrupt handler.
        outb(0x20, 0x20);
        return;
    }

    if (ext) {
        // Extended key (arrows, etc.): second byte, or second byte
        // with the high bit set for a release. Releases are ignored;
        // left/right are ignored too, the shell has no mid-line
        // cursor yet, so there is nothing sensible to do with them.
        ext = 0;
        if (!(scancode & 0x80)) {
            if (scancode == 72) shell_history_prev();
            else if (scancode == 80) shell_history_next();
        }
        outb(0x20, 0x20);
        return;
    }

    if (scancode == 0xE0) {
        ext = 1;
        outb(0x20, 0x20);
        return;
    }

    if (scancode & 0x80) {
        // Key release: only shift state changes.
        uint8_t make = scancode & 0x7F;
        if (make == 42) shift_left = 0;
        else if (make == 54) shift_right = 0;
    } else if (scancode == 42) {
        shift_left = 1;
    } else if (scancode == 54) {
        shift_right = 1;
    } else if (scancode == 58) {
        caps = !caps; // caps lock toggles on press, not on release
    } else if (scancode == 72) {
        shell_history_prev(); // keypad up
    } else if (scancode == 80) {
        shell_history_next(); // keypad down
    } else {
        char c = scancode_to_char[scancode];
        if (c == '\n') {
            shell_enter();
        } else if (c == '\b') {
            shell_backspace();
        } else if (c == '\t') {
            // Ignored: no tab completion yet, and printing a tab
            // wrote a raw VGA glyph instead of moving the cursor.
        } else if (c != 0) {
            int shift = shift_left || shift_right;
            if (c >= 'a' && c <= 'z') {
                if (shift ^ caps) c = (char)(c - 'a' + 'A');
            } else if (shift) {
                c = shift_char(c);
            }
            shell_putchar(c);
        }
    }
    outb(0x20, 0x20);
}
