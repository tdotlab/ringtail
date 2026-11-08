// ls [-R] [PATH...]    list files and directories
//
// One line per entry: name, size or <dir>, and when it was written.
// With no PATH the current directory is listed. A file is listed as
// itself, files first, then each directory (headed with its name when
// there is more than one thing to show). -R goes down into every
// directory below. The listing is always this long form, so -l and -1
// are taken and change nothing, and -a and -A too: the disk has no
// hidden files and "." and ".." are never listed. Entries come in the
// order the disk keeps them, which is not sorted.
//
// On the screen directories are blue and one summary of the entries
// and the free disk space follows; into a pipe or a file there are only
// the lines, so wc and grep can count them.
#include "ulib.h"

#define BLU  "\x1b[94m"
#define DIM  "\x1b[90m"
#define NORM "\x1b[0m"

// The name column fits the longest name up to LS_WIDE; a longer one
// pushes its size along rather than widening every line.
#define LS_NARROW 13
#define LS_WIDE 32

static int tty, recurse, status, first = 1, listed;
static uint32_t files, dirs;

// "2026-11-05 14:32" from the FAT words in a dirent, or a dash for an
// entry nothing ever stamped.
static void fmt_stamp(char* buf, size_t size, unsigned date, unsigned time) {
    if (!date) {
        strlcpy(buf, "-", size);
        return;
    }
    snprintf(buf, size, "%04u-%02u-%02u %02u:%02u", 1980 + (date >> 9), (date >> 5) & 15, date & 31, time >> 11,
             (time >> 5) & 63);
}

static int width_for(int w, const char* name, int is_dir) {
    int n = (int)strlen(name) + (is_dir ? 1 : 0);
    return n > w ? (n < LS_WIDE ? n : LS_WIDE) : w;
}

static void show(int width, const char* name, const dirent_t* d) {
    char stamp[24];
    fmt_stamp(stamp, sizeof stamp, d->mdate, d->mtime);
    if (d->flags & DIRENT_DIR) {
        char shown[PATH_MAX + 2];
        snprintf(shown, sizeof shown, "%s/", name);
        printf("  %s%-*s%s %8s  %s\n", tty ? BLU : "", width, shown, tty ? NORM : "", "<dir>", stamp);
        dirs++;
    } else {
        printf("  %-*s %8u  %s\n", width, name, d->size, stamp);
        files++;
    }
}

static void list_dir(const char* path, int header) {
    int e;
    uint32_t n = 0;
    dirent_t* list = read_dir(path, &n, &e);
    if (!list) {
        fs_error("ls", path, e);
        status = 1;
        return;
    }
    sort_dirents(list, n);
    listed = 1;
    if (!first) printf("\n");
    first = 0;
    if (header) printf("%s:\n", path);
    int width = LS_NARROW;
    for (uint32_t i = 0; i < n; i++) width = width_for(width, list[i].name, list[i].flags & DIRENT_DIR);
    for (uint32_t i = 0; i < n; i++) show(width, list[i].name, &list[i]);
    if (recurse) {
        for (uint32_t i = 0; i < n; i++) {
            if (!(list[i].flags & DIRENT_DIR)) continue;
            char child[PATH_MAX];
            if (join_path(child, sizeof child, path, list[i].name) < 0) {
                fs_error("ls", list[i].name, -ENAMETOOLONG);
                status = 1;
            } else {
                list_dir(child, 1);
            }
        }
    }
    free(list);
}

int main(int argc, char** argv) {
    char got[32];
    if (getflags(&argc, argv, "1alAR", got)) {
        dprintf(STDERR_FILENO, "usage: ls [-R] [PATH...]\n");
        return 2;
    }
    recurse = has_flag(got, 'R');
    tty = sys_fdtype(STDOUT_FILENO) == FDTYPE_TTY;
    static char* dot[] = { ".", 0 };
    char** ops = argc > 1 ? argv + 1 : dot;
    int nops = argc > 1 ? argc - 1 : 1;

    dirent_t* st = malloc(sizeof *st * (size_t)nops);
    if (!st) {
        dprintf(STDERR_FILENO, "ls: out of memory\n");
        return 1;
    }
    int ok[nops];
    int width = LS_NARROW;
    for (int i = 0; i < nops; i++) {
        int r = sys_stat(ops[i], &st[i]);
        ok[i] = r >= 0;
        if (r < 0) {
            fs_error("ls", ops[i], r);
            status = 1;
        } else if (!(st[i].flags & DIRENT_DIR)) {
            width = width_for(width, ops[i], 0);
        }
    }
    for (int i = 0; i < nops; i++) {
        if (!ok[i] || (st[i].flags & DIRENT_DIR)) continue;
        show(width, ops[i], &st[i]);
        listed = 1;
        first = 0;
    }
    int headed = nops > 1 || recurse;
    for (int i = 0; i < nops; i++)
        if (ok[i] && (st[i].flags & DIRENT_DIR)) list_dir(ops[i], headed);
    free(st);

    if (tty && listed) {
        char disk[64];
        if (sys_kinfo(KINFO_DISK, disk, sizeof disk) < 0) {
            fs_error("ls", ".", -EIO);
            return 1;
        }
        printf(DIM "%u file%s, ", files, files == 1 ? "" : "s");
        if (dirs) printf("%u director%s, ", dirs, dirs == 1 ? "y" : "ies");
        printf("%s" NORM, disk);
    }
    return status;
}
