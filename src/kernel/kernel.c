#include <stdint.h>

 // No more clear_screen on overflow - we scroll like a real terminal

static volatile unsigned short* const VGA = (volatile unsigned short*)0xB8000;
static const int WIDTH = 80;
static const int HEIGHT = 25;
static int cx = 0;
static int cy = 0;

#define COLOR_DEFAULT 0x0F
#define COLOR_PROMPT  0x0A
#define COLOR_TITLE   0x0B
#define COLOR_DIM     0x08

static unsigned char cur_color = COLOR_DEFAULT;

void set_color(unsigned char c) { cur_color = c; }
unsigned char get_color(void) { return cur_color; }

static unsigned short entry(char c, unsigned char color) {
    return (unsigned short)color << 8 | (unsigned short)c;
}

static void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static void update_cursor(void) {
    int pos = cy * WIDTH + cx;
    outb(0x3D4, 14);
    outb(0x3D5, pos >> 8);
    outb(0x3D4, 15);
    outb(0x3D5, pos & 0xFF);
}

static void scroll(void) {
    // move every line up by one
    for (int y = 0; y < HEIGHT - 1; y++) {
        for (int x = 0; x < WIDTH; x++) {
            VGA[y * WIDTH + x] = VGA[(y+1) * WIDTH + x];
        }
    }
    // clear last line
    for (int x = 0; x < WIDTH; x++) {
        VGA[(HEIGHT-1) * WIDTH + x] = entry(' ', 0x0F);
    }
    cy = HEIGHT - 1;
}

void clear_screen(void) {
    cur_color = COLOR_DEFAULT;
    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            VGA[y * WIDTH + x] = entry(' ', 0x0F);
        }
    }
    cx = 0;
    cy = 0;
    update_cursor();
}

void print_char(char c) {
    if (c == '\n') {
        cx = 0;
        cy++;
        if (cy >= HEIGHT) scroll();
        update_cursor();
        return;
    }
    VGA[cy * WIDTH + cx] = entry(c, cur_color);
    cx++;
    if (cx >= WIDTH) { cx = 0; cy++; }
    if (cy >= HEIGHT) scroll();
    update_cursor();
}

void delete_char(void) {
    if (cx == 0 && cy == 0) return;
    if (cx > 0) {
        cx--;
    } else {
        cy--;
        cx = WIDTH - 1;
    }
    VGA[cy * WIDTH + cx] = entry(' ', COLOR_DEFAULT);
    update_cursor();
}

void print(const char* s) {
    for (int i = 0; s[i] != 0; i++) print_char(s[i]);
}

void print_color(const char* s, unsigned char color) {
    unsigned char old = cur_color;
    cur_color = color;
    print(s);
    cur_color = old;
}

// v0.5.1: the VGA print lock. print() itself does NOT lock: a
// counter line is three separate print calls ("counter: ", the
// number, "\n"), and locking per call would still let another task
// print its line between them. Instead, a logical output unit
// locks once, prints everything, and unlocks: the counter locks
// around its whole line, the shell locks around a whole command's
// output. That makes command output atomic, which is what the
// v0.5.0 interleaving ("tacounter: 5") was missing.
// Why a spinlock and not cli/sti: the shell's sleep command must
// NOT hold the lock while it waits (ticks have to keep flowing,
// and the counter has to be able to print during the sleep), so
// the sleep branch releases the lock before waiting and takes it
// again after. A cli critical section held across a sleep would
// mask the timer and deadlock the sleep. A spinlock lets the
// holder be preempted: a task spinning on the lock gets switched
// away on the next tick, the holder runs and releases, and the
// spinner then acquires. No deadlock, as long as the holder
// always runs to its unlock, which both holders here do.
// Why recursive (owner + count): print_color calls print, and the
// shell's sleep branch unlocks and relocks inside an outer lock,
// so the same task must be able to acquire twice and release in
// matching pairs. A plain flag would deadlock on the second
// acquire, because the task would spin waiting for itself.
// Typed input echo is deliberately NOT locked: shell_putchar
// prints one character at a time as keys arrive, and holding the
// lock across a whole typing session would stall the counter for
// as long as the user pauses between keys. Typed characters can
// still interleave with counter output, and that is normal
// terminal behavior (real terminals do this too); the shell still
// reads the typed command correctly, only its on-screen echo is
// mixed with output. Command output, by contrast, is atomic now.
extern int task_current_id(void);

static volatile int vga_lock_flag = 0;
static volatile int vga_lock_owner = -1;
static volatile int vga_lock_count = 0;

void vga_lock(void) {
    int me = task_current_id();
    if (vga_lock_owner == me) { vga_lock_count++; return; }
    while (1) {
        int old;
        // xchg is atomic: exactly one task sees the old value 0
        // and becomes the holder; the rest see 1 and keep spinning.
        __asm__ volatile("xchg %0, %1" : "=r"(old), "+m"(vga_lock_flag) : "0"(1));
        if (old == 0) break;
        __asm__ volatile("pause");
    }
    vga_lock_owner = me;
    vga_lock_count = 1;
}

void vga_unlock(void) {
    int me = task_current_id();
    if (vga_lock_owner != me) return; // never release someone else's lock
    if (--vga_lock_count == 0) {
        vga_lock_owner = -1;
        __asm__ volatile("" ::: "memory");
        vga_lock_flag = 0;
    }
}

// Fatal-path escape hatch: if a task faults in the middle of a
// print, the exception handler would spin forever waiting for a
// lock held by a task that will never run again. Breaking the
// lock first lets the diagnostic print, and the system halts
// right after anyway, so the broken lock state does not matter.
void vga_lock_break(void) {
    vga_lock_owner = -1;
    vga_lock_count = 0;
    vga_lock_flag = 0;
}

extern void idt_init(void);
extern void heap_init(void);
extern uint32_t heap_get_start(void);
extern uint32_t heap_get_free(void);
extern void shell_init(void);
extern void ramfs_init(void);
extern void timer_init(void);
extern int keyboard_poll(void);
extern void keyboard_process_scancode(uint8_t scancode);
extern void task_init(void);
extern void task_cleanup(void);
extern void task_yield(void);

// v0.2.3: kernel_main now takes the multiboot magic and info pointer
// from boot.asm (cdecl: first two stack arguments), so it can verify
// it was really loaded by a multiboot loader and that there is enough
// RAM for the 4 MB heap, instead of assuming both.
void kernel_main(uint32_t magic, uint32_t mbi) {
    // enable cursor: set scanlines 0-15
    outb(0x3D4, 10);
    outb(0x3D5, 0);
    outb(0x3D4, 11);
    outb(0x3D5, 15);

    clear_screen();
    if (magic != 0x2BADB002) {
        print_color("Bad multiboot magic - halting.\n", 0x0C);
        while (1) __asm__ volatile("hlt");
    }
    if (mbi != 0) {
        uint32_t* info = (uint32_t*)mbi;
        if (info[0] & 0x1) {
            // info[2] is mem_upper: KB of RAM above 1 MB. The heap
            // needs 4 MB, plus the kernel itself above 1 MB, so
            // require at least 5 MB above 1 MB. If the flags bit is
            // clear the size is unknown and we skip the check.
            if (info[2] < 5120) {
                print_color("Not enough RAM for 4 MB heap - halting.\n", 0x0C);
                while (1) __asm__ volatile("hlt");
            }
        }
    }
    print_color("RingTail v0.5.1 - Preemption hardening\n", COLOR_TITLE);
    print_color("RAM filesystem, files in heap\n", COLOR_DIM);
    print("--------------\n");

    heap_init();
    ramfs_init();
    timer_init();
    task_init();
    shell_init();
    print_color("ringtail> ", COLOR_PROMPT);
    update_cursor();

    idt_init();
    __asm__ volatile("sti");

    // Main loop (v0.5.0): task 0. Drain the input queue, free dead
    // tasks, run the next ready task if there is one (task_yield
    // returns at once if there is not), then halt until the next
    // interrupt. The timer also preempts this loop on every tick,
    // so tasks run even while the main loop is halted or inside a
    // shell sleep; the PIT ticks every 10 ms and wakes the halt
    // regardless, so a sleeping task is always reconsidered promptly.
    while (1) {
        int sc;
        while ((sc = keyboard_poll()) >= 0) {
            keyboard_process_scancode((uint8_t)sc);
        }
        task_cleanup();
        task_yield();
        __asm__ volatile("hlt");
    }
}
