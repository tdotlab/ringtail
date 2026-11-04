// filedemo: the first program that uses the disk, not just the
// screen. It opens demo.txt (creating it if it is not there),
// writes a line through the write syscall, closes, reopens,
// reads the line back through the read syscall, and prints what
// it read, so the output proves the round trip instead of just
// claiming it. It deliberately leaves demo.txt on disk: after
// the program exits, the shell can cat demo.txt and see the
// program's writing, which is the cross-boundary proof that a
// program and the shell share one filesystem. It also checks
// two error paths, because a demo that only shows the happy
// path does not prove validation: reading a closed descriptor
// must fail, and opening a 32-character name must fail, and the
// program prints whether each check behaved as expected.
// Same packaging as the other programs: separate binary, linked
// at 0x08000000, entry at offset 0, ends in sys_exit.
#include "syscall_stub.h"

static void utoa(int n, char* out) {
    char tmp[12];
    int i = 0;
    if (n == 0) { out[0] = '0'; out[1] = 0; return; }
    while (n > 0) { tmp[i++] = '0' + (n % 10); n /= 10; }
    int j = 0;
    while (i > 0) out[j++] = tmp[--i];
    out[j] = 0;
}

static void print_num(int n) {
    char buf[12];
    utoa(n, buf);
    sys_print(buf, prog_strlen(buf));
}

__attribute__((section(".text._start"))) void _start(void) {
    int fd = sys_open("demo.txt");
    if (fd < 0) {
        const char* msg = "open failed\n";
        sys_print(msg, prog_strlen(msg));
        sys_exit();
    }
    const char* hdr = "fd=";
    sys_print(hdr, prog_strlen(hdr));
    print_num(fd);
    sys_print("\n", 1);

    const char* text = "Hello from a program!\n";
    int written = sys_write(fd, text, prog_strlen(text));
    const char* whdr = "wrote ";
    sys_print(whdr, prog_strlen(whdr));
    print_num(written);
    sys_print(" bytes\n", 7);
    sys_close(fd);

    fd = sys_open("demo.txt");
    if (fd < 0) {
        const char* msg = "reopen failed\n";
        sys_print(msg, prog_strlen(msg));
        sys_exit();
    }
    char buf[128];
    int readn = sys_read(fd, buf, sizeof(buf) - 1);
    if (readn < 0) {
        const char* msg = "read failed\n";
        sys_print(msg, prog_strlen(msg));
        sys_exit();
    }
    buf[readn] = 0;
    const char* rhdr = "read back: ";
    sys_print(rhdr, prog_strlen(rhdr));
    sys_print(buf, readn);
    sys_close(fd);

    // Error path 1: reading a closed descriptor must fail.
    int bad = sys_read(fd, buf, 10);
    if (bad < 0) {
        const char* msg = "closed fd read correctly failed\n";
        sys_print(msg, prog_strlen(msg));
    } else {
        const char* msg = "ERROR: closed fd read succeeded\n";
        sys_print(msg, prog_strlen(msg));
    }

    // Error path 2: a 32-character name must fail (too long for 8.3).
    int longfd = sys_open("12345678901234567890123456789012");
    if (longfd < 0) {
        const char* msg = "long name correctly rejected\n";
        sys_print(msg, prog_strlen(msg));
    } else {
        const char* msg = "ERROR: long name accepted\n";
        sys_print(msg, prog_strlen(msg));
        sys_close(longfd);
    }

    sys_exit();
}
