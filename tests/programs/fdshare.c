// Shared open files: a copy of a descriptor, from dup2 or handed to a
// child by spawn, is the same open file, with one offset, as in POSIX;
// it stays open until the last copy is closed, and nothing leaks.
#include "ulib.h"

#define TMP "fdshare.tmp"

static void wait_ticks(uint32_t n) {
    uint32_t start = sys_ticks();
    while (sys_ticks() - start < n) sys_poll(1);
}

static int spawn_with(const char* const* argv, int in, int out) {
    spawn_opts_t o = { 0, 0, { in, out, -1 }, -1 };
    return sys_spawn_opts("fdshare", argv, &o);
}

// What the file holds, NUL-terminated, or -errno.
static int contents(char* buf, int size) {
    int f = sys_open(TMP, O_RDONLY);
    if (f < 0) return f;
    int got = 0, n;
    while (got < size - 1 && (n = sys_read(f, buf + got, (uint32_t)(size - 1 - got))) > 0) got += n;
    sys_close(f);
    buf[got] = 0;
    return got;
}

static int put(const char* text) {
    int f = sys_open(TMP, O_WRONLY | O_CREAT | O_TRUNC);
    if (f < 0) return f;
    int n = sys_write(f, text, (uint32_t)strlen(text));
    sys_close(f);
    return n;
}

static int heap_used(void) {
    char buf[200];
    if (sys_kinfo(KINFO_HEAP, buf, sizeof buf) < 0) return -1;
    // "... KB total, N bytes used, ..."
    for (char* p = buf; *p; p++) {
        if (memcmp(p, "total, ", 7)) continue;
        int n = 0;
        for (p += 7; *p >= '0' && *p <= '9'; p++) n = n * 10 + (*p - '0');
        return n;
    }
    return -1;
}

static int child(int argc, char** argv) {
    const char* role = argv[1];
    if (!strcmp(role, "write") && argc == 3)
        return sys_write(STDOUT_FILENO, argv[2], (uint32_t)strlen(argv[2])) == (int)strlen(argv[2]) ? 0 : 1;
    if (!strcmp(role, "late") && argc == 3) {
        wait_ticks(20);
        return sys_write(STDOUT_FILENO, argv[2], (uint32_t)strlen(argv[2])) == (int)strlen(argv[2]) ? 0 : 1;
    }
    if (!strcmp(role, "read") && argc == 3) {  // the next bytes on stdin must be argv[2]
        char buf[16];
        int want = (int)strlen(argv[2]);
        return sys_read(STDIN_FILENO, buf, (uint32_t)want) == want && !memcmp(buf, argv[2], (size_t)want) ? 0 : 1;
    }
    if (!strcmp(role, "seek") && argc == 2) return sys_seek(STDOUT_FILENO, 1, SEEK_SET) == 1 ? 0 : 1;
    if (!strcmp(role, "lines") && argc == 3) {  // 200 lines of ten bytes, one write each
        char line[10];
        memset(line, argv[2][0], 9);
        line[9] = '\n';
        for (int i = 0; i < 200; i++)
            if (sys_write(STDOUT_FILENO, line, 10) != 10) return 1;
        return 0;
    }
    return 2;
}

int main(int argc, char** argv) {
    if (argc >= 2) return child(argc, argv);
    char buf[4200];

    // Writing: parent, child, parent.
    int f = sys_open(TMP, O_WRONLY | O_CREAT | O_TRUNC);
    sys_write(f, "a", 1);
    const char* w[] = { "fdshare", "write", "bb", 0 };
    int id = spawn_with(w, -1, f);
    check(id > 0 && sys_join(id) == 0, "a child writes to its parent's open file");
    check(sys_seek(f, 0, SEEK_CUR) == 3, "and moves the parent's offset with it");
    sys_write(f, "c", 1);
    sys_close(f);
    check(contents(buf, sizeof buf) == 4 && !strcmp(buf, "abbc"), "so the parent's next write comes after the child's");

    // Reading: the child takes up where the parent left off, and the
    // parent after the child.
    put("0123456789");
    f = sys_open(TMP, O_RDONLY);
    sys_read(f, buf, 2);
    const char* r[] = { "fdshare", "read", "234", 0 };
    id = spawn_with(r, f, -1);
    check(id > 0 && sys_join(id) == 0, "a child reads on from where its parent's offset is");
    check(sys_read(f, buf, 2) == 2 && !memcmp(buf, "56", 2), "and the parent reads on from where the child stopped");
    sys_close(f);

    // A child's seek moves the parent's offset too.
    put("hello");
    f = sys_open(TMP, O_WRONLY);
    const char* s[] = { "fdshare", "seek", 0 };
    id = spawn_with(s, -1, f);
    check(id > 0 && sys_join(id) == 0 && sys_seek(f, 0, SEEK_CUR) == 1, "a child's seek moves its parent's offset");
    sys_write(f, "J", 1);
    sys_close(f);
    check(contents(buf, sizeof buf) == 5 && !strcmp(buf, "hJllo"), "and the parent writes where the child sought");

    // dup2 within one task.
    f = sys_open(TMP, O_RDWR | O_TRUNC);
    check(sys_dup2(f, 7) == 7, "dup2 a file");
    sys_write(7, "xy", 2);
    check(sys_seek(f, 0, SEEK_CUR) == 2, "the copy and the original share one offset");
    sys_seek(f, 0, SEEK_SET);
    check(sys_seek(7, 0, SEEK_CUR) == 0, "both ways");
    sys_close(f);
    sys_seek(7, 0, SEEK_END);
    check(sys_write(7, "z", 1) == 1 && sys_seek(7, 0, SEEK_CUR) == 3, "closing one copy leaves the other open");
    sys_close(7);
    check(contents(buf, sizeof buf) == 3 && !strcmp(buf, "xyz"), "and what it wrote is there");

    // The parent closes its copy at once; the child's keeps the file
    // open until it has written, 200 ms later.
    f = sys_open(TMP, O_WRONLY | O_TRUNC);
    const char* l[] = { "fdshare", "late", "after", 0 };
    id = spawn_with(l, -1, f);
    sys_close(f);
    check(id > 0 && sys_join(id) == 0, "a child writes to a file its parent has already closed");
    check(contents(buf, sizeof buf) == 5 && !strcmp(buf, "after"), "and its output is in the file");

    // Two children write to the same open file at once: every write
    // gets bytes of its own, none lands on another's.
    f = sys_open(TMP, O_WRONLY | O_TRUNC);
    const char* la[] = { "fdshare", "lines", "A", 0 };
    const char* lb[] = { "fdshare", "lines", "B", 0 };
    int a = spawn_with(la, -1, f), b = spawn_with(lb, -1, f);
    int sa = a > 0 ? sys_join(a) : -1, sb = b > 0 ? sys_join(b) : -1;
    int end = sys_seek(f, 0, SEEK_CUR);
    sys_close(f);
    int n = contents(buf, sizeof buf), as = 0, bs = 0, bad = 0;
    for (int i = 0; i + 10 <= n; i += 10) {
        if (!memcmp(buf + i, "AAAAAAAAA\n", 10)) as++;
        else if (!memcmp(buf + i, "BBBBBBBBB\n", 10)) bs++;
        else bad++;
    }
    check(sa == 0 && sb == 0 && n == 4000 && end == 4000 && as == 200 && bs == 200 && !bad,
          "two children writing one open file at once lose nothing");

    // Opening, copying and closing leaves the kernel heap as it was.
    int before = heap_used();
    for (int i = 0; i < 300; i++) {
        int g = sys_open(TMP, O_RDONLY), p[2];
        sys_dup2(g, 8);
        sys_dup2(8, 9);
        sys_dup2(g, 9);  // over a copy of the same file
        sys_close(g);
        sys_close(8);
        sys_close(9);
        if (sys_pipe(p) == 0) {
            sys_dup2(p[1], 8);
            sys_close(p[0]);
            sys_close(p[1]);
            sys_close(8);
        }
        int u = sys_socket(SOCK_DGRAM);
        sys_dup2(u, 8);
        sys_close(u);
        sys_close(8);
    }
    int after = heap_used();
    check(before > 0 && after > 0 && after - before < 2048, "300 rounds of open, pipe, socket, dup2 and close leak nothing");

    sys_delete(TMP);
    return check_summary("fdshare");
}
