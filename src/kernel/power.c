// Reboot and power off.
#include "kernel.h"
#include "arch.h"
#include "console.h"

void machine_reboot(void) {
    cli();
    // Pulse the CPU reset line through the keyboard controller.
    for (int i = 0; i < 100000 && (inb(0x64) & 0x02); i++) { }
    outb(0x64, 0xFE);
    for (volatile int i = 0; i < 1000000; i++) { }
    // Fallback: load an empty IDT and trap. With no handler for the
    // trap or the double fault it causes, the CPU triple faults and
    // resets.
    static const struct { uint16_t limit; uint32_t base; } __attribute__((packed)) none = { 0, 0 };
    __asm__ volatile("lidt %0; int3" :: "m"(none));
    for (;;) hlt();
}

void machine_poweroff(void) {
    cli();
    // There is no ACPI interpreter here, so try the fixed ports the
    // common emulators use: QEMU's PIIX4 PM block, Bochs and older
    // QEMU, VirtualBox. Then QEMU's isa-debug-exit device, which the
    // test harness attaches. On real hardware none of these work and
    // the machine just halts.
    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);
    outw(0x4004, 0x3400);
    outb(0xF4, 0x00);
    console_panic_mode();
    kprintf("It is now safe to turn off the computer.\n");
    for (;;) hlt();
}
