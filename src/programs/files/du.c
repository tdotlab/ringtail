// du [-a] [-c] [-h] [-k] [-s] [PATH...]    disk space used by files and directories
//
// Sizes are what the files take on the disk: whole clusters (the
// kernel says how big one is), so a file of 700 bytes on a disk with
// 512-byte clusters counts as 1024. They are written in 1K blocks,
// rounded up, or with -h in the largest of K, M and G that keeps the
// number short (one decimal under 10, rounded up, as GNU does). An empty
// file takes no cluster. A directory counts what is inside it; its own
// clusters are not counted, as the kernel does not say how many it has.
//
// With no PATH, ".". A directory is listed after everything in it, with
// the total; -a lists the files as well, -s only the PATHs themselves.
// -c adds a line with the grand total. -k is the default, spelled out.
#include "ulib.h"

static int all, summary, total_line, human_out, status;
static uint32_t cluster = 512;
static uint64_t grand;

static int usage(void) {
    dprintf(STDERR_FILENO, "usage: du [-achks] [PATH...]\n");
    return 2;
}

// What a file of this many bytes takes: whole clusters.
static uint64_t used(uint32_t size) {
    return ((uint64_t)size + cluster - 1) / cluster * cluster;
}

static void show(uint64_t bytes, const char* name) {
    if (!human_out) {
        printf("%u\t%s\n", (uint32_t)((bytes + 1023) / 1024), name);
        return;
    }
    // Under 1K as it is; then the unit where the number is under 1024,
    // with a decimal while it is under 10.
    if (bytes < 1024) {
        printf("%u\t%s\n", (uint32_t)bytes, name);
        return;
    }
    static const char units[] = "KMG";
    uint64_t unit = 1024;
    int u = 0;
    for (;;) {
        uint64_t tenths = (bytes * 10 + unit - 1) / unit;
        if (tenths < 100) {
            printf("%u.%u%c\t%s\n", (uint32_t)(tenths / 10), (uint32_t)(tenths % 10), units[u], name);
            return;
        }
        uint64_t whole = (bytes + unit - 1) / unit;
        if (whole < 1024 || u == 2) {
            printf("%u%c\t%s\n", (uint32_t)whole, units[u], name);
            return;
        }
        unit *= 1024;
        u++;
    }
}

// The space taken by the directory at path (and everything under it).
static uint64_t walk_dir(const char* path, const char* shown) {
    int e;
    uint32_t n = 0;
    dirent_t* list = read_dir(path, &n, &e);
    uint64_t sum = 0;
    if (!list) {
        fs_error("du", shown, e);
        status = 1;
        return 0;
    }
    for (uint32_t i = 0; i < n; i++) {
        char child[PATH_MAX], name[PATH_MAX];
        if (join_path(child, sizeof child, path, list[i].name) < 0 ||
            join_path(name, sizeof name, shown, list[i].name) < 0) {
            fs_error("du", list[i].name, -ENAMETOOLONG);
            status = 1;
            continue;
        }
        if (list[i].flags & DIRENT_DIR) {
            uint64_t sub = walk_dir(child, name);
            sum += sub;
            if (!summary) show(sub, name);
        } else {
            uint64_t b = used(list[i].size);
            sum += b;
            if (all) show(b, name);  // never with -s: main refuses the pair
        }
    }
    free(list);
    return sum;
}

int main(int argc, char** argv) {
    char got[32];
    if (getflags(&argc, argv, "achks", got)) return usage();
    all = has_flag(got, 'a');
    total_line = has_flag(got, 'c');
    human_out = has_flag(got, 'h');
    summary = has_flag(got, 's');
    if (all && summary) {
        dprintf(STDERR_FILENO, "du: cannot both summarize and show all entries\n");
        return usage();
    }
    char text[16];
    if (sys_kinfo(KINFO_CLUSTER, text, sizeof text) > 0) {
        uint32_t v = 0;
        for (const char* p = text; *p >= '0' && *p <= '9'; p++) v = v * 10 + (uint32_t)(*p - '0');
        if (v) cluster = v;
    }
    static char* dot[] = { ".", 0 };
    char** ops = argc > 1 ? argv + 1 : dot;
    int nops = argc > 1 ? argc - 1 : 1;
    for (int i = 0; i < nops; i++) {
        dirent_t d;
        int r = sys_stat(ops[i], &d);
        if (r < 0) {
            fs_error("du", ops[i], r);
            status = 1;
            continue;
        }
        uint64_t b = (d.flags & DIRENT_DIR) ? walk_dir(ops[i], ops[i]) : used(d.size);
        grand += b;
        show(b, ops[i]);
    }
    if (total_line) show(grand, "total");
    return status;
}
