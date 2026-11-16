// The x86_64 signal frame: what a handler's call puts on the program's
// stack, and how SYS_SIGRETURN takes it off again.
//
// Below the interrupted rsp, past the 128-byte red zone the ABI lets
// a function use without moving rsp, goes:
//
//   restorer   the handler's return address; rsp points here as the
//              handler starts, 8 past a multiple of 16, as after a call
//   sig, mask  the signal, and the mask to put back
//   regs       every register as it was, rip and rflags among them
//   fpu        the x87 and SSE registers, as fxsave left them
//
// The handler's ret pops restorer, which makes the system call with
// rsp 8 past the frame's start; nothing else is on the stack then.
//
// Everything comes back from user memory, which the program may have
// changed in between, so SYS_SIGRETURN takes only what a program could
// have set for itself anyway: the general registers, a rip and rsp in
// the user half (one past it would fault in ring 0, on the iretq), the
// arithmetic flags and DF, TF and AC (interrupts stay on, IOPL 0), and
// an MXCSR without reserved bits (fxrstor faults on those). The code
// and stack selectors are the user ones whatever the frame says.
#include "kernel.h"
#include "arch.h"
#include "mm.h"
#include "task.h"
#include "arch/regs.h"
#include "arch/fpu.h"

typedef struct {
    uint64_t restorer;
    uint64_t sig;
    uint64_t mask;
    uint64_t pad;
    regs_t regs;
    fpu_state_t fpu;
} sigframe_t;

#define RED_ZONE 128

#define FL_CF 0x1
#define FL_PF 0x4
#define FL_AF 0x10
#define FL_ZF 0x40
#define FL_SF 0x80
#define FL_TF 0x100
#define FL_IF 0x200
#define FL_DF 0x400
#define FL_OF 0x800
#define FL_AC 0x40000
#define FL_FIXED 0x2  // reads as 1
#define FL_USER (FL_CF | FL_PF | FL_AF | FL_ZF | FL_SF | FL_TF | FL_DF | FL_OF | FL_AC)

int arch_signal_push(regs_t* r, uintptr_t handler, uintptr_t restorer, int sig, uint32_t mask) {
    sigframe_t f;  // on the kernel stack: copy_to_user can be preempted, and the next task deliver too
    memset(&f, 0, sizeof f);
    f.restorer = restorer;
    f.sig = (uint64_t)sig;
    f.mask = mask;
    f.regs = *r;
    fpu_save(&f.fpu);  // the task's own: the kernel never touches these registers
    uintptr_t at = ((r->rsp - RED_ZONE - sizeof f) & ~(uintptr_t)15) - 8;
    if (r->rsp < RED_ZONE + sizeof f + 16 || copy_to_user((void*)at, &f, sizeof f)) return 0;
    r->rsp = at;
    r->rip = handler;
    r->rdi = (uint64_t)sig;
    r->rflags &= ~(uint64_t)(FL_DF | FL_TF);  // the ABI wants DF clear on entry
    return 1;
}

// MXCSR bits the CPU accepts, from the mask fxsave reports (0 there
// means the original SSE default).
static uint32_t mxcsr_mask(void) {
    static uint32_t m;
    if (!m) {
        fpu_state_t probe;
        fpu_save(&probe);
        memcpy(&m, probe.b + 28, 4);
        if (!m) m = 0xFFBF;
    }
    return m;
}

int arch_signal_pop(regs_t* r, uint32_t* mask) {
    sigframe_t f;
    if (copy_from_user(&f, (const void*)(r->rsp - 8), sizeof f)) return 0;
    if (f.regs.rip >= USER_TOP || f.regs.rsp > USER_TOP) return 0;
    r->r15 = f.regs.r15; r->r14 = f.regs.r14; r->r13 = f.regs.r13; r->r12 = f.regs.r12;
    r->r11 = f.regs.r11; r->r10 = f.regs.r10; r->r9 = f.regs.r9; r->r8 = f.regs.r8;
    r->rbp = f.regs.rbp; r->rdi = f.regs.rdi; r->rsi = f.regs.rsi; r->rdx = f.regs.rdx;
    r->rcx = f.regs.rcx; r->rbx = f.regs.rbx; r->rax = f.regs.rax;
    r->rip = f.regs.rip;
    r->rsp = f.regs.rsp;
    r->rflags = (f.regs.rflags & FL_USER) | FL_IF | FL_FIXED;
    uint32_t mxcsr;
    memcpy(&mxcsr, f.fpu.b + 24, 4);
    mxcsr &= mxcsr_mask();
    memcpy(f.fpu.b + 24, &mxcsr, 4);
    fpu_restore(&f.fpu);
    *mask = (uint32_t)f.mask;
    return 1;
}
