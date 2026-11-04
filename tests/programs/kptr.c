// Try to make the kernel read or write kernel memory on our behalf.
// Before v0.11 a read() into the IDT's address let any program
// install its own ring-0 interrupt gate. Every one of these must now
// fail with EFAULT, and the kernel must keep running.
#include "ulib.h"

#define KTEXT ((void*)0x00100000)   // kernel code
#define KTOP  ((void*)0x3FFFF000)   // last kernel page
#define ABOVE ((void*)0xC0000000)   // above user space

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    char ok[8] = "1234567";
    int fd = sys_open("WELCOME.TXT", O_RDONLY);
    check(fd >= 0, "open WELCOME.TXT");
    check(sys_read(fd, KTEXT, 8) == -EFAULT, "read() into kernel code");
    check(sys_read(fd, (void*)0x00110000, 8) == -EFAULT, "read() into kernel data");
    check(sys_read(fd, KTOP, 8) == -EFAULT, "read() into the top kernel page");
    check(sys_read(fd, ABOVE, 8) == -EFAULT, "read() above user space");
    check(sys_read(fd, (void*)main, 8) == -EFAULT, "read() into our own read-only code");
    check(sys_read(fd, (void*)0x3FFFFFFC, 8) == -EFAULT, "read() straddling into user space");
    check(sys_print(KTEXT, 16) == -EFAULT, "print() of kernel memory");
    check(sys_print((void*)0xFFFFFFF0, 0x20) == -EFAULT, "print() wrapping around 4 GB");
    check(sys_print(ok, 0xFFFFFFFF) == -EFAULT, "print() with a huge length");
    check(sys_mouse_state(KTEXT) == -EFAULT, "mouse state into kernel memory");
    check(sys_gfx_font(KTEXT) == -EFAULT, "font copy into kernel memory");
    check(sys_list(0, KTEXT, 16) == -EFAULT, "list into kernel memory");
    check(sys_open(KTEXT, O_RDONLY) == -EFAULT, "open with a kernel name pointer");
    check(sys_read(fd, ok, 4) == 4, "the descriptor still works");
    sys_close(fd);
    return check_summary("kptr");
}
