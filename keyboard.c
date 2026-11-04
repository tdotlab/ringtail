// RingTail - Piece 6.5: Keyboard with history navigation
// v0.3.0: the interrupt handler no longer runs the shell. It only
// reads the scancode, enqueues it, and sends EOI. The kernel main
// loop dequeues scancodes and calls keyboard_process_scancode, so
// file operations, allocations, and terminal output all happen in
// normal kernel context, not interrupt context. This is the
// foundation a scheduler needs: before this change, the shell ran
// inside the keyboard ISR, and sleep needed an sti/hlt/cli dance
// plus a key-drop guard to avoid re-entering the shell.
//
// Queue discipline: key_head is written only by the ISR, key_tail
// only by the main loop, so no lock is needed. Head and tail are
// 32-bit counters masked to 256 entries; their difference, in
// unsigned arithmetic, is the queue depth. A full queue drops new
// keys rather than overwriting queued ones. A main loop that
// drains the queue and then halts cannot deadlock on a lost
// wakeup: the PIT ticks every 10 ms and wakes it regardless.

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

#define KEY_QUEUE_SIZE 256

static volatile uint8_t key_queue[KEY_QUEUE_SIZE];
static volatile uint32_t key_head = 0; // ISR only
static volatile uint32_t key_tail = 0; // main loop only

// Called from keyboard_isr (isr.asm). Keep it short: read, enqueue,
// acknowledge. Never call the shell from here.
void keyboard_handler(void) {
    uint8_t scancode = inb(0x60);
    if ((key_head - key_tail) < KEY_QUEUE_SIZE) {
        key_queue[key_head & (KEY_QUEUE_SIZE - 1)] = scancode;
        key_head++;
    }
    // else: queue full, drop the key rather than overwriting one
    outb(0x20, 0x20);
}

// Called from the kernel main loop. Returns -1 if the queue is
// empty, otherwise the next scancode.
int keyboard_poll(void) {
    if (key_tail == key_head) return -1;
    uint8_t scancode = key_queue[key_tail & (KEY_QUEUE_SIZE - 1)];
    key_tail++;
    return scancode;
}

extern void shell_putchar(char c);
extern void shell_backspace(void);
extern void shell_enter(void);
extern void shell_history_prev(void);
extern void shell_history_next(void);

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

// Runs in the kernel main loop, not in interrupt context. This is
// the v0.2.3 scancode state machine, unchanged in behavior: E0
// extended keys, shift (left/right) and caps lock, tab ignored.
void keyboard_process_scancode(uint8_t scancode) {
    static int ext = 0;         // previous byte was 0xE0
    static int shift_left = 0;  // left shift held
    static int shift_right = 0; // right shift held
    static int caps = 0;        // caps lock toggled

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
        return;
    }

    if (scancode == 0xE0) {
        ext = 1;
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
}
