// Signals, on the receiving end: actions, the mask, delivery on the way
// back to user mode, and the return from a handler. Sending is
// task_signal (task.c). The numbers and rules are in ringtail/abi.h.
//
// Only the task itself changes its mask and actions, so those need no
// lock beyond the kernel's; sig_pending is set by anyone, from any CPU
// or an interrupt handler, so it changes only by atomic operations.
#include "kernel.h"
#include "arch.h"
#include "mm.h"
#include "console.h"
#include "task.h"
#include "signal.h"
#include "ringtail/abi.h"

#define UNBLOCKABLE SIGBIT(SIGKILL)
#define ALL_SIGNALS ((SIGBIT(NSIG) - 1) & ~1u)

static void clear_pending(uint32_t bits) {
    __atomic_fetch_and(&current->sig_pending, ~bits, __ATOMIC_SEQ_CST);
}

void signal_exit(int sig) {
    // Pending SIGKILL makes anything the exit waits for (a socket
    // closing, say) give up at once, as it did for the kill itself.
    __atomic_fetch_or(&current->sig_pending, SIGBIT(SIGKILL), __ATOMIC_SEQ_CST);
    task_exit(EXIT_SIGNAL(sig));
}

static int push(regs_t* r, int sig) {
    sigaction_t* a = &current->sig_action[sig];
    if (!arch_signal_push(r, (uintptr_t)a->handler, (uintptr_t)a->restorer, sig, current->sig_blocked)) return 0;
    current->sig_blocked |= (a->mask | (a->flags & SA_NODEFER ? 0 : SIGBIT(sig))) & ~UNBLOCKABLE & ALL_SIGNALS;
    if (a->flags & SA_RESETHAND) a->handler = SIG_DFL;
    return 1;
}

void signal_deliver(regs_t* r) {
    for (;;) {
        uint32_t ready = current->sig_pending & ~current->sig_blocked;
        if (!ready) return;
        int sig = __builtin_ctz(ready);
        clear_pending(SIGBIT(sig));
        sighandler_t h = sig == SIGKILL ? SIG_DFL : current->sig_action[sig].handler;
        if (h == SIG_IGN) continue;  // ignored after it was sent
        if (h == SIG_DFL) signal_exit(sig);
        if (!push(r, sig)) {
            kprintf_color(COLOR_ERROR, "%s: no room on the stack for a signal handler - task killed\n", current->name);
            signal_exit(SIGSEGV);
        }
        // The handler's frame is in place; one for any other signal
        // still ready goes on top, so that one's handler runs first.
    }
}

int signal_fault(regs_t* r, int sig) {
    // A fault the program cannot take, because it blocks or ignores the
    // signal or left it at its default, ends it: running on would only
    // fault again.
    sighandler_t h = current->sig_action[sig].handler;
    if (h == SIG_DFL || h == SIG_IGN || (current->sig_blocked & SIGBIT(sig))) return 0;
    return push(r, sig);
}

void signal_return(regs_t* r) {
    uint32_t mask;
    if (!arch_signal_pop(r, &mask)) {
        kprintf_color(COLOR_ERROR, "%s: bad signal frame - task killed\n", current->name);
        signal_exit(SIGSEGV);
    }
    current->sig_blocked = mask & ~UNBLOCKABLE & ALL_SIGNALS;
}

static int user_code(uintptr_t a) { return a >= USER_BASE && a < USER_TOP; }

long sys_sigaction(uintptr_t usig, uintptr_t uact, uintptr_t uold) {
    int sig = (int)usig;
    if (sig <= 0 || sig >= NSIG) return -EINVAL;
    sigaction_t act = { 0 };
    if (uact) {
        if (sig == SIGKILL) return -EINVAL;
        if (copy_from_user(&act, (const void*)uact, sizeof act)) return -EFAULT;
        // A handler the kernel jumps to must be in the user half (an
        // address past it would fault on the way out, in ring 0).
        // Where it returns to is the program's own affair.
        if (act.handler != SIG_DFL && act.handler != SIG_IGN && !user_code((uintptr_t)act.handler)) return -EFAULT;
        if (act.flags & ~(unsigned)(SA_RESETHAND | SA_NODEFER)) return -EINVAL;
        act.mask &= ALL_SIGNALS & ~UNBLOCKABLE;
    }
    if (uold && copy_to_user((void*)uold, &current->sig_action[sig], sizeof(sigaction_t))) return -EFAULT;
    if (uact) {
        current->sig_action[sig] = act;
        if (act.handler == SIG_IGN) clear_pending(SIGBIT(sig));  // one waiting is dropped too
    }
    return 0;
}

long sys_sigmask(uintptr_t how, uintptr_t umask, uintptr_t unused) {
    (void)unused;
    uint32_t mask = (uint32_t)umask & ALL_SIGNALS & ~UNBLOCKABLE, old = current->sig_blocked;
    switch (how) {
    case SIG_BLOCK: current->sig_blocked |= mask; break;
    case SIG_UNBLOCK: current->sig_blocked &= ~mask; break;
    case SIG_SETMASK: current->sig_blocked = mask; break;
    default: return -EINVAL;
    }
    return old;  // anything just unblocked and pending is acted on as this call returns
}
