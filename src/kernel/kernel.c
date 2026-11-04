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

// v0.9.0: text cursor save/restore for graphics mode. The
// graphics save captures video memory and VGA registers, but
// the print cursor lives in these two static variables, not in
// a register, so a text print during graphics mode (a fault
// diagnostic, or a program printing while it owns the display)
// would advance them while the restore reverted the screen,
// and the next typed command would land on a fresh line with no
// prompt in front of it. Saving them on entry and restoring
// them on exit keeps the cursor and the restored screen in
// agreement, and discards invisible prints along with the
// invisible screen they wrote to.
void text_cursor_get(int* x, int* y) {
    *x = cx;
    *y = cy;
}
void text_cursor_set(int x, int y) {
    cx = x;
    cy = y;
    update_cursor();
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

// v0.6.0: the kernel GDT and the TSS. Boot's GDT (boot.asm) only
// has kernel code/data segments, enough to reach C. This GDT
// replaces it before interrupts start, adding user code/data
// segments and a TSS, which are the two pieces user mode needs.
// Segments, in plain terms: the CPU does not run code at a raw
// address, it runs it through a segment, and each segment carries
// a privilege level (DPL). Kernel segments are DPL 0, user
// segments are DPL 3. A task's privilege is the DPL of its code
// segment: CS 0x08 means ring 0, CS 0x1B means ring 3. Selectors
// are index*8 + requested privilege: kernel code index 1 -> 0x08,
// kernel data index 2 -> 0x10, user code index 3 with RPL 3 ->
// 0x1B, user data index 4 with RPL 3 -> 0x23, TSS index 5 -> 0x28.
// The TSS (Task State Segment) is a small CPU-owned record whose
// only job here is esp0/ss0: when a ring 3 task traps into ring 0
// (a syscall, a timer tick), the CPU has to switch stacks, because
// the user stack is not trusted for kernel work, and it finds the
// kernel stack pointer in esp0. That is why every context switch
// updates esp0 to the incoming task's kernel stack top: without
// it, a trap from user task B would land on user task A's kernel
// stack. TSS layout used here (offsets in bytes): esp0 at 4, ss0
// at 8, iomap base at 102. Everything else is zero, and iomap
// base = 104 (the TSS size) means "no IO permission bitmap", so
// all port IO from ring 3 is denied, as it should be.
extern void gdt_load(uint32_t ptr);

static uint64_t gdt[6];
static uint8_t tss[104];

struct gdt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

static void gdt_set_entry(int index, uint32_t base, uint32_t limit, uint8_t access, uint8_t flags) {
    gdt[index] = (uint64_t)(limit & 0xFFFF)
        | ((uint64_t)(base & 0xFFFF) << 16)
        | ((uint64_t)((base >> 16) & 0xFF) << 32)
        | ((uint64_t)access << 40)
        | ((uint64_t)((limit >> 16) & 0xF) << 48)
        | ((uint64_t)(flags & 0xF) << 52)
        | ((uint64_t)((base >> 24) & 0xFF) << 56);
}

void tss_set_esp0(uint32_t esp0) {
    *(uint32_t*)(tss + 4) = esp0;
}

void gdt_init(void) {
    gdt_set_entry(0, 0, 0, 0, 0);                       // null
    gdt_set_entry(1, 0, 0xFFFFF, 0x9A, 0xC);             // kernel code, DPL 0
    gdt_set_entry(2, 0, 0xFFFFF, 0x92, 0xC);             // kernel data, DPL 0
    gdt_set_entry(3, 0, 0xFFFFF, 0xFA, 0xC);             // user code, DPL 3
    gdt_set_entry(4, 0, 0xFFFFF, 0xF2, 0xC);             // user data, DPL 3
    gdt_set_entry(5, (uint32_t)tss, 103, 0x89, 0x0);     // TSS
    for (int i = 0; i < 104; i++) tss[i] = 0;
    *(uint32_t*)(tss + 8) = 0x10; // ss0: kernel data selector
    *(uint16_t*)(tss + 102) = 104; // iomap base: no IO bitmap
    struct gdt_ptr ptr;
    ptr.limit = sizeof(gdt) - 1;
    ptr.base = (uint32_t)gdt;
    gdt_load((uint32_t)&ptr);
}

extern void idt_init(void);
extern void heap_init(void);
extern uint32_t heap_get_start(void);
extern uint32_t heap_get_free(void);
extern uint32_t heap_get_end(void);
extern void shell_init(void);
extern void diskfs_init(void);
extern void timer_init(void);
extern int mouse_init(void);
extern void paging_init(uint32_t heap_end, uint32_t ram_top);
extern int keyboard_poll(void);
extern int gfx_is_active(void);
extern void keyboard_process_scancode(uint8_t scancode);
extern void task_init(void);
extern void task_cleanup(void);
extern void task_yield(void);
extern void gdt_init(void);

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
    print_color("RingTail v0.10.2 - GUI launcher\n", COLOR_TITLE);
    print_color("Graphical desktop: run gui\n", COLOR_DIM);
    print("--------------\n");

    // v0.7.0: RAM top for the frame allocator. Multiboot info[2]
    // is KB above 1 MB; without it, assume 16 MB, which is what
    // the QEMU test machine has.
    uint32_t ram_top = 0x01000000u;
    if (mbi != 0) {
        uint32_t* info_top = (uint32_t*)mbi;
        if (info_top[0] & 0x1) ram_top = 0x100000u + info_top[2] * 1024u;
    }
    heap_init();
    // Paging before tasks: task_init needs the kernel page
    // directory for the main task, and no task may run before
    // its address space exists.
    paging_init(heap_get_end(), ram_top);
    diskfs_init();
    timer_init();
    // Mouse before tasks and before sti: init polls the controller
    // with interrupts off, and any packets the mouse sends in the
    // meantime simply wait in the controller until sti, when the
    // IRQ 12 handler (installed by idt_init below) is ready for
    // them. A missing mouse prints and continues, like no disk.
    if (mouse_init() == 0) {
        print_color("Mouse: PS/2 found\n", COLOR_DIM);
    } else {
        print_color("Mouse: not found\n", COLOR_DIM);
    }
    task_init();
    shell_init();
    print_color("ringtail> ", COLOR_PROMPT);
    update_cursor();

    // v0.6.0: install the kernel GDT (user segments + TSS) before
    // the IDT and before interrupts start. Interrupts are still
    // off here (boot did cli, sti comes below), so reloading the
    // segment registers cannot race a trap. Kernel selectors stay
    // 0x08/0x10, so the IDT gates, which reference 0x08, remain
    // valid after the reload.
    gdt_init();
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
        // v0.9.0: while a graphics program owns the display, the
        // keyboard belongs to it, not the shell. The main loop
        // leaves the queue alone, and the program drains it
        // through KEY_GET, which only the owner may call, so
        // there is exactly one drainer and no race on the queue
        // tail. Keys typed during the GUI stay queued for the
        // GUI; the shell never sees them, and the GUI drains the
        // queue each frame, so nothing is left for the shell
        // when it exits (except keys pressed after it died, which
        // are shell input and arrive after cleanup clears the
        // owner, in the right order).
        if (!gfx_is_active()) {
            while ((sc = keyboard_poll()) >= 0) {
                keyboard_process_scancode((uint8_t)sc);
            }
        }
        task_cleanup();
        task_yield();
        __asm__ volatile("hlt");
    }
}
