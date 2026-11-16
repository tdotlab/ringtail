// Core kernel interfaces: boot info, panic, fault log, GDT/TSS,
// version, and entry points of the larger subsystems.
#ifndef RINGTAIL_KERNEL_H
#define RINGTAIL_KERNEL_H

#include <stdint.h>
#include "lib.h"
#include "ringtail/abi.h"  // SPAWN_FDS

#define RINGTAIL_VERSION "0.28.0"
#define RINGTAIL_CODENAME "Time Will Tell"

// Segment selectors, fixed by the GDT layout in gdt.c.
#define SEL_KERNEL_CODE 0x08
#define SEL_KERNEL_DATA 0x10
#define SEL_USER_DATA   0x1B  // before user code: SYSRET needs it so (gdt.c)
#define SEL_USER_CODE   0x23
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

// The most processors the kernel will start, the boot CPU included.
#define MAX_CPUS 64

// The boot CPU's GDT, TSS and SYSCALL setup (gdt.c), and another
// CPU's (index 1 and up), with its three interrupt stacks in
// ist_stacks (IST_STACKS_SIZE bytes, 16-byte aligned).
void gdt_init(void);
void gdt_init_cpu(int index, void* ist_stacks);
// The kernel stack the CPU switches to on a trap or system call from
// ring 3.
void tss_set_kstack(uintptr_t top);
// The TSS's interrupt stacks (gdt.c) and the vectors that use them
// (cpu.c): traps that can arrive while rsp is not a kernel stack.
#define IST_NMI 1
#define IST_DOUBLE_FAULT 2
#define IST_MACHINE_CHECK 3
#define IST_STACK_SIZE 8192
#define IST_STACKS_SIZE (3 * IST_STACK_SIZE)

// The other processors (smp.c): started after the timer, which times
// the startup sequence and keeps the time for all of them, with
// interrupts still off. Each one sets itself up (GDT, TSS, interrupts,
// FPU, its local APIC and timer) and becomes an idle task, which runs
// tasks from its first tick once the boot CPU lets go of the kernel
// lock.
void smp_init(void);
int smp_cpus(void);  // processors running, the boot CPU included
// "4", or "1 of 4 started" and why, for the banner.
const char* smp_describe(char* buf, uint32_t size);
void smp_format(sbuf_t* sb);  // KINFO_CPUS
// Wake an idle CPU, if there is one, to run a task just made ready.
void smp_kick_idle(void);

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
#define ARGV_MAX 64
// held: the task waits for task_release before it runs.
int loader_spawn(int at, const char* path, int argc, const char* const* argv, int job, const int std[SPAWN_FDS], int held);

// SYS_KINFO's reports (kinfo.c): 0, or -EINVAL for an unknown one.
int kinfo_format(int what, sbuf_t* sb);

// The init task (init.c): starts the shell and starts it again
// whenever it ends.
void init_main(void* arg);

// Power control (power.c).
void machine_reboot(void) __attribute__((noreturn));
void machine_poweroff(void) __attribute__((noreturn));

#endif
