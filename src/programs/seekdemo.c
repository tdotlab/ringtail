// Seek: every whence, end-of-file reads, holes, and the error cases.
#include "ulib.h"

static int read_is(int fd, uint32_t n, const char* want) {
    char buf[32];
    return sys_read(fd, buf, n) == (int)strlen(want) && memcmp(buf, want, strlen(want)) == 0;
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    sys_delete("SEEK.TXT");
    int fd = sys_open("SEEK.TXT", O_RDWR | O_CREAT);
    check(sys_write(fd, "0123456789ABCDEF", 16) == 16, "write 16 bytes");
    check(sys_seek(fd, 5, SEEK_SET) == 5 && read_is(fd, 3, "567"), "SET 5, read 567");
    check(sys_seek(fd, 2, SEEK_CUR) == 10 && read_is(fd, 2, "AB"), "CUR +2, read AB");
    check(sys_seek(fd, -4, SEEK_END) == 12 && read_is(fd, 4, "CDEF"), "END -4, read CDEF");
    check(sys_seek(fd, 0, SEEK_END) == 16 && read_is(fd, 4, ""), "END 0 is end of file");
    check(sys_seek(fd, 100, SEEK_SET) == 100 && read_is(fd, 4, ""), "SET past the end reads nothing");
    check(sys_write(fd, "tail", 4) == 4, "write at 100 leaves a hole");
    char hole[84];
    int zero = 1;
    sys_seek(fd, 16, SEEK_SET);
    check(sys_read(fd, hole, sizeof hole) == (int)sizeof hole, "read the hole");
    for (uint32_t i = 0; i < sizeof hole; i++) if (hole[i]) zero = 0;
    check(zero, "the hole reads as zeros");
    check(read_is(fd, 4, "tail"), "followed by the tail");
    check(sys_seek(fd, 0, SEEK_END) == 104, "file is now 104 bytes");
    check(sys_seek(fd, 0, 99) == -EINVAL, "bad whence gives EINVAL");
    check(sys_seek(fd, -1, SEEK_SET) == -EINVAL, "negative offset gives EINVAL");
    check(sys_seek(fd, -1000, SEEK_CUR) == -EINVAL, "seeking before the start gives EINVAL");
    check(sys_seek(fd, 0, SEEK_CUR) == 104, "a failed seek leaves the offset alone");
    check(sys_delete("SEEK.TXT") == -EBUSY, "deleting an open file gives EBUSY");
    check(sys_seek(STDOUT_FILENO, 0, SEEK_SET) == -EINVAL, "the console does not seek");
    sys_close(fd);
    check(sys_seek(fd, 0, SEEK_SET) == -EBADF, "seek on a closed descriptor gives EBADF");
    check(sys_delete("SEEK.TXT") == 0, "delete after close works");
    return check_summary("seekdemo");
}
