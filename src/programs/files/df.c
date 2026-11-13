// df [-h] [-k] [FILE...]    free space on the disk
//
// RingTail has one filesystem, so there is one row; a FILE just has to
// exist, and the row is the same. Sizes are in 1K blocks, or with -h
// in the largest unit that keeps the number short (K, M, G, rounded
// up, like GNU df). -k is the default, spelled out for scripts.
#include "ulib.h"

static int usage(void) {
    dprintf(STDERR_FILENO, "usage: df [-h] [-k] [FILE...]\n");
    return 2;
}

static void human(char* out, size_t size, uint32_t kb) {
    uint32_t v = kb;
    char unit = 'K';
    if (v >= 1024 * 1024) { v = (kb + 1024 * 1024 - 1) / (1024 * 1024); unit = 'G'; }
    else if (v >= 1024) { v = (kb + 1023) / 1024; unit = 'M'; }
    snprintf(out, size, "%u%c", v, unit);
}

// "N KB free of M KB": the two numbers. 0 if that is what it says.
static int numbers(const char* s, uint32_t* a, uint32_t* b) {
    uint32_t v[2] = { 0, 0 };
    for (int k = 0; k < 2; k++) {
        while (*s && (*s < '0' || *s > '9')) s++;
        if (!*s) return -1;
        while (*s >= '0' && *s <= '9') v[k] = v[k] * 10 + (uint32_t)(*s++ - '0');
    }
    *a = v[0];
    *b = v[1];
    return 0;
}

int main(int argc, char** argv) {
    int hum = 0, a = 1;
    for (; a < argc && argv[a][0] == '-' && argv[a][1]; a++) {
        if (!strcmp(argv[a], "--")) { a++; break; }
        for (const char* o = argv[a] + 1; *o; o++) {
            if (*o == 'h') hum = 1;
            else if (*o == 'k') hum = 0;
            else return usage();
        }
    }
    int bad = 0;
    for (int i = a; i < argc; i++) {
        dirent_t d;
        int r = sys_stat(argv[i], &d);
        if (r < 0) {
            dprintf(STDERR_FILENO, "df: %s: %s\n", argv[i], strerror(r));
            bad = 1;
        }
    }
    if (bad) return 1;

    char text[64];
    uint32_t free_kb, total_kb;
    if (sys_kinfo(KINFO_DISK, text, sizeof text) < 0 || numbers(text, &free_kb, &total_kb)) {
        dprintf(STDERR_FILENO, "df: no disk\n");
        return 1;
    }
    uint32_t used = total_kb - free_kb;
    uint32_t pct = total_kb ? (uint32_t)(((uint64_t)used * 100 + total_kb - 1) / total_kb) : 0;
    char b1[16], b2[16], b3[16];
    if (hum) {
        human(b1, sizeof b1, total_kb);
        human(b2, sizeof b2, used);
        human(b3, sizeof b3, free_kb);
    } else {
        snprintf(b1, sizeof b1, "%u", total_kb);
        snprintf(b2, sizeof b2, "%u", used);
        snprintf(b3, sizeof b3, "%u", free_kb);
    }
    printf("Filesystem  %9s %9s %9s  Use%% Mounted on\n", hum ? "Size" : "1K-blocks", "Used", "Available");
    printf("disk        %9s %9s %9s %4u%% /\n", b1, b2, b3, pct);
    return 0;
}
