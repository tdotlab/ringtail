#include <stdint.h>

// RingTail - Piece 6.5: Shell with history
 // Buffer + backspace + enter + command history (up/down)

static char buf[256];
static int pos = 0;

// history - last 10 commands
static char history[10][256];
static int hist_count = 0; // how many saved
static int hist_nav = -1;  // -1 = not browsing, else index in history
static char temp_buf[256]; // save current line when you start browsing
static int temp_pos = 0;

extern void print(const char* s);
extern void print_char(char c);
extern void delete_char(void);
extern void clear_screen(void);
extern void* kmalloc(uint32_t size);
extern uint32_t heap_get_used(void);
extern uint32_t heap_get_free(void);
extern uint32_t heap_get_start(void);
extern void kfree(void* ptr);
extern uint32_t heap_get_free_blocks(void);
extern void ramfs_list(void);
extern int ramfs_create(const char* name);
extern int ramfs_write(const char* name, const char* content);
extern int ramfs_read(const char* name, char* out, uint32_t max);
extern int ramfs_delete(const char* name);
extern int ramfs_exists(const char* name);
extern uint32_t timer_get_ticks(void);
extern void timer_sleep(uint32_t seconds);
extern void print_color(const char* s, unsigned char color);
extern void set_color(unsigned char c);

static int strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}
static int strncmp(const char* a, const char* b, int n) {
    while (n && *a && *a == *b) { a++; b++; n--; }
    if (n == 0) return 0;
    return (unsigned char)*a - (unsigned char)*b;
}
static void strcpy(char* dst, const char* src) {
    while ((*dst++ = *src++));
}

void shell_init(void) {
    pos = 0;
    buf[0] = 0;
    hist_count = 0;
    hist_nav = -1;
}

void shell_putchar(char c) {
    if (pos < 255) {
        buf[pos++] = c;
        buf[pos] = 0;
        print_char(c);
        hist_nav = -1; // reset history browsing when you type
    }
}

void shell_backspace(void) {
    if (pos > 0) {
        pos--;
        buf[pos] = 0;
        delete_char();
    }
    hist_nav = -1;
}

static void clear_current_line(void) {
    while (pos > 0) {
        pos--;
        delete_char();
    }
    buf[0] = 0;
}

static void load_history_entry(int idx) {
    clear_current_line();
    strcpy(buf, history[idx]);
    // calculate pos
    pos = 0;
    while (buf[pos]) pos++;
    print(buf);
}

void shell_history_prev(void) {
    if (hist_count == 0) return;
    if (hist_nav == -1) {
        // save what you're currently typing
        strcpy(temp_buf, buf);
        temp_pos = pos;
        hist_nav = hist_count - 1;
    } else if (hist_nav > 0) {
        hist_nav--;
    } else {
        return; // at oldest
    }
    load_history_entry(hist_nav);
}

void shell_history_next(void) {
    if (hist_nav == -1) return;
    if (hist_nav < hist_count - 1) {
        hist_nav++;
        load_history_entry(hist_nav);
    } else {
        // back to current (empty or what you were typing)
        clear_current_line();
        strcpy(buf, temp_buf);
        pos = temp_pos;
        print(buf);
        hist_nav = -1;
    }
}

static void save_to_history(void) {
    if (pos == 0) return;
    // don't save duplicates of last entry
    if (hist_count > 0 && strcmp(buf, history[hist_count-1]) == 0) return;

    if (hist_count < 10) {
        strcpy(history[hist_count++], buf);
    } else {
        // shift left, drop oldest
        for (int i = 0; i < 9; i++) strcpy(history[i], history[i+1]);
        strcpy(history[9], buf);
    }
    hist_nav = -1;
}


static void print_num(uint32_t n) {
    char buf[12];
    int i = 0;
    if (n == 0) { print_char('0'); return; }
    while (n > 0) {
        buf[i++] = '0' + (n % 10);
        n /= 10;
    }
    while (i > 0) {
        print_char(buf[--i]);
    }
}
static uint32_t atoi(const char* s) {
    uint32_t n = 0;
    while (*s >= '0' && *s <= '9') {
        n = n * 10 + (*s - '0');
        s++;
    }
    return n;
}

static void cmd_color(const char* name) {
    unsigned char fg;
    if (strcmp(name, "blue") == 0) fg = 0x01;
    else if (strcmp(name, "green") == 0) fg = 0x02;
    else if (strcmp(name, "cyan") == 0) fg = 0x03;
    else if (strcmp(name, "red") == 0) fg = 0x04;
    else if (strcmp(name, "magenta") == 0) fg = 0x05;
    else if (strcmp(name, "yellow") == 0) fg = 0x0E;
    else if (strcmp(name, "gray") == 0) fg = 0x07;
    else if (strcmp(name, "lightgreen") == 0) fg = 0x0A;
    else if (strcmp(name, "lightred") == 0) fg = 0x0C;
    else if (strcmp(name, "lightcyan") == 0) fg = 0x0B;
    else if (strcmp(name, "white") == 0) fg = 0x0F;
    else {
        print_color("Unknown color. Try: blue green cyan red magenta yellow gray lightgreen lightred lightcyan white\n", 0x0C);
        return;
    }
    set_color(fg);
    print_color("Color set\n", fg);
}

static void cmd_colors(void) {
    print_color("blue\n", 0x01);
    print_color("green\n", 0x02);
    print_color("cyan\n", 0x03);
    print_color("red\n", 0x04);
    print_color("magenta\n", 0x05);
    print_color("yellow\n", 0x0E);
    print_color("gray\n", 0x07);
    print_color("lightgreen\n", 0x0A);
    print_color("lightred\n", 0x0C);
    print_color("lightcyan\n", 0x0B);
    print_color("white\n", 0x0F);
}

static void cmd_uptime(void) {
    print("Uptime: ");
    print_num(timer_get_ticks() / 100);
    print(" seconds\n");
}

static void cmd_tick(void) {
    print("Ticks: ");
    print_num(timer_get_ticks());
    print("\n");
}

static void cmd_help(void) {
    print("Commands:\n");
    print("  help    - this list\n");
    print("  clear   - clear screen\n");
    print("  version - show RingTail version\n");
    print("  echo <text>\n");
    print("  heap      - show heap stats\n");
    print("  malloc <n> - alloc n bytes\n");
    print("  free <addr> - free at addr\n");
    print("  history - show last commands\n");
    print("  ls - list files\n");
    print("  touch <file> - create file\n");
    print("  cat <file> - show file\n");
    print("  write <file> <text> - write to file\n");
    print("  rm <file> - delete file\n");
    print_color("  color <name> - set text color\n", 0x0B);
    print_color("  colors - show palette\n", 0x0B);
    print("  uptime - show seconds since boot\n");
    print("  tick   - show raw timer ticks (100 per second)\n");
    print("  sleep <seconds> - wait\n");
}

static void cmd_version(void) {
    print("RingTail v0.2.2 - Timer\n");
}

static void cmd_heap(void) {
    print("Heap start: 0x");
    print_num(heap_get_start());
    print("\nUsed: ");
    print_num(heap_get_used());
    print(" bytes\nFree: ");
    print_num(heap_get_free());
    print(" bytes\nFree blocks: ");
    print_num(heap_get_free_blocks());
    print("\n");
}


static char* find_space(char* s) {
    while (*s && *s != ' ') s++;
    return *s==' ' ? s : 0;
}

static void cmd_history(void) {
    for (int i = 0; i < hist_count; i++) {
        print(history[i]);
        print("\n");
    }
}


void shell_execute(void) {
    if (pos == 0) return;
    save_to_history();

    if (strcmp(buf, "help") == 0) {
        cmd_help();
    } else if (strcmp(buf, "clear") == 0) {
        clear_screen();
    } else if (strcmp(buf, "version") == 0) {
        cmd_version();
    } else if (strcmp(buf, "heap") == 0) {
        cmd_heap();
    } else if (strcmp(buf, "ls") == 0) {
        ramfs_list();
    } else if (strncmp(buf, "touch ", 6) == 0) {
        char* name = buf+6;
        if (ramfs_create(name)==0) {
            print_color("Created ", 0x0A);
            print(name);
            print("\n");
        } else {
            print_color("Failed (exists?)\n", 0x0C);
        }
    } else if (strncmp(buf, "cat ", 4) == 0) {
        char* name = buf+4;
        char out[256];
        int r = ramfs_read(name, out, 256);
        if (r>=0) {
            print(out);
            print("\n");
        } else {
            print_color("No such file\n", 0x0C);
        }
    } else if (strncmp(buf, "rm ", 3) == 0) {
        char* name = buf+3;
        if (ramfs_delete(name)==0) {
            print_color("Deleted\n", 0x0A);
        } else {
            print_color("No such file\n", 0x0C);
        }
    } else if (strncmp(buf, "write ", 6) == 0) {
        char* rest = buf+6;
        char* sp = find_space(rest);
        if (!sp) {
            print_color("Usage: write <file> <text>\n", 0x0C);
        } else {
            *sp=0;
            char* fname = rest;
            char* text = sp+1;
            if (ramfs_write(fname, text)==0) {
                print_color("Wrote\n", 0x0A);
            } else {
                print_color("Failed\n", 0x0C);
            }
        }
    } else if (strncmp(buf, "free ", 5) == 0) {
        uint32_t addr = atoi(buf+5);
        kfree((void*)addr);
        print("Freed 0x");
        print_num(addr);
        print("\n");
    } else if (strncmp(buf, "malloc ", 7) == 0) {
        uint32_t sz = atoi(buf+7);
        void* p = kmalloc(sz);
        if (p) {
            print("Allocated ");
            print_num(sz);
            print(" bytes at 0x");
            print_num((uint32_t)p);
            print("\n");
        } else {
            print_color("Out of memory!\n", 0x0C);
        }
    } else if (strcmp(buf, "history") == 0) {
        cmd_history();
    } else if (strcmp(buf, "colors") == 0) {
        cmd_colors();
    } else if (strncmp(buf, "color ", 6) == 0) {
        cmd_color(buf + 6);
    } else if (strcmp(buf, "uptime") == 0) {
        cmd_uptime();
    } else if (strcmp(buf, "tick") == 0) {
        cmd_tick();
    } else if (strncmp(buf, "sleep ", 6) == 0) {
        uint32_t secs = atoi(buf + 6);
        timer_sleep(secs);
        print("Slept ");
        print_num(secs);
        print(" seconds\n");
    } else if (strncmp(buf, "echo ", 5) == 0) {
        print(buf + 5);
        print("\n");
    } else {
        print_color("unknown: ", 0x0C);
        print(buf);
        print("\n");
    }
}

void shell_enter(void) {
    print_char('\n');
    shell_execute();
    pos = 0;
    buf[0] = 0;
    print_color("ringtail> ", 0x0A);
}
