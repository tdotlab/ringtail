// The local APIC (one per CPU: its interrupts, its timer, the EOI) and
// the I/O APICs (the board's interrupt lines, each routed to a vector
// and a CPU), as the ACPI MADT describes them. Only the boot CPU is
// set up; every routed interrupt goes to it.
#include "kernel.h"
#include "arch.h"
#include "lib.h"
#include "mm.h"
#include "console.h"
#include "interrupt.h"
#include "drivers.h"

#define MSR_APIC_BASE     0x1B
#define APIC_BASE_ENABLE  (1u << 11)
#define APIC_BASE_X2APIC  (1u << 10)

// Local APIC registers, as byte offsets into its 4 KB page.
#define LAPIC_ID          0x020
#define LAPIC_TPR         0x080
#define LAPIC_EOI         0x0B0
#define LAPIC_SVR         0x0F0
#define LAPIC_IRR         0x200  // 8 registers of 32 bits, 16 bytes apart
#define LAPIC_ESR         0x280
#define LAPIC_LVT_TIMER   0x320
#define LAPIC_LVT_LINT0   0x350
#define LAPIC_LVT_LINT1   0x360
#define LAPIC_LVT_ERROR   0x370
#define LAPIC_TIMER_INIT  0x380
#define LAPIC_TIMER_COUNT 0x390
#define LAPIC_TIMER_DIV   0x3E0

#define SVR_ENABLE        (1u << 8)
#define LVT_MASKED        (1u << 16)
#define LVT_PERIODIC      (1u << 17)
#define LVT_NMI           (4u << 8)  // delivery mode
#define LVT_ACTIVE_LOW    (1u << 13)
#define LVT_LEVEL         (1u << 15)
#define TIMER_DIV_16      0x3

// I/O APIC: an index register and a data window.
#define IOAPIC_REGSEL     0x00
#define IOAPIC_WIN        0x10
#define IOAPIC_VER        0x01
#define IOAPIC_REDIR      0x10  // two registers per input, low word first
#define REDIR_ACTIVE_LOW  (1u << 13)
#define REDIR_LEVEL       (1u << 15)
#define REDIR_MASKED      (1u << 16)

#define SPURIOUS_VECTOR   0xFF

static volatile uint32_t* lapic;
static uint32_t bsp_id;
static const madt_t* madt;

static struct {
    volatile uint32_t* regs;
    uint32_t gsi_base, inputs;
} ioapics[MADT_MAX_IOAPICS];
static int nioapics;

static uint32_t lapic_read(uint32_t reg) { return lapic[reg / 4]; }
static void lapic_write(uint32_t reg, uint32_t v) {
    lapic[reg / 4] = v;
    (void)lapic[LAPIC_ID / 4];  // reading back makes the write land before what follows
}

static uint32_t ioapic_read(volatile uint32_t* io, uint32_t reg) {
    io[IOAPIC_REGSEL / 4] = reg;
    return io[IOAPIC_WIN / 4];
}
static void ioapic_write(volatile uint32_t* io, uint32_t reg, uint32_t v) {
    io[IOAPIC_REGSEL / 4] = reg;
    io[IOAPIC_WIN / 4] = v;
}

uint32_t apic_id(void) { return lapic_read(LAPIC_ID) >> 24; }
int apic_cpus(void) { return madt ? madt->cpus + madt->x2apic_cpus : 1; }

// Local APIC pin wiring for NMI, from the MADT entries for this CPU or
// for every CPU. Without any, LINT1 is NMI, as on the PC since the MP
// specification's virtual wire mode.
static void set_nmi_pins(void) {
    int acpi_id = -1;
    for (int i = 0; i < madt->cpus; i++)
        if (madt->cpu[i].apic_id == bsp_id) acpi_id = madt->cpu[i].acpi_id;
    uint32_t lint[2] = { LVT_MASKED, LVT_NMI };
    int listed = 0;
    for (int i = 0; i < madt->nmis; i++) {
        if (madt->nmi[i].acpi_id != 0xFF && madt->nmi[i].acpi_id != acpi_id) continue;
        if (madt->nmi[i].lint > 1) continue;
        if (!listed) lint[1] = LVT_MASKED;  // the table says which, so only what it says
        listed = 1;
        // NMIs are edge triggered whatever the flags say; only the
        // polarity applies.
        uint32_t v = LVT_NMI;
        if ((madt->nmi[i].flags & 3) == MADT_POLARITY_LOW) v |= LVT_ACTIVE_LOW;
        lint[madt->nmi[i].lint] = v;
    }
    lapic_write(LAPIC_LVT_LINT0, lint[0]);
    lapic_write(LAPIC_LVT_LINT1, lint[1]);
}

int apic_init(const char** why) {
    uint32_t a, b, c, d;
    cpuid(1, &a, &b, &c, &d);
    if (!(d & (1u << 9))) { *why = "no local APIC"; return -ENOENT; }
    if (acpi_init() < 0) { *why = "no ACPI MADT"; return -ENOENT; }
    madt = acpi_madt();
    if (!madt->ioapics) { *why = "no I/O APIC in the MADT"; return -ENOENT; }

    uint64_t base = rdmsr(MSR_APIC_BASE);
    // The firmware may have put the APIC in x2APIC mode (MSRs, not
    // memory); leaving it needs the APIC disabled first, and a machine
    // that needed it has CPUs this kernel could not address anyway.
    if (base & APIC_BASE_X2APIC) { *why = "local APIC in x2APIC mode"; return -ENOENT; }
    uintptr_t phys = (uintptr_t)(base & ~0xFFFull & 0xFFFFFFFFFFull);
    lapic = mmio_map(phys, PAGE_SIZE);
    for (int i = 0; i < madt->ioapics && nioapics < MADT_MAX_IOAPICS; i++) {
        volatile uint32_t* io = mmio_map(madt->ioapic[i].phys, 0x20);
        if (!io) continue;
        ioapics[nioapics].regs = io;
        ioapics[nioapics].gsi_base = madt->ioapic[i].gsi_base;
        ioapics[nioapics].inputs = ((ioapic_read(io, IOAPIC_VER) >> 16) & 0xFF) + 1;
        nioapics++;
    }
    if (!lapic || !nioapics) { *why = "no room to map the APICs"; return -ENOMEM; }
    wrmsr(MSR_APIC_BASE, base | APIC_BASE_ENABLE);
    bsp_id = apic_id();

    // Every input masked until a driver asks for it.
    for (int i = 0; i < nioapics; i++)
        for (uint32_t pin = 0; pin < ioapics[i].inputs; pin++) {
            ioapic_write(ioapics[i].regs, IOAPIC_REDIR + 2 * pin, REDIR_MASKED);
            ioapic_write(ioapics[i].regs, IOAPIC_REDIR + 2 * pin + 1, 0);
        }

    lapic_write(LAPIC_TPR, 0);  // accept every priority
    lapic_write(LAPIC_LVT_TIMER, LVT_MASKED);
    lapic_write(LAPIC_LVT_ERROR, LVT_MASKED);
    set_nmi_pins();
    lapic_write(LAPIC_ESR, 0);  // writes clear it; two, as some parts want
    lapic_write(LAPIC_ESR, 0);
    lapic_write(LAPIC_SVR, SVR_ENABLE | SPURIOUS_VECTOR);
    lapic_write(LAPIC_EOI, 0);  // anything left in service from the firmware
    return 0;
}

// The 8259s' edge/level control registers: the firmware sets the bit
// for each ISA IRQ it gave to a PCI device, which shares its line and
// holds it asserted (low) until served.
static int elcr_level(int irq) {
    return (inb(irq < 8 ? 0x4D0 : 0x4D1) >> (irq & 7)) & 1;
}

void apic_route(int irq) {
    uint32_t gsi = (uint32_t)irq;
    // ISA is edge triggered and active high; a PCI interrupt on an ISA
    // IRQ is level triggered and active low.
    int level = elcr_level(irq), low = level;
    for (int i = 0; i < madt->overrides; i++) {
        if (madt->override[i].irq != irq) continue;
        gsi = madt->override[i].gsi;
        uint16_t f = madt->override[i].flags;
        if ((f & 3) == MADT_POLARITY_HIGH) low = 0;
        if ((f & 3) == MADT_POLARITY_LOW) low = 1;
        if ((f >> 2 & 3) == MADT_TRIGGER_EDGE) level = 0;
        if ((f >> 2 & 3) == MADT_TRIGGER_LEVEL) level = 1;
    }
    for (int i = 0; i < nioapics; i++) {
        if (gsi < ioapics[i].gsi_base || gsi >= ioapics[i].gsi_base + ioapics[i].inputs) continue;
        uint32_t pin = gsi - ioapics[i].gsi_base;
        // Fixed delivery to the boot CPU's APIC ID.
        uint32_t lo = (uint32_t)(IRQ_BASE + irq);
        if (low) lo |= REDIR_ACTIVE_LOW;
        if (level) lo |= REDIR_LEVEL;
        ioapic_write(ioapics[i].regs, IOAPIC_REDIR + 2 * pin + 1, bsp_id << 24);
        ioapic_write(ioapics[i].regs, IOAPIC_REDIR + 2 * pin, lo);
        return;
    }
    kprintf_color(COLOR_ERROR, "apic: IRQ %d (GSI %u) is on no I/O APIC\n", irq, gsi);
}

// A level-triggered interrupt's EOI also goes to the I/O APIC that
// raised it (the local APIC broadcasts it), which then looks at the
// line again.
void apic_eoi(void) { lapic[LAPIC_EOI / 4] = 0; }

int apic_pending(int vector) {
    return (lapic_read(LAPIC_IRR + (uint32_t)(vector / 32) * 0x10) >> (vector % 32)) & 1;
}

void apic_timer_oneshot(uint32_t count) {
    lapic_write(LAPIC_TIMER_DIV, TIMER_DIV_16);
    lapic_write(LAPIC_LVT_TIMER, LVT_MASKED);
    lapic_write(LAPIC_TIMER_INIT, count);
}

void apic_timer_periodic(uint32_t count, int vector) {
    lapic_write(LAPIC_TIMER_DIV, TIMER_DIV_16);
    lapic_write(LAPIC_LVT_TIMER, LVT_PERIODIC | (uint32_t)vector);
    lapic_write(LAPIC_TIMER_INIT, count);
}

void apic_timer_stop(void) {
    lapic_write(LAPIC_LVT_TIMER, LVT_MASKED);
    lapic_write(LAPIC_TIMER_INIT, 0);
}

uint32_t apic_timer_count(void) { return lapic_read(LAPIC_TIMER_COUNT); }
