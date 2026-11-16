// Signals: handlers, masks, ignoring, default actions, interrupted
// waits, SIGPIPE, faults a program catches, and a frame a handler
// tampers with. Children (this program again, with a role) are for
// what ends a program, or must come from outside it.
//
// `sigtest wait` and `sigtest stubborn` are for the shell tests: the
// first catches SIGINT and SIGUSR1, the second ignores SIGINT and
// SIGQUIT and has to be killed by a third Ctrl+backslash.
#include "ulib.h"

static void wait_ticks(uint32_t n) {
    uint32_t start = sys_ticks();
    while (sys_ticks() - start < n) sys_poll(1);
}

static int spawn_role(const char* const* argv) { return sys_spawn_args("sigtest", argv); }

// Run `sigtest role [arg]` and return how it ended.
static int child_status(const char* role, const char* arg) {
    const char* argv[] = { "sigtest", role, arg, 0 };
    int id = spawn_role(argv);
    return id < 0 ? id : sys_join(id);
}

// A child that sends `sig` to this task after `delay` ticks.
static int kill_me_later(int sig, int delay) {
    char pid[12], s[12], d[12];
    snprintf(pid, sizeof pid, "%d", sys_getpid());
    snprintf(s, sizeof s, "%d", sig);
    snprintf(d, sizeof d, "%d", delay);
    const char* argv[] = { "sigtest", "kill", pid, s, d, 0 };
    return spawn_role(argv);
}

static volatile int got[NSIG];
static volatile int last_sig;
static volatile uintptr_t entry_fp;
static volatile uint64_t entry_flags;

static void count(int sig) {
    got[sig]++;
    last_sig = sig;
    entry_fp = (uintptr_t)__builtin_frame_address(0);
    entry_flags = __builtin_ia32_readeflags_u64();
}

static int set(int sig, sighandler_t h, unsigned mask, unsigned flags) {
    sigaction_t a = { h, mask, flags, 0 };
    return sys_sigaction(sig, &a, 0);
}

// ---- Registers across an asynchronous handler ----
//
// regs_spin fills every general register (rcx and r11 too, which a
// SYSRET would trash), three SSE registers, DF and MXCSR's rounding
// with patterns, spins until spin_flag is set, and stores what is left
// in spin_out. The handler sets the flag and trashes every register a
// C function may.
volatile int spin_flag;
uint64_t spin_out[20];
void regs_spin(void);
__asm__(".text\n"
        "regs_spin:\n"
        "    push %rbx\n push %rbp\n push %r12\n push %r13\n push %r14\n push %r15\n"
        "    sub $8, %rsp\n"
        "    movl $0x7F80, (%rsp)\n"       // round toward zero, everything masked
        "    ldmxcsr (%rsp)\n"
        "    std\n"
        "    mov $0x1111111111111111, %rax\n"
        "    mov $0x2222222222222222, %rbx\n"
        "    mov $0x3333333333333333, %rcx\n"
        "    mov $0x4444444444444444, %rdx\n"
        "    mov $0x5555555555555555, %rsi\n"
        "    mov $0x6666666666666666, %rdi\n"
        "    mov $0x7777777777777777, %rbp\n"
        "    mov $0x8888888888888888, %r8\n"
        "    mov $0x9999999999999999, %r9\n"
        "    mov $0xAAAAAAAAAAAAAAAA, %r10\n"
        "    mov $0xBBBBBBBBBBBBBBBB, %r11\n"
        "    mov $0xCCCCCCCCCCCCCCCC, %r12\n"
        "    mov $0xDDDDDDDDDDDDDDDD, %r13\n"
        "    mov $0xEEEEEEEEEEEEEEEE, %r14\n"
        "    mov $0x0F0F0F0F0F0F0F0F, %r15\n"
        "    movq %rax, %xmm0\n movq %rbx, %xmm8\n movq %rcx, %xmm15\n"
        "1:  cmpl $0, spin_flag(%rip)\n"
        "    je 1b\n"
        "    mov %rax, spin_out+0(%rip)\n mov %rbx, spin_out+8(%rip)\n mov %rcx, spin_out+16(%rip)\n"
        "    mov %rdx, spin_out+24(%rip)\n mov %rsi, spin_out+32(%rip)\n mov %rdi, spin_out+40(%rip)\n"
        "    mov %rbp, spin_out+48(%rip)\n mov %r8, spin_out+56(%rip)\n mov %r9, spin_out+64(%rip)\n"
        "    mov %r10, spin_out+72(%rip)\n mov %r11, spin_out+80(%rip)\n mov %r12, spin_out+88(%rip)\n"
        "    mov %r13, spin_out+96(%rip)\n mov %r14, spin_out+104(%rip)\n mov %r15, spin_out+112(%rip)\n"
        "    movq %xmm0, %rax\n mov %rax, spin_out+120(%rip)\n"
        "    movq %xmm8, %rax\n mov %rax, spin_out+128(%rip)\n"
        "    movq %xmm15, %rax\n mov %rax, spin_out+136(%rip)\n"
        "    pushfq\n pop %rax\n mov %rax, spin_out+144(%rip)\n"
        "    cld\n"
        "    stmxcsr spin_out+152(%rip)\n"
        "    movl $0x1F80, (%rsp)\n"
        "    ldmxcsr (%rsp)\n"
        "    add $8, %rsp\n"
        "    pop %r15\n pop %r14\n pop %r13\n pop %r12\n pop %rbp\n pop %rbx\n"
        "    ret\n");

static volatile uint64_t trash_flags;

static void trash(int sig) {
    (void)sig;
    trash_flags = __builtin_ia32_readeflags_u64();
    unsigned zero = 0x1F80;
    __asm__ volatile("ldmxcsr %0\n"
                     "pxor %%xmm0, %%xmm0\n pxor %%xmm8, %%xmm8\n pcmpeqd %%xmm15, %%xmm15\n"
                     "xor %%eax, %%eax\n mov $-1, %%rcx\n xor %%edx, %%edx\n xor %%esi, %%esi\n xor %%edi, %%edi\n"
                     "xor %%r8d, %%r8d\n xor %%r9d, %%r9d\n xor %%r10d, %%r10d\n mov $-1, %%r11\n"
                     :: "m"(zero)
                     : "rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "xmm0", "xmm8", "xmm15", "cc");
    spin_flag = 1;
}

// ---- Handlers for the order of nested ones ----

static char order[16];
static volatile int order_n;
static void note(char c) { if (order_n < (int)sizeof order - 1) order[order_n++] = c; }

static volatile int outer_runs;
static void outer(int sig) {
    (void)sig;
    note('1');
    raise(SIGUSR2);                         // runs now, inside this one
    if (outer_runs++ == 0) raise(SIGUSR1);  // blocked while this runs: after it
    note('/');
}

static void inner(int sig) { (void)sig; note('2'); }

static volatile int depth, deepest;
static void recurse(int sig) {
    depth++;
    if (depth > deepest) deepest = depth;
    if (depth < 3) raise(sig);
    depth--;
}

// ---- Children ----

static void exit42(int sig) { (void)sig; sys_exit(42); }
static void exit43(int sig) { (void)sig; sys_exit(43); }
static void exit44(int sig) { (void)sig; sys_exit(44); }

static volatile int once;
static void mark(int sig) { (void)sig; once++; }

// The frame starts where the handler's return address is, just above
// its saved rbp; the registers are 32 bytes in (arch sigframe.c), rip
// the 18th of them and rflags the 20th.
static void to_kernel(int sig) {
    (void)sig;
    volatile uint64_t* frame = (volatile uint64_t*)((uintptr_t)__builtin_frame_address(0) + 8);
    frame[4 + 17] = 0xFFFFFFFF80100000ull;  // rip: the kernel's text
}

static void take_iopl(int sig) {
    (void)sig;
    volatile uint64_t* frame = (volatile uint64_t*)((uintptr_t)__builtin_frame_address(0) + 8);
    frame[4 + 19] |= 0x3000;  // rflags: IOPL 3, which would let cli run
}

static volatile int printed;
static void say(int sig) {
    printf("sigtest: got SIG%s\n", signame(sig));
    printed = sig;
}

static int child(int argc, char** argv) {
    const char* role = argv[1];
    if (!strcmp(role, "kill") && argc == 5) {
        uint32_t pid, sig, delay;
        if (parse_uint(argv[2], &pid) || parse_uint(argv[3], &sig) || parse_uint(argv[4], &delay)) return 2;
        wait_ticks(delay);
        return sys_kill((int)pid, (int)sig) == 0 ? 0 : 1;
    }
    if (!strcmp(role, "die") && argc == 3) {
        raise(signum(argv[2]));
        wait_ticks(5);  // in case it is not acted on at once
        return 1;
    }
    if (!strcmp(role, "killblocked")) {
        sys_sigmask(SIG_SETMASK, 0xFFFF);
        raise(SIGKILL);
        return 1;
    }
    if (!strcmp(role, "pipe")) {
        int p[2];
        sys_pipe(p);
        sys_close(p[0]);
        sys_write(p[1], "x", 1);
        return 1;
    }
    if (!strcmp(role, "segv")) {
        signal(SIGSEGV, exit42);
        *(volatile int*)8 = 1;
        return 1;
    }
    if (!strcmp(role, "segvblocked")) {
        signal(SIGSEGV, exit42);
        sys_sigmask(SIG_BLOCK, SIGBIT(SIGSEGV));
        *(volatile int*)8 = 1;
        return 1;
    }
    if (!strcmp(role, "fpe")) {
        signal(SIGFPE, exit43);
        volatile int zero = 0;
        return 100 / zero;
    }
    if (!strcmp(role, "ill")) {
        signal(SIGILL, exit44);
        __asm__ volatile("ud2");
        return 1;
    }
    if (!strcmp(role, "overflow")) {  // a SIGSEGV handler cannot help with no stack left
        signal(SIGSEGV, exit42);
        volatile char big[64 * 1024];
        big[0] = 1;
        return child(argc, argv) + big[0];
    }
    if (!strcmp(role, "resethand")) {
        set(SIGUSR1, mark, 0, SA_RESETHAND);
        raise(SIGUSR1);
        if (once != 1) return 6;
        raise(SIGUSR1);  // the default now: this ends it
        return 7;
    }
    if (!strcmp(role, "badrip")) {
        signal(SIGUSR1, to_kernel);
        raise(SIGUSR1);
        return 1;
    }
    if (!strcmp(role, "iopl")) {
        signal(SIGUSR1, take_iopl);
        raise(SIGUSR1);
        return (__builtin_ia32_readeflags_u64() & 0x3000) ? 1 : 0;
    }
    if (!strcmp(role, "wait")) {
        signal(SIGINT, say);
        signal(SIGUSR1, say);
        puts("sigtest: waiting");
        while (printed != SIGINT) sys_sleep(1);
        return 0;
    }
    if (!strcmp(role, "stubborn")) {
        signal(SIGINT, SIG_IGN);
        signal(SIGQUIT, SIG_IGN);
        puts("sigtest: stubborn");
        for (;;) sys_sleep(1);
    }
    return 2;
}

int main(int argc, char** argv) {
    if (argc >= 2) return child(argc, argv);

    // Setting actions.
    sigaction_t old;
    sigaction_t a = { count, 0, 0, 0 };
    check(sys_sigaction(0, &a, 0) == -EINVAL && sys_sigaction(NSIG, &a, 0) == -EINVAL, "signals are 1 to 15");
    check(sys_sigaction(SIGKILL, &a, 0) == -EINVAL && sys_sigaction(SIGKILL, 0, &old) == 0 && old.handler == SIG_DFL,
          "SIGKILL's action can be read but not set");
    sigaction_t bad = { (sighandler_t)0xFFFFFFFF80100000ull, 0, 0, 0 };
    check(sys_sigaction(SIGUSR1, &bad, 0) == -EFAULT, "a handler in the kernel is refused");
    sigaction_t badflags = { count, 0, 0x100, 0 };
    check(sys_sigaction(SIGUSR1, &badflags, 0) == -EINVAL, "and flags nobody knows");
    check(sys_sigaction(SIGUSR1, 0, &old) == 0 && old.handler == SIG_DFL, "every action starts at the default");
    check(signal(SIGUSR1, count) == SIG_DFL && signal(SIGUSR1, count) == count, "signal() gives back the handler before");

    // A handler, called by raise before it returns.
    check(raise(SIGUSR1) == 0 && got[SIGUSR1] == 1 && last_sig == SIGUSR1, "raise calls the handler, with the signal");
    check(entry_fp % 16 == 0, "with the stack aligned as after a call");
    check(!(entry_flags & 0x400), "and DF clear");
    check(sys_sigmask(SIG_BLOCK, 0) == 0, "and the mask back as it was after it returns");

    // Sending.
    check(sys_kill(sys_getpid(), 0) == 0 && sys_kill(30000, 0) == -ESRCH, "signal 0 only checks the task is there");
    check(sys_kill(sys_getpid(), 16) == -EINVAL && sys_kill(sys_getpid(), -1) == -EINVAL, "a signal past 15 is refused");
    check(sys_kill(1, SIGUSR1) == -EPERM, "kernel tasks get none");

    // Blocking and unblocking.
    check(sys_sigmask(SIG_BLOCK, SIGBIT(SIGUSR1)) == 0, "block SIGUSR1");
    raise(SIGUSR1);
    check(got[SIGUSR1] == 1, "a blocked signal waits");
    check(sys_sigmask(SIG_BLOCK, SIGBIT(SIGKILL)) == (int)SIGBIT(SIGUSR1) &&
          sys_sigmask(SIG_BLOCK, 0) == (int)SIGBIT(SIGUSR1), "SIGKILL cannot be blocked");
    check(sys_sigmask(SIG_UNBLOCK, SIGBIT(SIGUSR1)) == (int)SIGBIT(SIGUSR1) && got[SIGUSR1] == 2,
          "and runs as soon as it is unblocked");
    check(sys_sigmask(7, 0) == -EINVAL, "an unknown way to change the mask is refused");
    sys_sigmask(SIG_BLOCK, SIGBIT(SIGUSR1));
    raise(SIGUSR1);
    raise(SIGUSR1);
    sys_sigmask(SIG_UNBLOCK, SIGBIT(SIGUSR1));
    check(got[SIGUSR1] == 3, "two of one signal while blocked are one");

    // Ignoring.
    check(signal(SIGUSR2, SIG_IGN) == SIG_DFL && raise(SIGUSR2) == 0, "an ignored signal does nothing");
    sys_sigmask(SIG_BLOCK, SIGBIT(SIGUSR1));
    raise(SIGUSR1);
    signal(SIGUSR1, SIG_IGN);
    signal(SIGUSR1, count);
    sys_sigmask(SIG_UNBLOCK, SIGBIT(SIGUSR1));
    check(got[SIGUSR1] == 3, "ignoring a signal drops one already waiting");

    // Nesting.
    set(SIGUSR1, outer, 0, 0);
    set(SIGUSR2, inner, 0, 0);
    raise(SIGUSR1);
    order[order_n] = 0;
    check(!strcmp(order, "12/12/"), "a handler is interrupted by another signal, not by its own");
    set(SIGUSR1, recurse, 0, SA_NODEFER);
    raise(SIGUSR1);
    check(deepest == 3 && depth == 0, "unless SA_NODEFER");
    order_n = outer_runs = 0;
    set(SIGUSR1, outer, SIGBIT(SIGUSR2), 0);
    raise(SIGUSR1);
    order[order_n] = 0;
    check(!strcmp(order, "1/1/2"), "and sa_mask holds others back until it returns");
    set(SIGUSR2, SIG_DFL, 0, 0);

    // A signal from another task, into a spinning loop: every register,
    // the SSE ones and MXCSR and DF, back as they were.
    signal(SIGUSR1, trash);
    int k = kill_me_later(SIGUSR1, 10);
    regs_spin();
    int ok = 1;
    static const uint64_t want[] = {
        0x1111111111111111ull, 0x2222222222222222ull, 0x3333333333333333ull, 0x4444444444444444ull,
        0x5555555555555555ull, 0x6666666666666666ull, 0x7777777777777777ull, 0x8888888888888888ull,
        0x9999999999999999ull, 0xAAAAAAAAAAAAAAAAull, 0xBBBBBBBBBBBBBBBBull, 0xCCCCCCCCCCCCCCCCull,
        0xDDDDDDDDDDDDDDDDull, 0xEEEEEEEEEEEEEEEEull, 0x0F0F0F0F0F0F0F0Full,
        0x1111111111111111ull, 0x2222222222222222ull, 0x3333333333333333ull,
    };
    for (int i = 0; i < 18; i++)
        if (spin_out[i] != want[i]) {
            printf("  register %d is %016lx\n", i, (unsigned long)spin_out[i]);
            ok = 0;
        }
    check(k > 0 && sys_join(k) == 0, "another task sends a signal");
    check(ok, "a handler that trashes every register leaves the loop it interrupted as it was");
    check((spin_out[18] & 0x400) && !(trash_flags & 0x400), "DF is clear in the handler and set again after");
    check((spin_out[19] & 0xFFFF) == 0x7F80, "and MXCSR is the loop's own again");

    // Interrupted waits.
    signal(SIGUSR1, count);
    int before = got[SIGUSR1];
    uint32_t t0 = sys_ticks();
    k = kill_me_later(SIGUSR1, 20);
    int r = sys_sleep(5);
    check(r == -EINTR && got[SIGUSR1] == before + 1 && sys_ticks() - t0 < 200, "a signal cuts a sleep short with EINTR");
    sys_join(k);
    int p[2];
    sys_pipe(p);
    char c;
    k = kill_me_later(SIGUSR1, 20);
    check(sys_read(p[0], &c, 1) == -EINTR && got[SIGUSR1] == before + 2, "and a read from an empty pipe");
    sys_join(k);
    sys_sigmask(SIG_BLOCK, SIGBIT(SIGUSR1));
    k = kill_me_later(SIGUSR1, 10);
    t0 = sys_ticks();
    r = sys_sleep(1);
    check(r == 0 && sys_ticks() - t0 >= 90 && got[SIGUSR1] == before + 2, "a blocked one does not");
    sys_join(k);
    sys_sigmask(SIG_UNBLOCK, SIGBIT(SIGUSR1));
    check(got[SIGUSR1] == before + 3, "until it is unblocked");

    // SIGPIPE, caught: the write still fails.
    signal(SIGPIPE, count);
    sys_close(p[0]);
    check(sys_write(p[1], "x", 1) == -EPIPE && got[SIGPIPE] == 1, "a caught SIGPIPE, and EPIPE");
    sys_close(p[1]);

    // How children end.
    check(child_status("die", "TERM") == EXIT_SIGNAL(SIGTERM), "SIGTERM's default ends a program");
    check(child_status("die", "HUP") == EXIT_SIGNAL(SIGHUP) && EXIT_SIGNUM(EXIT_SIGNAL(SIGHUP)) == SIGHUP,
          "and the exit status names the signal");
    check(child_status("killblocked", 0) == EXIT_SIGNAL(SIGKILL), "SIGKILL ends it even with everything blocked");
    check(child_status("pipe", 0) == EXIT_SIGNAL(SIGPIPE), "writing to a pipe nobody reads sends SIGPIPE");
    check(child_status("resethand", 0) == EXIT_SIGNAL(SIGUSR1), "SA_RESETHAND puts the default back");
    check(child_status("segv", 0) == 42, "a program can catch its own bad memory access");
    check(child_status("fpe", 0) == 43, "and a division by zero");
    check(child_status("ill", 0) == 44, "and an invalid instruction");
    check(child_status("segvblocked", 0) == EXIT_SIGNAL(SIGSEGV), "but not with SIGSEGV blocked");
    check(child_status("overflow", 0) == EXIT_SIGNAL(SIGSEGV), "nor with no stack left for the handler");
    check(child_status("badrip", 0) == EXIT_SIGNAL(SIGSEGV), "a frame changed to return into the kernel ends the program");
    check(child_status("iopl", 0) == 0, "and one asking for IOPL 3 does not get it");

    check(EXIT_SIGNUM(0) == 0 && EXIT_SIGNUM(-1) == 0 && EXIT_SIGNUM(-256) == 0 && EXIT_SIGNUM(EXIT_SIGNAL(NSIG)) == 0,
          "an ordinary exit status names no signal");
    check(signum("INT") == SIGINT && signum("sigterm") == SIGTERM && signum("9") == SIGKILL && signum("BOGUS") == -1 &&
          signum("16") == -1 && !strcmp(signame(SIGSEGV), "SEGV"), "signal names");
    return check_summary("sigtest");
}
