// Hand the kernel bad pointers and bad requests from user space. Each
// must fail with an error code; none may crash the kernel or kill
// this program.
#include "ulib.h"

#define UNMAPPED ((void*)0x50000000)

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    char buf[16];
    check(sys_print(0, 10) == -EFAULT, "print from a null pointer gives EFAULT");
    check(sys_print(UNMAPPED, 10) == -EFAULT, "print from an unmapped page gives EFAULT");
    check(sys_write(STDOUT_FILENO, UNMAPPED, 10) == -EFAULT, "write from an unmapped page gives EFAULT");
    int fd = sys_open("WELCOME.TXT", O_RDONLY);
    check(fd >= 0, "open WELCOME.TXT");
    check(sys_read(fd, UNMAPPED, 10) == -EFAULT, "read into an unmapped page gives EFAULT");
    check(sys_read(fd, buf, 4) == 4, "the descriptor still works afterwards");
    sys_close(fd);
    check(sys_open(UNMAPPED, O_RDONLY) == -EFAULT, "open with an unmapped name gives EFAULT");
    check(sys_print(buf, 0) == 0, "print of zero bytes is fine");
    check(sys_gfx_pixel(1, 1, 1) == -EPERM, "drawing without the display gives EPERM");
    check(sys_key_get() == -EPERM, "raw keys without the display give EPERM");
    check(sys_gfx_end() == -EPERM, "releasing a display we do not own gives EPERM");
    check(syscall3(99, 0, 0, 0) == -ENOSYS, "unknown system call gives ENOSYS");
    check(syscall3(-1, 0, 0, 0) == -ENOSYS, "negative system call number gives ENOSYS");
    check(sys_join(sys_getpid()) == -ESRCH, "joining yourself gives ESRCH");
    check(sys_join(999999) == -ESRCH, "joining a task that never existed gives ESRCH");
    check(sys_spawn("NOSUCH") == -ENOENT, "spawning a missing program gives ENOENT");
    check(sys_spawn("WELCOME.TXT") == -ENOEXEC, "spawning a text file gives ENOEXEC");
    return check_summary("badptr");
}
