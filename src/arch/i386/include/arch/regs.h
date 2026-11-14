// The interrupt frame on i386, as isr.asm builds it, and what the rest
// of the kernel needs from it.
#ifndef RINGTAIL_ARCH_REGS_H
#define RINGTAIL_ARCH_REGS_H

#include <stdint.h>

typedef struct regs {
    uint32_t gs, fs, es, ds;                         // pushed by the stub
    uint32_t edi, esi, ebp, esp0, ebx, edx, ecx, eax; // pusha
    uint32_t vector, err;                            // stub, or CPU error code
    uint32_t eip, cs, eflags;                        // CPU
    uint32_t user_esp, user_ss;                      // CPU, only when coming from ring 3
} regs_t;

static inline int regs_from_user(const regs_t* r) { return (r->cs & 3) == 3; }

#define REGS_IP_NAME "eip"
static inline uintptr_t regs_ip(const regs_t* r) { return r->eip; }
static inline uintptr_t regs_fp(const regs_t* r) { return r->ebp; }

// System calls (int 0x80): the number in eax, arguments in ebx, ecx
// and edx, the result in eax.
#define SYSCALL_NR(r)   ((r)->eax)
#define SYSCALL_ARG0(r) ((r)->ebx)
#define SYSCALL_ARG1(r) ((r)->ecx)
#define SYSCALL_ARG2(r) ((r)->edx)
#define SYSCALL_RET(r)  ((r)->eax)

#endif
