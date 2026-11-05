// PIT channel 0 at 100 Hz: the clock for sleeping and the
// scheduler's time slice.
#include "kernel.h"
#include "arch.h"
#include "interrupt.h"
#include "drivers.h"

#define PIT_CH0 0x40
#define PIT_CMD 0x43
#define PIT_DIVISOR 11932  // 1193182 Hz / 100, rounded

static volatile uint32_t ticks;
static volatile uint64_t ticks64;

static void timer_irq(void) {
    ticks++;
    ticks64++;
    need_resched = 1;  // one tick is one time slice
}

void timer_init(void) {
    // Rate generator rather than square wave: same interrupts, but the
    // counter runs down once per tick, so timer_micros can read it.
    outb(PIT_CMD, 0x34);  // channel 0, lo/hi byte, mode 2
    outb(PIT_CH0, PIT_DIVISOR & 0xFF);
    outb(PIT_CH0, PIT_DIVISOR >> 8);
    irq_register(IRQ_TIMER, timer_irq);
}

uint32_t timer_ticks(void) { return ticks; }

uint64_t timer_ticks64(void) {
    uint32_t f = irq_save();  // two words, so not while the timer adds one
    uint64_t t = ticks64;
    irq_restore(f);
    return t;
}

uint32_t timer_micros(void) {
    uint32_t f = irq_save();
    outb(PIT_CMD, 0x00);  // latch channel 0's count
    uint32_t count = inb(PIT_CH0);
    count |= (uint32_t)inb(PIT_CH0) << 8;
    uint32_t t = ticks;
    // The counter may have rolled over with its interrupt not yet
    // taken (interrupts are off): then the tick count is one behind.
    outb(0x20, 0x0A);     // master PIC: read the request register
    int pending = inb(0x20) & 1;
    irq_restore(f);
    uint32_t done = count > PIT_DIVISOR ? 0 : PIT_DIVISOR - count;
    if (pending && done < PIT_DIVISOR / 2) t++;
    return t * (1000000u / TIMER_HZ) + done * (1000000u / TIMER_HZ) / PIT_DIVISOR;
}
