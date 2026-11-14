// The interrupt frame on x86_64, as isr.asm builds it, and what the
// rest of the kernel needs from it.
#ifndef RINGTAIL_ARCH_REGS_H
#define RINGTAIL_ARCH_REGS_H

#include <stdint.h>

typedef struct regs {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;  // pushed by the stub
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, err;                           // stub, or CPU error code
    uint64_t rip, cs, rflags, rsp, ss;              // CPU, from either ring
} regs_t;

static inline int regs_from_user(const regs_t* r) { return (r->cs & 3) == 3; }

#define REGS_IP_NAME "rip"
static inline uintptr_t regs_ip(const regs_t* r) { return r->rip; }
static inline uintptr_t regs_fp(const regs_t* r) { return r->rbp; }

// System calls (int 0x80): the number in rax, arguments in rdi, rsi
// and rdx as for a C call, the result in rax. Every other register
// comes back as it was.
#define SYSCALL_NR(r)   ((r)->rax)
#define SYSCALL_ARG0(r) ((r)->rdi)
#define SYSCALL_ARG1(r) ((r)->rsi)
#define SYSCALL_ARG2(r) ((r)->rdx)
#define SYSCALL_RET(r)  ((r)->rax)

#endif
