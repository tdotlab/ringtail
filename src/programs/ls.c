// ls [DIR]    list a directory (default: the current one)
//
// One line per entry: name, size or <dir>, and when it was written.
// On the screen directories are blue and a summary of the entries and
// the free disk space follows; into a pipe or a file there are only the
// lines, so wc and grep can count them.
#include "ulib.h"

#define BLU  "\x1b[94m"
#define DIM  "\x1b[90m"
#define NORM "\x1b[0m"

// The name column fits the longest name up to LS_WIDE; a longer one
// pushes its size along rather than widening every line.
#define LS_NARROW 13
#define LS_WIDE 32

// "2026-11-05 14:32" from the FAT words in a dirent, or a dash for an
// entry nothing ever stamped.
static void fmt_stamp(char* buf, size_t size, unsigned date, unsigned time) {
    if (!date) { strlcpy(buf, "-", size); return; }
    snprintf(buf, size, "%04u-%02u-%02u %02u:%02u", 1980 + (date >> 9), (date >> 5) & 15, date & 31,
             time >> 11, (time >> 5) & 63);
}

int main(int argc, char** argv) {
    int a = 1, dd = 0;
    if (a < argc && !strcmp(argv[a], "--")) a++, dd = 1;
    if (argc - a > 1 || (!dd && a < argc && argv[a][0] == '-' && argv[a][1])) {
        dprintf(STDERR_FILENO, "usage: ls [DIR]\n");
        return 2;
    }
    const char* dir = a < argc ? argv[a] : ".";
    int tty = sys_fdtype(STDOUT_FILENO) == FDTYPE_TTY;
    int cursor = 0, files = 0, dirs = 0, width = LS_NARROW, r;
    dirent_t d;
    while ((r = sys_readdir(dir, &cursor, &d)) > 0) {
        int n = (int)strlen(d.name) + ((d.flags & DIRENT_DIR) ? 1 : 0);
        if (n > width) width = n < LS_WIDE ? n : LS_WIDE;
    }
    cursor = 0;
    while (r >= 0 && (r = sys_readdir(dir, &cursor, &d)) > 0) {
        char stamp[24];
        fmt_stamp(stamp, sizeof stamp, d.mdate, d.mtime);
        if (d.flags & DIRENT_DIR) {
            char name[sizeof d.name + 1];
            snprintf(name, sizeof name, "%s/", d.name);
            printf("  %s%-*s%s %8s  %s\n", tty ? BLU : "", width, name, tty ? NORM : "", "<dir>", stamp);
            dirs++;
        } else {
            printf("  %-*s %8u  %s\n", width, d.name, d.size, stamp);
            files++;
        }
    }
    if (r < 0) {
        fs_error("ls", dir, r);
        return 1;
    }
    if (!tty) return 0;
    char disk[64];
    if (sys_kinfo(KINFO_DISK, disk, sizeof disk) < 0) {
        fs_error("ls", dir, -EIO);
        return 1;
    }
    printf(DIM "%d file%s, ", files, files == 1 ? "" : "s");
    if (dirs) printf("%d director%s, ", dirs, dirs == 1 ? "y" : "ies");
    printf("%s" NORM, disk);
    return 0;
}
