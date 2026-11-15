// Device drivers.
#ifndef RINGTAIL_DRIVERS_H
#define RINGTAIL_DRIVERS_H

#include <stdint.h>
#include "ringtail/abi.h"  // rtc_time_t
#include "lib.h"           // sbuf_t

// ---- The tick (timer.c) ----
// 100 Hz from the local APIC's timer when the APIC is in use, from the
// PIT's channel 0 otherwise. The PIT runs either way: it is the
// reference the APIC timer and the TSC are measured against, and
// stopwatch_ms counts its wrap-arounds. With the APICs the count of
// ticks, and the time in microseconds, come from the TSC.
#define TIMER_HZ 100
void timer_init(void);
void timer_init_cpu(void);    // on each other CPU, as it starts
uint64_t timer_tsc_hz(void);  // 0 unless the TSC keeps the time
// "local APIC, 1000 MHz bus" and the like, for the banner.
const char* timer_source(char* buf, uint32_t size);
uint32_t timer_ticks(void);
// The same count without the wrap after 497 days, for deadlines that
// can be that far off (DHCP leases).
uint64_t timer_ticks64(void);
// Time since the tick started in microseconds, from the TSC, or from
// the tick count and the timer's own counter. Wraps after about 71
// minutes, so only differences mean anything.
uint32_t timer_micros(void);
// A clock for busy waits that also runs with interrupts off (the disk is
// read before they come on at boot). It counts the PIT counter's
// wrap-arounds the caller sees while it polls, and takes the tick count
// when that is larger. Both can only miss time, never invent it, so a
// wait timed by it may run long but never ends early. Poll at least once
// per tick (10 ms) for it to keep time with interrupts off.
typedef struct {
    uint32_t start_ticks;
    uint32_t last_count;
    uint32_t wraps;
} stopwatch_t;
void stopwatch_start(stopwatch_t* w);
uint32_t stopwatch_ms(stopwatch_t* w);

// ---- ACPI tables (acpi.c): the MADT, nothing that needs AML ----
#define MADT_MAX_CPUS      64
#define MADT_MAX_IOAPICS   8
#define MADT_MAX_OVERRIDES 16
#define MADT_MAX_NMIS      8
// Interrupt source override flags: polarity in bits 0-1, trigger mode
// in bits 2-3. 0 means "as the bus has it" (ISA: edge, active high).
#define MADT_POLARITY_HIGH 1
#define MADT_POLARITY_LOW  3
#define MADT_TRIGGER_EDGE  1
#define MADT_TRIGGER_LEVEL 3
typedef struct {
    uint8_t revision;     // the RSDP's: 0 for ACPI 1.0, 2 from 2.0 on
    char oem[7];
    uint64_t lapic_phys;
    uint32_t flags;       // bit 0: the machine has 8259s as well
    int cpus;             // enabled processors with an 8-bit APIC ID
    struct { uint8_t acpi_id, apic_id; } cpu[MADT_MAX_CPUS];
    int x2apic_cpus;      // enabled ones listed only with a 32-bit ID
    int ioapics;
    struct { uint8_t id; uint32_t phys, gsi_base; } ioapic[MADT_MAX_IOAPICS];
    // ISA interrupts not wired to the GSI of the same number, or not
    // edge triggered and active high.
    int overrides;
    struct { uint8_t irq; uint32_t gsi; uint16_t flags; } override[MADT_MAX_OVERRIDES];
    // Local APIC pins wired to NMI; acpi_id 0xFF means every processor.
    int nmis;
    struct { uint8_t acpi_id, lint; uint16_t flags; } nmi[MADT_MAX_NMIS];
} madt_t;
// Finds the RSDP and reads the MADT. Needs the device window (after
// paging_init). 0, or -ENOENT when there are no tables or no MADT.
int acpi_init(void);
const madt_t* acpi_madt(void);  // 0 without one

// ---- Local and I/O APICs (apic.c) ----
// Takes over from the 8259s when the MADT describes the APICs: enables
// this CPU's local APIC (spurious vector 0xFF, LINT1 as NMI), masks
// every I/O APIC input, and leaves the 8259s for the caller to mask.
// 0, or -ENOENT (-ENOMEM when the device window is full) with *why
// saying what was missing.
int apic_init(const char** why);
// Unmask ISA interrupt irq (0-15) on vector IRQ_BASE + irq, using the
// MADT's overrides for its GSI, polarity and trigger mode.
void apic_route(int irq);
void apic_eoi(void);
// Whether vector is requested but not yet taken (interrupts off).
int apic_pending(int vector);
uint32_t apic_id(void);        // this CPU's
uint32_t apic_bsp_id(void);
int apic_cpus(void);            // processors the MADT lists
// Another CPU's local APIC, set up as apic_init set up the boot CPU's
// (run on that CPU, once it has started).
void apic_init_cpu(void);
// Starting another CPU: INIT resets it to wait for a startup IPI,
// which sets it running in real mode at physical address page << 12.
// 0, or -EIO if its APIC never took the interrupt.
int apic_send_init(uint32_t id);
int apic_send_startup(uint32_t id, uint32_t page);
int apic_send_ipi(uint32_t id, int vector);  // a fixed interrupt on vector
void apic_nmi_others(void);                 // every CPU but this one
// The local APIC's timer, counting down at the bus clock divided by 16.
void apic_timer_oneshot(uint32_t count);
void apic_timer_periodic(uint32_t count, int vector);
void apic_timer_stop(void);
uint32_t apic_timer_count(void);

// ---- Real-time clock (CMOS) ----
void rtc_read(rtc_time_t* t);
// Sets the clock to t, which must be valid with a year from 2000 to 2099.
// The chip's next second starts half a second after the call.
void rtc_write(const rtc_time_t* t);

// ---- Serial port (COM1) ----
void serial_init(void);
void serial_enable_input(void);
int serial_present(void);
void serial_putc(char c);
void serial_write(const char* s, uint32_t len);

// ---- PS/2 keyboard ----
void keyboard_init(void);
// Raw scancode queue, filled only while graphics mode is active,
// for the graphics owner. Returns -1 when empty.
int keyboard_raw_get(void);
int keyboard_raw_pending(void);
void keyboard_raw_flush(void);

// ---- PS/2 mouse ----
int mouse_init(void);
void mouse_get(int* x, int* y, int* buttons);

// ---- ATA disk (primary master, PIO) ----
int ata_init(void);
int ata_present(void);
uint32_t ata_sectors(void);
int ata_read(uint32_t lba, uint32_t count, void* buf);
int ata_write(uint32_t lba, uint32_t count, const void* buf);
int ata_flush(void);

// ---- PCI ----
typedef struct {
    uint8_t bus, dev, fn;
    uint16_t vendor, device;
    uint8_t class_code, subclass, prog_if;
    uint8_t irq;         // interrupt line the firmware routed it to; 0xFF for none
    uint32_t bar[6];     // as read, flag bits included
} pci_dev_t;
void pci_init(void);     // scan every bus, once, at boot
int pci_count(void);
const pci_dev_t* pci_get(int i);
uint32_t pci_read32(const pci_dev_t* d, uint8_t off);
void pci_write32(const pci_dev_t* d, uint8_t off, uint32_t v);
void pci_enable(const pci_dev_t* d);  // memory and I/O decoding, bus mastering
// "Intel ethernet" and the like, in buf.
const char* pci_describe(const pci_dev_t* d, char* buf, uint32_t size);
void pci_format(sbuf_t* sb);          // the device list, for KINFO_PCI

// ---- Intel 8254x gigabit ethernet (e1000) ----
struct netdev;
// Finds and starts the first supported card. Returns it, or 0.
struct netdev* e1000_probe(void);

// ---- VGA graphics (mode 13h) ----
// One task at a time may own the display. Acquire switches to mode
// 13h, release switches back to text mode and restores the console.
int gfx_acquire(int task_id);   // 0, or -EBUSY
int gfx_release(int task_id);   // 0, or -EPERM if not the owner
int gfx_owner(void);            // owner task id, or -1
void gfx_task_exit(int task_id);
void gfx_emergency_text(void);  // for panic: back to text, no questions
void gfx_pixel(int x, int y, uint8_t color);
void gfx_clear(uint8_t color);
void gfx_line(int x0, int y0, int x1, int y1, uint8_t color);
void gfx_rect(int x, int y, int w, int h, uint8_t color, int filled);
void gfx_text(int x, int y, const char* s, uint8_t color);
void gfx_blit(const uint8_t* pixels);
const uint8_t* gfx_font(void);  // 256 glyphs x 16 rows
void gfx_init_font(void);       // read the BIOS font and turn off blink, while still in text mode

#endif
