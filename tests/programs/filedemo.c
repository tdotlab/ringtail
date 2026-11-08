// Files through system calls: create, write, read back, append,
// error paths, descriptor limits, and a write that spans clusters.
#include "ulib.h"

static char big[10000], back[10000];

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    const char* text = "Hello from a program!\n";
    uint32_t len = strlen(text);
    char buf[64];

    int fd = sys_open("DEMO.TXT", O_RDWR | O_CREAT | O_TRUNC);
    check(fd >= 2, "create DEMO.TXT");
    check(sys_write(fd, text, len) == (int)len, "write 22 bytes");
    check(sys_close(fd) == 0, "close");

    fd = sys_open("demo.txt", O_RDONLY);
    check(fd >= 2, "reopen read-only (names are case-insensitive)");
    int n = sys_read(fd, buf, sizeof buf);
    check(n == (int)len && memcmp(buf, text, len) == 0, "read back the same bytes");
    check(sys_read(fd, buf, sizeof buf) == 0, "then end of file");
    check(sys_write(fd, "x", 1) == -EBADF, "write to a read-only descriptor gives EBADF");
    sys_close(fd);
    check(sys_read(fd, buf, 4) == -EBADF, "read from a closed descriptor gives EBADF");

    fd = sys_open("DEMO.TXT", O_WRONLY | O_APPEND);
    check(sys_write(fd, "more\n", 5) == 5, "append 5 bytes");
    check(sys_read(fd, buf, 4) == -EBADF, "read from a write-only descriptor gives EBADF");
    sys_close(fd);
    fd = sys_open("DEMO.TXT", O_RDONLY);
    n = sys_read(fd, buf, sizeof buf);
    check(n == (int)len + 5 && memcmp(buf + len, "more\n", 5) == 0, "appended data is at the end");
    sys_close(fd);

    check(sys_open("NOFILE.TXT", O_RDONLY) == -ENOENT, "open of a missing file gives ENOENT");
    char toolong[NAME_MAX + 2];
    memset(toolong, 'n', NAME_MAX + 1);
    toolong[NAME_MAX + 1] = 0;
    check(sys_open(toolong, O_RDWR | O_CREAT) == -ENAMETOOLONG, "a name over 255 characters gives ENAMETOOLONG");
    check(sys_open("BAD*.TXT", O_RDWR | O_CREAT) == -EINVAL, "a forbidden character gives EINVAL");
    check(sys_open("DEMO.TXT", O_RDONLY | O_TRUNC) == -EINVAL, "truncating read-only gives EINVAL");

    // The terminal by name, for a program whose standard input is a pipe.
    int tt = sys_open(DEV_TTY, O_WRONLY);
    check(tt >= 0 && sys_fdtype(tt) == FDTYPE_TTY, "/dev/tty opens for writing, and is the terminal");
    check(sys_read(tt, buf, 1) == -EBADF, "and cannot be read through that descriptor");
    if (tt >= 0) sys_close(tt);
    tt = sys_open(DEV_TTY, O_RDONLY);
    check(tt >= 0 && sys_fdtype(tt) == FDTYPE_TTY, "/dev/tty opens for reading too");
    check(sys_write(tt, buf, 1) == -EBADF, "and cannot be written through that one");
    if (tt >= 0) sys_close(tt);
    check(sys_open(DEV_TTY, O_RDWR) == -EINVAL, "but not both ways at once");
    check(sys_open(DEV_TTY, O_WRONLY | O_CREAT) == -EINVAL, "or with creating flags");

    int opened = 0, last = 0;
    int held[32];
    for (int i = 0; i < 32; i++) {
        last = sys_open("DEMO.TXT", O_RDONLY);
        if (last < 0) break;
        held[opened++] = last;
    }
    check(last == -EMFILE && opened == 13, "the 14th open gives EMFILE (16 slots, 3 are stdio)");
    for (int i = 0; i < opened; i++) sys_close(held[i]);

    for (uint32_t i = 0; i < sizeof big; i++) big[i] = (char)(i * 7 + i / 251);
    fd = sys_open("BIG.DAT", O_RDWR | O_CREAT | O_TRUNC);
    check(sys_write(fd, big, sizeof big) == (int)sizeof big, "write 10000 bytes");
    check(sys_seek(fd, 0, SEEK_SET) == 0, "seek back to the start");
    check(sys_read(fd, back, sizeof back) == (int)sizeof back && memcmp(big, back, sizeof big) == 0,
          "read all 10000 back intact");
    check(sys_seek(fd, 5000, SEEK_SET) == 5000 && sys_read(fd, back, 100) == 100 &&
          memcmp(back, big + 5000, 100) == 0, "read 100 from the middle");
    sys_close(fd);
    check(sys_delete("BIG.DAT") == 0, "delete BIG.DAT");
    check(sys_delete("DEMO.TXT") == 0, "delete DEMO.TXT");
    check(sys_open("DEMO.TXT", O_RDONLY) == -ENOENT, "it is gone");
    return check_summary("filedemo");
}
