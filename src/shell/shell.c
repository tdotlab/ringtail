#include <stdint.h>

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
extern int kfree(void* ptr);
extern uint32_t heap_get_free_blocks(void);
extern uint32_t heap_get_total(void);
extern void diskfs_list(void);
extern int diskfs_create(const char* name);
extern int diskfs_write(const char* name, const char* content);
extern int diskfs_read(const char* name, char* out, uint32_t max);
extern int diskfs_delete(const char* name);
extern int diskfs_exists(const char* name);
extern uint32_t timer_get_ticks(void);
extern void task_list_tasks(void);
extern void faultlog_dump(void);
extern void mmap_dump(void);
extern int task_spawn_counter(void);
extern int task_spawn_busy(void);
extern int task_kill(int id);
extern int loader_run(const char* name);
extern int timer_sleep(uint32_t seconds);
extern void print_color(const char* s, unsigned char color);
extern void set_color(unsigned char c);
extern void vga_lock(void);
extern void vga_unlock(void);
extern int mouse_found(void);
extern int mouse_get_x(void);
extern int mouse_get_y(void);
extern int mouse_get_buttons(void);
extern uint32_t mouse_get_packets(void);
extern void gfx_demo(void);

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
// Real hex output. The old code printed decimal after a "0x" prefix,
// so addresses looked like hex but were not.
static void print_hex(uint32_t n) {
    char buf[9];
    int i = 0;
    print("0x");
    if (n == 0) { print_char('0'); return; }
    while (n > 0) {
        uint32_t d = n % 16;
        buf[i++] = (d < 10) ? ('0' + d) : ('a' + d - 10);
        n /= 16;
    }
    while (i > 0) {
        print_char(buf[--i]);
    }
}
// Strict parsers: the old atoi silently turned "abc" into 0 and
// stopped at trailing garbage, so "free abc" reported success and
// "sleep abc" slept zero seconds. Both return 0 on success, -1 on
// empty input, non-digit input, trailing garbage, or overflow.
static int parse_num(const char* s, uint32_t* out) {
    uint32_t n = 0;
    if (!*s) return -1;
    while (*s >= '0' && *s <= '9') {
        uint32_t d = *s - '0';
        if (n > (0xFFFFFFFFu - d) / 10) return -1;
        n = n * 10 + d;
        s++;
    }
    if (*s) return -1;
    *out = n;
    return 0;
}
static int parse_hex(const char* s, uint32_t* out) {
    uint32_t n = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    if (!*s) return -1;
    while (*s) {
        uint32_t d;
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
        else return -1;
        if (n > (0xFFFFFFFFu - d) / 16) return -1;
        n = n * 16 + d;
        s++;
    }
    *out = n;
    return 0;
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
    print("  free <hex addr> - free at addr (as printed by malloc)\n");
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
    print("  tasks - list tasks\n");
    print("  faults - show recent fault log\n");
    print("  mmap - show multiboot memory map\n");
    print("  spawn counter - start a counting task\n");
    print("  spawn busy - start a task that never yields\n");
    print("  kill <id> - stop a task\n");
    print("  run <name> - load and run a program from disk\n");
    print("  mouse - show mouse position and buttons\n");
    print("  gfx - graphics demo (move mouse, any key exits)\n");
}

static void cmd_version(void) {
    print("RingTail v0.10.4 - Heap from memory map\n");
}

static void cmd_mouse(void) {
    if (!mouse_found()) {
        print_color("Mouse: not found\n", 0x0C);
        return;
    }
    // Buttons as a bitmask, same bits the packet carries: 1 left,
    // 2 right, 4 middle. Packets counts every complete 3-byte
    // packet the handler assembled, so a rising count proves the
    // IRQ path is alive even before the cursor visibly moves.
    print("Mouse: PS/2, x=");
    print_num((uint32_t)mouse_get_x());
    print(" y=");
    print_num((uint32_t)mouse_get_y());
    print(" buttons=");
    print_num((uint32_t)mouse_get_buttons());
    print(" packets=");
    print_num(mouse_get_packets());
    print("\n");
}

static void cmd_heap(void) {
    print("Heap start: ");
    print_hex(heap_get_start());
    print("\nTotal: ");
    print_num(heap_get_total());
    print(" bytes\nUsed: ");
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
        diskfs_list();
    } else if (strncmp(buf, "touch ", 6) == 0) {
        char* name = buf+6;
        if (name[0] == 0) {
            print_color("Usage: touch <file>\n", 0x0C);
        } else {
            int r = diskfs_create(name);
            if (r == 0) {
                print_color("Created ", 0x0A);
                print(name);
                print("\n");
            } else if (r == -1) {
                print_color("Already exists\n", 0x0C);
            } else if (r == -2) {
                print_color("Name must fit 8.3 (e.g. DEMO.TXT)\n", 0x0C);
            } else if (r == -4) {
                print_color("No disk\n", 0x0C);
            } else {
                print_color("Failed\n", 0x0C);
            }
        }
    } else if (strncmp(buf, "cat ", 4) == 0) {
        char* name = buf+4;
        if (name[0] == 0) {
            print_color("Usage: cat <file>\n", 0x0C);
            return;
        }
        char out[256];
        int r = diskfs_read(name, out, 256);
        if (r>=0) {
            print(out);
            print("\n");
        } else if (r == -4) {
            print_color("No disk\n", 0x0C);
        } else {
            print_color("No such file\n", 0x0C);
        }
    } else if (strncmp(buf, "rm ", 3) == 0) {
        char* name = buf+3;
        if (name[0] == 0) {
            print_color("Usage: rm <file>\n", 0x0C);
        } else {
            int r = diskfs_delete(name);
            if (r == 0) {
                print_color("Deleted\n", 0x0A);
            } else if (r == -4) {
                print_color("No disk\n", 0x0C);
            } else {
                print_color("No such file\n", 0x0C);
            }
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
            if (fname[0] == 0) {
                print_color("Usage: write <file> <text>\n", 0x0C);
            } else {
                int r = diskfs_write(fname, text);
                if (r == 0) {
                    print_color("Wrote\n", 0x0A);
                } else if (r == -2) {
                    print_color("Disk full or write failed\n", 0x0C);
                } else if (r == -4) {
                    print_color("No disk\n", 0x0C);
                } else {
                    print_color("Failed\n", 0x0C);
                }
            }
        }
    } else if (strncmp(buf, "free ", 5) == 0) {
        uint32_t addr;
        if (parse_hex(buf+5, &addr) != 0) {
            print_color("Invalid address (hex, e.g. 0x10500c)\n", 0x0C);
        } else {
            int r = kfree((void*)addr);
            if (r == 0) {
                print("Freed ");
                print_hex(addr);
                print("\n");
            } else if (r == -2) {
                print_color("Already freed\n", 0x0C);
            } else {
                print_color("Invalid address\n", 0x0C);
            }
        }
    } else if (strncmp(buf, "malloc ", 7) == 0) {
        uint32_t sz;
        if (parse_num(buf+7, &sz) != 0) {
            print_color("Invalid size\n", 0x0C);
        } else if (sz == 0) {
            print_color("Cannot allocate 0 bytes\n", 0x0C);
        } else {
            void* p = kmalloc(sz);
            if (p) {
                print("Allocated ");
                print_num(sz);
                print(" bytes at ");
                print_hex((uint32_t)p);
                print("\n");
            } else {
                print_color("Out of memory!\n", 0x0C);
            }
        }
    } else if (strcmp(buf, "history") == 0) {
        cmd_history();
    } else if (strcmp(buf, "colors") == 0) {
        cmd_colors();
    } else if (strncmp(buf, "color ", 6) == 0) {
        cmd_color(buf + 6);
    } else if (strcmp(buf, "tasks") == 0) {
        task_list_tasks();
    } else if (strcmp(buf, "faults") == 0) {
        faultlog_dump();
    } else if (strcmp(buf, "mmap") == 0) {
        mmap_dump();
    } else if (strcmp(buf, "spawn counter") == 0) {
        int id = task_spawn_counter();
        if (id < 0) {
            print_color("Out of memory!\n", 0x0C);
        } else {
            print("Spawned counter as task ");
            print_num((uint32_t)id);
            print("\n");
        }
    } else if (strcmp(buf, "spawn busy") == 0) {
        int id = task_spawn_busy();
        if (id < 0) {
            print_color("Out of memory!\n", 0x0C);
        } else {
            print("Spawned busy as task ");
            print_num((uint32_t)id);
            print("\n");
        }
    } else if (strncmp(buf, "run ", 4) == 0) {
        int id = loader_run(buf + 4);
        if (id < 0) {
            print_color("Not found on disk or load failed\n", 0x0C);
        } else {
            print("Started ");
            print(buf + 4);
            print(" as task ");
            print_num((uint32_t)id);
            print("\n");
        }
    } else if (strncmp(buf, "kill ", 5) == 0) {
        uint32_t id;
        if (parse_num(buf + 5, &id) != 0) {
            print_color("Usage: kill <id>\n", 0x0C);
        } else if (task_kill((int)id) != 0) {
            print_color("No such task (or cannot kill task 0)\n", 0x0C);
        } else {
            print("Killed task ");
            print_num(id);
            print("\n");
        }
    } else if (strcmp(buf, "uptime") == 0) {
        cmd_uptime();
    } else if (strcmp(buf, "mouse") == 0) {
        cmd_mouse();
    } else if (strcmp(buf, "gfx") == 0) {
        // Same lock rule as sleep: the demo runs for an
        // indefinite time, and holding the print lock across it
        // would stall every other task's output until the user
        // presses a key. Release it, run the demo (which draws
        // to the framebuffer, not the text screen, so it needs
        // no lock), take it again before returning.
        vga_unlock();
        gfx_demo();
        vga_lock();
    } else if (strcmp(buf, "tick") == 0) {
        cmd_tick();
    } else if (strncmp(buf, "sleep ", 6) == 0) {
        uint32_t secs;
        if (parse_num(buf + 6, &secs) != 0) {
            print_color("Usage: sleep <seconds>\n", 0x0C);
        } else {
            // v0.5.1: the print lock must NOT be held across the
            // wait. shell_enter holds it for the whole command, and
            // holding it here would stall the counter's prints for
            // the entire sleep (and, with a cli lock, would stop
            // ticks entirely and deadlock the sleep). Release it,
            // wait with ticks flowing and the counter free to
            // print, then take it again before reporting.
            vga_unlock();
            int r = timer_sleep(secs);
            vga_lock();
            if (r != 0) {
                print_color("Duration too large (max 21474836 seconds)\n", 0x0C);
            } else {
                print("Slept ");
                print_num(secs);
                print(" seconds\n");
            }
        }
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
    // v0.5.1: one lock for a whole command's output, from the
    // newline before it to the next prompt. The counter cannot
    // print in the middle of a command's output anymore, only
    // between commands. The sleep branch inside shell_execute
    // temporarily releases this lock (see there) and takes it
    // again, so the count stays balanced at 1 on entry and exit
    // here. Early returns inside shell_execute do not unlock:
    // they return to here, and this unlock covers them.
    vga_lock();
    print_char('\n');
    shell_execute();
    pos = 0;
    buf[0] = 0;
    print_color("ringtail> ", 0x0A);
    vga_unlock();
}
