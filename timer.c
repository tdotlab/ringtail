// RingTail - v0.2.2 Piece 11: PIT timer
// The OS finally has a notion of time: a tick counter at ~100 Hz.
// IRQ0 (PIT channel 0) -> interrupt 32, same ISR pattern as the keyboard.

#include <stdint.h>

#define PIT_CHANNEL0 0x40
#define PIT_COMMAND  0x43
#define PIT_HZ       1193182
#define TICKS_PER_SEC 100
#define PIT_DIVISOR  (PIT_HZ / TICKS_PER_SEC) // 11931, close enough to 10 ms

static volatile uint32_t ticks = 0;

// Set while shell_sleep() waits. The keyboard handler checks this and
// drops keys, so a keypress during sleep cannot re-enter the shell.
// The shell runs inside the keyboard interrupt handler (see below),
// so this guard is what makes sleep safe.
volatile int timer_sleeping = 0;

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

// Called from timer_isr (isr.asm), like keyboard_handler is from keyboard_isr.
void timer_handler(void) {
    ticks++;
    outb(0x20, 0x20); // EOI to master PIC only: IRQ0 lives on the master
}

void timer_init(void) {
    // Channel 0, lobyte/hibyte, mode 3 (square wave), binary counter
    outb(PIT_COMMAND, 0x36);
    outb(PIT_CHANNEL0, PIT_DIVISOR & 0xFF);
    outb(PIT_CHANNEL0, (PIT_DIVISOR >> 8) & 0xFF);
    // Ticks do not actually advance until the kernel runs sti;
    // the PIT just starts counting, and the first IRQ stays pending.
}

uint32_t timer_get_ticks(void) {
    return ticks;
}

// Wait `seconds` seconds. Must be called with interrupts off, as the
// shell is (it runs inside the keyboard ISR): we re-enable them so the
// timer can fire, halt until each tick, then disable them again before
// returning, restoring the interrupt state the shell caller expects.
// iret at the end of the keyboard ISR then restores IF to what it was
// in the idle loop (enabled).
void timer_sleep(uint32_t seconds) {
    uint32_t target = ticks + seconds * TICKS_PER_SEC;
    timer_sleeping = 1;
    __asm__ volatile ("sti");
    while (ticks < target) {
        __asm__ volatile ("hlt");
    }
    __asm__ volatile ("cli");
    timer_sleeping = 0;
}
