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

static void timer_irq(void) {
    ticks++;
    need_resched = 1;  // one tick is one time slice
}

void timer_init(void) {
    outb(PIT_CMD, 0x36);  // channel 0, lo/hi byte, square wave
    outb(PIT_CH0, PIT_DIVISOR & 0xFF);
    outb(PIT_CH0, PIT_DIVISOR >> 8);
    irq_register(IRQ_TIMER, timer_irq);
}

uint32_t timer_ticks(void) { return ticks; }
