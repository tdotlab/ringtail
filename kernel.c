#include <stdint.h>

// RingTail - Piece 7: Scrolling + cursor
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

extern void idt_init(void);
extern void heap_init(void);
extern uint32_t heap_get_start(void);
extern uint32_t heap_get_free(void);
extern void shell_init(void);
extern void ramfs_init(void);
extern void timer_init(void);

void kernel_main(void) {
    // enable cursor: set scanlines 0-15
    outb(0x3D4, 10);
    outb(0x3D5, 0);
    outb(0x3D4, 11);
    outb(0x3D5, 15);

    clear_screen();
    print_color("RingTail v0.2.2 - Timer\n", COLOR_TITLE);
    print_color("RAM filesystem, files in heap\n", COLOR_DIM);
    print("--------------\n");

    heap_init();
    ramfs_init();
    timer_init();
    shell_init();
    print_color("ringtail> ", 0x0A);
    update_cursor();

    idt_init();
    __asm__ volatile("sti");

    while(1) __asm__ volatile("hlt");
}
