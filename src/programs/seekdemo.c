// seekdemo: the program that proves seek works. It writes a
// known file, "0123456789ABCDEF" (16 bytes, so every index is
// its own digit or letter), then jumps around in it with all
// three seek modes, reading back after each jump, and checks
// that the bytes it reads are the ones at the new position.
// It also checks the error paths, because a demo that only
// shows the happy path does not prove validation: a bad
// whence, a negative target, and a seek on a closed
// descriptor must all fail without moving the offset, which
// the program proves by reading after each failed seek and
// showing the bytes are still the ones at the old position.
// Finally it checks the one asymmetry in seek's design:
// seeking from the start or current offset is offset
// arithmetic only and still succeeds on a file deleted after
// opening, while seeking from the end needs the file's size
// and must fail once the file is gone. It leaves seek.txt
// on disk, so the shell can cat it afterward, like filedemo
// leaves demo.txt. Same packaging as the other programs:
// separate binary, linked at 0x08000000, entry at offset 0,
// ends in sys_exit.
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

static char buf[64];

// Read len bytes and check they equal expect. Returns 1 on
// a match, 0 otherwise. buf still holds what was read, so
// the caller can print it in the ok line.
static int read_matches(int fd, const char* expect, int len) {
    int n = sys_read(fd, buf, len);
    if (n != len) return 0;
    for (int i = 0; i < len; i++) if (buf[i] != expect[i]) return 0;
    return 1;
}

static void print_ok_read(const char* label, int ret) {
    sys_print(label, prog_strlen(label));
    sys_print(" = ", 3);
    print_num(ret);
    sys_print(", read ", 7);
}

__attribute__((section(".text._start"))) void _start(void) {
    sys_delete("seek.txt"); // clean start, ignore the result
    int fd = sys_open("seek.txt");
    if (fd < 0) {
        const char* msg = "open failed\n";
        sys_print(msg, prog_strlen(msg));
        sys_exit();
    }
    const char* text = "0123456789ABCDEF";
    int written = sys_write(fd, text, 16);
    if (written != 16) {
        sys_print("ERROR: wrote ", 13);
        print_num(written);
        sys_print("\n", 1);
        sys_exit();
    }
    sys_print("wrote 16 ok\n", 12);

    // SET 5: offset 5, read "567", offset now 8.
    int r = sys_seek(fd, 5, SEEK_SET);
    if (r == 5 && read_matches(fd, "567", 3)) {
        print_ok_read("SET 5", r);
        sys_print(buf, 3);
        sys_print(" ok\n", 4);
    } else {
        sys_print("ERROR: SET 5\n", 13);
    }

    // CUR 2: from 8 to 10, read "AB", offset now 12.
    r = sys_seek(fd, 2, SEEK_CUR);
    if (r == 10 && read_matches(fd, "AB", 2)) {
        print_ok_read("CUR 2", r);
        sys_print(buf, 2);
        sys_print(" ok\n", 4);
    } else {
        sys_print("ERROR: CUR 2\n", 13);
    }

    // END -4: size 16, so offset 12, read "CDEF", offset 16.
    r = sys_seek(fd, -4, SEEK_END);
    if (r == 12 && read_matches(fd, "CDEF", 4)) {
        print_ok_read("END -4", r);
        sys_print(buf, 4);
        sys_print(" ok\n", 4);
    } else {
        sys_print("ERROR: END -4\n", 14);
    }

    // END 0: offset 16, read at end of file returns 0.
    r = sys_seek(fd, 0, SEEK_END);
    if (r == 16 && sys_read(fd, buf, 4) == 0) {
        sys_print("END 0 = 16, eof ok\n", 19);
    } else {
        sys_print("ERROR: END 0\n", 13);
    }

    // SET 100: past end of file is allowed, read returns 0.
    r = sys_seek(fd, 100, SEEK_SET);
    if (r == 100 && sys_read(fd, buf, 4) == 0) {
        sys_print("SET 100 = 100, eof ok\n", 22);
    } else {
        sys_print("ERROR: SET 100\n", 15);
    }

    // CUR -100: from 100 back to 0, read "0123".
    r = sys_seek(fd, -100, SEEK_CUR);
    if (r == 0 && read_matches(fd, "0123", 4)) {
        print_ok_read("CUR -100", r);
        sys_print(buf, 4);
        sys_print(" ok\n", 4);
    } else {
        sys_print("ERROR: CUR -100\n", 16);
    }

    // Bad whence: must fail, offset stays 4, read "45".
    r = sys_seek(fd, 0, 99);
    if (r < 0 && read_matches(fd, "45", 2)) {
        sys_print("bad whence correctly failed, read ", 34);
        sys_print(buf, 2);
        sys_print(" ok\n", 4);
    } else {
        sys_print("ERROR: bad whence\n", 18);
    }

    // SET -1: must fail, offset stays 6, read "67".
    r = sys_seek(fd, -1, SEEK_SET);
    if (r < 0 && read_matches(fd, "67", 2)) {
        sys_print("SET -1 correctly failed, read ", 30);
        sys_print(buf, 2);
        sys_print(" ok\n", 4);
    } else {
        sys_print("ERROR: SET -1\n", 14);
    }

    // CUR -100 from 8: would go negative, must fail, offset
    // stays 8, read "89".
    r = sys_seek(fd, -100, SEEK_CUR);
    if (r < 0 && read_matches(fd, "89", 2)) {
        sys_print("CUR -100 correctly failed, read ", 32);
        sys_print(buf, 2);
        sys_print(" ok\n", 4);
    } else {
        sys_print("ERROR: CUR -100\n", 16);
    }

    // Closed descriptor: seek must fail.
    sys_close(fd);
    r = sys_seek(fd, 0, SEEK_SET);
    if (r < 0) {
        sys_print("closed fd seek correctly failed\n", 32);
    } else {
        sys_print("ERROR: closed fd seek succeeded\n", 32);
    }

    // Deleted file: open a second file, delete it while open,
    // seek from the start still succeeds (offset arithmetic
    // only), seek from the end fails (it needs the size).
    int fd2 = sys_open("seek2.txt");
    if (fd2 < 0) {
        sys_print("ERROR: seek2 open failed\n", 25);
    } else {
        sys_write(fd2, "hi", 2);
        sys_delete("seek2.txt");
        int s = sys_seek(fd2, 0, SEEK_SET);
        int e = sys_seek(fd2, 0, SEEK_END);
        if (s == 0 && e < 0) {
            sys_print("deleted file set = 0, end correctly failed\n", 43);
        } else {
            sys_print("ERROR: deleted file seek\n", 25);
        }
        sys_close(fd2);
    }

    sys_exit();
}
