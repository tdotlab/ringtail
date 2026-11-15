// Starting the other processors, x86_64.
//
// At reset only the boot CPU runs; the firmware leaves the others
// waiting for an INIT and a startup IPI from it. The MADT lists them
// by APIC ID. The startup IPI sets one running in real mode at a page
// below 1 MB, where trampoline.asm has been copied; that takes it to
// 64-bit mode on page tables made for it, and to ap_main on a stack of
// its own. One CPU starts at a time, since they share the trampoline
// and its data block, and the boot CPU waits for each to say it is up
// before starting the next.
//
// Low memory is the BIOS's and the kernel's alone: the frame
// allocator starts at 1 MB, and nothing else is put there.
//
// A started CPU becomes the idle task of its own (task.c) and turns on
// its timer; from its first tick it takes tasks from the same list as
// the boot CPU, under the kernel lock (interrupt.c). The boot CPU
// holds that lock until it has finished starting the system, so the
// others wait for it to do so.
#include "kernel.h"
#include "arch.h"
#include "lib.h"
#include "mm.h"
#include "interrupt.h"
#include "drivers.h"
#include "task.h"
#include "arch/fpu.h"

#define TRAMPOLINE      0x8000  // trampoline.asm is assembled for it
#define TRAMPOLINE_PML4 0x9000
#define TRAMPOLINE_PDPT 0xA000
#define TRAMPOLINE_PD   0xB000

#define MSR_EFER  0xC0000080
#define EFER_LME  (1u << 8)
#define EFER_NXE  (1u << 11)
#define CR4_PAE   (1u << 5)
#define CR4_PGE   (1u << 7)

// The block at offset 8 of the trampoline.
typedef struct __attribute__((packed)) {
    uint32_t cr3, cr4, cr0, efer;
    uint64_t stack, entry, arg;
} trampoline_t;
#define TRAMPOLINE_DATA 8

extern const uint8_t ap_trampoline[], ap_trampoline_end[];

static struct {
    void* ist;
    task_t* idle;
    volatile int online;
} cpu_info[MAX_CPUS];
static int ncpus = 1;       // running, the boot CPU included
static int listed = 1;      // in the MADT
static char failed[48];     // why the rest were not started, if one failed
static uintptr_t boot_cr4;

int smp_cpus(void) { return ncpus; }

// A started CPU's first C, with interrupts off, on the trampoline's
// page tables (whose upper half is the kernel's) and its own stack.
// Its descriptor tables first, so a fault or an NMI from here on has
// somewhere to go, and gs, which this_cpu() reads; then the kernel's
// page tables and the boot CPU's CR4 (SMEP, SSE). Then it is this
// CPU's idle task, which halts until there is something to run; it
// never holds the kernel lock but in an interrupt.
static void __attribute__((noreturn)) ap_main(uint64_t index) {
    gdt_init_cpu((int)index, cpu_info[index].ist);
    idt_load_here();
    write_cr3(kernel_pd());
    write_cr4(boot_cr4);
    fpu_enable();
    apic_init_cpu();
    this_cpu()->apic_id = apic_id();
    task_idle_enter(cpu_info[index].idle);
    timer_init_cpu();
    __atomic_store_n(&cpu_info[index].online, 1, __ATOMIC_RELEASE);
    sti();
    for (;;) hlt();
}

static void wait_ms(uint32_t ms) {
    stopwatch_t w;
    stopwatch_start(&w);
    while (stopwatch_ms(&w) < ms) cpu_relax();
}

// The trampoline's code and page tables: the kernel's upper half, and
// 0 - 2 MB at 0 for the trampoline itself, in one large page. The CPU
// is in 32-bit mode when it loads CR3, so the tables sit below 4 GB,
// here with the code.
static void trampoline_init(void) {
    memcpy(phys_to_virt(TRAMPOLINE), ap_trampoline, (size_t)(ap_trampoline_end - ap_trampoline));
    uint64_t* pml4 = phys_to_virt(TRAMPOLINE_PML4);
    uint64_t* pdpt = phys_to_virt(TRAMPOLINE_PDPT);
    uint64_t* pd = phys_to_virt(TRAMPOLINE_PD);
    const uint64_t* kpml4 = phys_to_virt(kernel_pd());
    memset(pml4, 0, PAGE_SIZE);
    memset(pdpt, 0, PAGE_SIZE);
    memset(pd, 0, PAGE_SIZE);
    for (int i = 256; i < 512; i++) pml4[i] = kpml4[i];
    pml4[0] = TRAMPOLINE_PDPT | PTE_PRESENT | PTE_WRITE;
    pdpt[0] = TRAMPOLINE_PD | PTE_PRESENT | PTE_WRITE;
    pd[0] = 0 | PTE_PRESENT | PTE_WRITE | PTE_PS;

    trampoline_t* t = phys_to_virt(TRAMPOLINE + TRAMPOLINE_DATA);
    boot_cr4 = read_cr4();
    t->cr3 = TRAMPOLINE_PML4;
    t->cr4 = (uint32_t)(boot_cr4 & (CR4_PAE | CR4_PGE));
    t->cr0 = (uint32_t)read_cr0();
    t->efer = (uint32_t)(rdmsr(MSR_EFER) & EFER_NXE) | EFER_LME;
    t->entry = (uintptr_t)ap_main;
}

// The INIT, then two startup IPIs 1 ms apart, as Intel's sequence has
// it (a CPU that took the first ignores the second), then up to half
// a second for the CPU to say it is up. 0, or -EIO.
static int start_cpu(int index, uint32_t id) {
    trampoline_t* t = phys_to_virt(TRAMPOLINE + TRAMPOLINE_DATA);
    uint8_t* stack = kmalloc(KSTACK_SIZE);
    void* ist = kmalloc(IST_STACKS_SIZE);
    task_t* idle = stack ? task_idle_create(index, (uintptr_t)stack + KSTACK_SIZE) : 0;
    if (!stack || !ist || !idle) {
        kfree(stack);
        kfree(ist);
        kfree(idle);
        return -ENOMEM;
    }
    cpu_info[index].ist = ist;
    cpu_info[index].idle = idle;
    t->stack = (uintptr_t)stack + KSTACK_SIZE;
    t->arg = (uint64_t)index;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (apic_send_init(id) < 0) return -EIO;
    wait_ms(10);
    for (int i = 0; i < 2; i++) {
        if (apic_send_startup(id, TRAMPOLINE >> 12) < 0) return -EIO;
        wait_ms(1);
    }
    stopwatch_t w;
    stopwatch_start(&w);
    while (!__atomic_load_n(&cpu_info[index].online, __ATOMIC_ACQUIRE)) {
        // Its stacks stay allocated: it could still start late.
        if (stopwatch_ms(&w) >= 500) return -EIO;
        cpu_relax();
    }
    return 0;
}

void smp_init(void) {
    if (!irq_uses_apic()) return;
    const madt_t* m = acpi_madt();
    uint32_t self = apic_bsp_id();
    this_cpu()->apic_id = self;
    cpu_info[0].online = 1;
    listed = apic_cpus();
    if (listed < 2) return;
    // The time comes from the TSC when the CPUs are many: none of them
    // takes every tick.
    if (!timer_tsc_hz()) {
        snprintf(failed, sizeof failed, "the TSC was not measured");
        return;
    }
    trampoline_init();
    for (int i = 0; i < m->cpus; i++) {
        uint32_t id = m->cpu[i].apic_id;
        if (id == self || ncpus == MAX_CPUS) continue;
        int r = start_cpu(ncpus, id);
        if (r < 0) {
            // A CPU that did not answer may yet run the trampoline, so
            // it is not used to start another.
            snprintf(failed, sizeof failed, "APIC ID %u did not start", id);
            return;
        }
        ncpus++;
    }
}

// Called by whatever leaves a task ready, with interrupts off and the
// kernel lock: an idle CPU would otherwise not look until its next
// tick. One CPU per task made ready, and not one already sent for.
// The others' task fields change only under the lock, so they hold
// still while this reads them.
void smp_kick_idle(void) {
    if (ncpus < 2) return;
    cpu_t* me = this_cpu();
    for (int i = 0; i < ncpus; i++) {
        cpu_t* c = cpu_get(i);
        if (c == me || c->task != c->idle || c->kicked) continue;
        c->kicked = 1;
        apic_send_ipi(c->apic_id, RESCHED_VECTOR);
        return;
    }
}

const char* smp_describe(char* buf, uint32_t size) {
    if (failed[0]) snprintf(buf, size, "%d of %d started (%s)", ncpus, listed, failed);
    else if (ncpus < listed) snprintf(buf, size, "%d of %d started", ncpus, listed);
    else snprintf(buf, size, "%d", ncpus);
    return buf;
}

// One line per CPU, under the kernel lock: the others' counts move on
// meanwhile, and that is all.
void smp_format(sbuf_t* sb) {
    uint64_t hz = timer_tsc_hz();
    if (hz) sbprintf(sb, "TSC: %u.%u MHz\n", (unsigned)(hz / 1000000), (unsigned)(hz / 100000 % 10));
    sbprintf(sb, "CPU  APIC  TICKS     SWITCHES  RUNNING\n");
    uint32_t f = irq_save();
    for (int i = 0; i < ncpus; i++) {
        cpu_t* c = cpu_get(i);
        task_t* t = c->task;
        sbprintf(sb, "%3d  %4u  %-8lu  %-8lu  %s\n", i, c->apic_id, (unsigned long)c->ticks,
                 (unsigned long)c->switches, t ? t->name : "-");
    }
    irq_restore(f);
}
