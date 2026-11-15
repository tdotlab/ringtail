// The two ways into the kernel, syscall and int 0x80, and what each
// gives back. Every register is loaded with a pattern, or a call's
// arguments, a getcwd is made, and every register is compared after:
// the kernel may change rax (the result) and, after syscall, rcx and
// r11, which the CPU itself fills with the return address and the
// flags. getcwd does enough in the kernel to use the scratch registers,
// and copies with rep movsb, so the direction flag, set going in, has
// to be clear in the kernel (the path must arrive whole, and nothing
// around it be touched) and set again coming out.
//
// Under QEMU's TCG, SYSCALL clears the direction flag whatever FMASK
// says, so there only int 0x80's cld is really put to the test.
//
// "sysregs" checks both ways once; "sysregs loop S" makes calls both
// ways for S seconds, checking every one (the suite sends NMIs
// meanwhile); "sysregs tf" makes a syscall with the trap flag set,
// which must kill the program with a debug exception in ring 3, not
// trap in the kernel; "sysregs time" prints cycles per call, both ways.
#include "ulib.h"

enum { RAX, RBX, RCX, RDX, RSI, RDI, RBP, R8, R9, R10, R11, R12, R13, R14, R15, RSP, NREGS };
static const char* const names[NREGS] = {"rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "r8",
                                         "r9", "r10", "r11", "r12", "r13", "r14", "r15", "rsp"};

// Written by probe below; global so the assembly can name them.
uint64_t probe_out[NREGS];
uint64_t probe_args[4];  // rax, rdi, rsi, rdx going in
uint64_t probe_flags_in, probe_flags_out, probe_rsp_in, probe_how;
extern char probe_after_syscall[];
void probe(void);

__asm__(
    ".intel_syntax noprefix\n"
    ".text\n"
    ".globl probe\n"
    "probe:\n"
    "    push rbx\n push rbp\n push r12\n push r13\n push r14\n push r15\n"
    "    mov [rip + probe_rsp_in], rsp\n"
    "    mov rax, [rip + probe_args]\n"
    "    mov rdi, [rip + probe_args + 8]\n"
    "    mov rsi, [rip + probe_args + 16]\n"
    "    mov rdx, [rip + probe_args + 24]\n"
    "    movabs rbx, 0x1032547698badcfe\n"
    "    movabs rcx, 0x2301674523016745\n"
    "    movabs rbp, 0x67452301efcdab89\n"
    "    movabs r8,  0x7654321076543210\n"
    "    movabs r9,  0x89abcdef89abcdef\n"
    "    movabs r10, 0x98badcfe98badcfe\n"
    "    movabs r11, 0xab89efcdab89efcd\n"
    "    movabs r12, 0xba98fedcba98fedc\n"
    "    movabs r13, 0xcdef89abcdef89ab\n"
    "    movabs r14, 0xdcfe98badcfe98ba\n"
    "    movabs r15, 0xefcdab89efcdab89\n"
    "    std\n"
    "    cmp qword ptr [rip + probe_how], 1\n"
    "    je 2f\n"
    "    ja 3f\n"
    "    pushfq\n pop qword ptr [rip + probe_flags_in]\n"
    "    syscall\n"
    ".globl probe_after_syscall\n"
    "probe_after_syscall:\n"
    "    jmp 4f\n"
    "2:  pushfq\n pop qword ptr [rip + probe_flags_in]\n"
    "    int 0x80\n"
    "    jmp 4f\n"
    "3:  pushfq\n"                       // the trap flag, then syscall
    "    or qword ptr [rsp], 0x100\n"
    "    popfq\n"
    "    syscall\n"
    "    nop\n"
    "4:  pushfq\n pop qword ptr [rip + probe_flags_out]\n"
    "    cld\n"
    "    mov [rip + probe_out + 0], rax\n"
    "    mov [rip + probe_out + 8], rbx\n"
    "    mov [rip + probe_out + 16], rcx\n"
    "    mov [rip + probe_out + 24], rdx\n"
    "    mov [rip + probe_out + 32], rsi\n"
    "    mov [rip + probe_out + 40], rdi\n"
    "    mov [rip + probe_out + 48], rbp\n"
    "    mov [rip + probe_out + 56], r8\n"
    "    mov [rip + probe_out + 64], r9\n"
    "    mov [rip + probe_out + 72], r10\n"
    "    mov [rip + probe_out + 80], r11\n"
    "    mov [rip + probe_out + 88], r12\n"
    "    mov [rip + probe_out + 96], r13\n"
    "    mov [rip + probe_out + 104], r14\n"
    "    mov [rip + probe_out + 112], r15\n"
    "    mov [rip + probe_out + 120], rsp\n"
    "    mov rsp, [rip + probe_rsp_in]\n"
    "    pop r15\n pop r14\n pop r13\n pop r12\n pop rbp\n pop rbx\n"
    "    ret\n"
    ".att_syntax prefix\n");

static uint64_t loaded[NREGS] = {
    0, 0x1032547698badcfe, 0x2301674523016745, 0, 0, 0, 0x67452301efcdab89, 0x7654321076543210, 0x89abcdef89abcdef,
    0x98badcfe98badcfe, 0xab89efcdab89efcd, 0xba98fedcba98fedc, 0xcdef89abcdef89ab,
    0xdcfe98badcfe98ba, 0xefcdab89efcdab89, 0};

#define DF 0x400

// getcwd's buffer, with guard bytes either side.
static char area[96];
#define GUARD 16
#define BUF (area + GUARD)
#define BUFLEN (sizeof area - 2 * GUARD)

// One call; the first register that is wrong (NREGS for the flags or
// the copy), or -1. Quiet unless told.
static int one(int how, int loud, long cwdlen) {
    memset(area, 0x5A, sizeof area);
    probe_args[0] = SYS_GETCWD;
    probe_args[1] = (uintptr_t)BUF;
    probe_args[2] = BUFLEN;
    probe_args[3] = 0x3210fedc3210fedc;
    loaded[RAX] = probe_args[0];
    loaded[RDI] = probe_args[1];
    loaded[RSI] = probe_args[2];
    loaded[RDX] = probe_args[3];
    probe_how = (uint64_t)how;
    probe();
    int bad = -1;
    for (int i = 0; i < NREGS; i++) {
        uint64_t want = i == RSP ? probe_rsp_in : loaded[i];
        if (i == RAX) want = (uint64_t)cwdlen;
        if (how == 0 && i == RCX) want = (uintptr_t)probe_after_syscall;
        if (how == 0 && i == R11) want = probe_flags_in;
        if (probe_out[i] != want) {
            if (loud) printf("  %s is %016lx, wanted %016lx\n", names[i], (unsigned long)probe_out[i],
                             (unsigned long)want);
            if (bad < 0) bad = i;
        }
    }
    if (!(probe_flags_out & DF)) {
        if (loud) printf("  the direction flag was cleared\n");
        if (bad < 0) bad = NREGS;
    }
    int guarded = 1;
    for (int i = 0; i < GUARD; i++)
        if (area[i] != 0x5A || area[sizeof area - 1 - i] != 0x5A) guarded = 0;
    char cwd[BUFLEN];
    sys_getcwd(cwd, sizeof cwd);
    if (!guarded || strcmp(BUF, cwd) != 0) {
        if (loud) printf("  getcwd gave \"%.20s\", wanted \"%s\"%s\n", BUF, cwd, guarded ? "" : ", and wrote past it");
        if (bad < 0) bad = NREGS;
    }
    return bad;
}

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (uint64_t)hi << 32 | lo;
}

int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "tf") == 0) {
        probe_args[0] = SYS_GETPID;
        probe_how = 2;
        probe();
        puts("sysregs: the trap flag did not trap");
        return 1;
    }
    if (argc > 1 && strcmp(argv[1], "time") == 0) {
        enum { N = 20000 };
        uint64_t t0 = rdtsc();
        for (int i = 0; i < N; i++) sys_getpid();
        uint64_t t1 = rdtsc();
        for (int i = 0; i < N; i++) {
            long r;
            __asm__ volatile("int $0x80" : "=a"(r) : "a"(SYS_GETPID) : "memory");
        }
        uint64_t t2 = rdtsc();
        printf("syscall:  %lu cycles a call\n", (unsigned long)((t1 - t0) / N));
        printf("int 0x80: %lu cycles a call\n", (unsigned long)((t2 - t1) / N));
        return 0;
    }
    char cwd[BUFLEN];
    long cwdlen = sys_getcwd(cwd, sizeof cwd);
    if (argc > 2 && strcmp(argv[1], "loop") == 0) {
        uint32_t secs = 0;
        if (parse_uint(argv[2], &secs) != 0 || secs == 0 || secs > 60) secs = 1;
        uint32_t end = (uint32_t)sys_ticks() + secs * 100, calls = 0, bad = 0;
        while ((int32_t)(end - (uint32_t)sys_ticks()) > 0)
            for (int how = 0; how < 2; how++, calls++)
                if (one(how, 0, cwdlen) >= 0) bad++;
        printf("%u calls\n", calls);
        check(bad == 0, "every call, both ways, gave every register back");
        return check_summary("sysregs");
    }
    check(one(0, 1, cwdlen) < 0, "syscall keeps every register but rax, rcx and r11");
    check(probe_out[RCX] == (uintptr_t)probe_after_syscall && probe_out[R11] == probe_flags_in,
          "and gives the return address in rcx and the flags in r11");
    check(one(1, 1, cwdlen) < 0, "int 0x80 keeps every register, rcx and r11 included, but rax");
    check(cwdlen > 0 && (long)probe_out[RAX] == cwdlen, "both return the result in rax");
    return check_summary("sysregs");
}
