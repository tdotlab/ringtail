// Core kernel interfaces: boot info, panic, fault log, GDT/TSS,
// version, and entry points of the larger subsystems.
#ifndef RINGTAIL_KERNEL_H
#define RINGTAIL_KERNEL_H

#include <stdint.h>
#include "lib.h"

#define RINGTAIL_VERSION "0.24.3"
#define RINGTAIL_CODENAME "Handshake"

// Segment selectors, fixed by the GDT layout in gdt.c.
#define SEL_KERNEL_CODE 0x08
#define SEL_KERNEL_DATA 0x10
#define SEL_USER_CODE   0x1B
#define SEL_USER_DATA   0x23
#define SEL_TSS         0x28

// Boot information, copied out of the multiboot structures before
// anything can overwrite them.
#define BOOT_MMAP_MAX 32
typedef struct {
    uint64_t base;
    uint64_t len;
    uint32_t type; // 1 = usable RAM
} mmap_entry_t;

typedef struct {
    uint32_t mmap_count;
    mmap_entry_t mmap[BOOT_MMAP_MAX];
    char cmdline[128];
} boot_info_t;

extern boot_info_t boot_info;

// Linker symbols (see linker.ld).
extern char _kernel_start[], _ro_start[], _ro_end[], _kernel_end[];

void panic(const char* fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));

void gdt_init(void);
void tss_set_esp0(uint32_t esp0);

// Fault log: the last few fatal events, kept so a fault that happened
// while the screen was in graphics mode can still be read afterwards.
void faultlog_add(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void faultlog_format(sbuf_t* sb);

// Syscalls (syscall.c).
struct regs;
void syscall_handle(struct regs* r);

// Program loader (loader.c). Loads an ELF program from the disk (path
// resolved from directory node at) and starts it in job (0 for a new job of its own), with descriptors 0-2
// copied from the current task's std[0..2], or the console when std
// is NULL. Returns the new task id or -errno.
#define ARGV_MAX 16
// held: the task waits for task_release before it runs.
int loader_spawn(int at, const char* path, int argc, const char* const* argv, int job, const int std[3], int held);

// SYS_KINFO's reports (kinfo.c): 0, or -EINVAL for an unknown one.
int kinfo_format(int what, sbuf_t* sb);

// The init task (init.c): starts the shell and starts it again
// whenever it ends.
void init_main(void* arg);

// Power control (power.c).
void machine_reboot(void) __attribute__((noreturn));
void machine_poweroff(void) __attribute__((noreturn));

#endif
