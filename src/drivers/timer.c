// The 100 Hz tick: the clock for sleeping and the scheduler's time
// slice. It comes from the local APIC's timer when the APICs are in
// use (each CPU has one, and it needs no I/O port to read), and from
// PIT channel 0 otherwise. The PIT runs in both cases.
#include "kernel.h"
#include "arch.h"
#include "lib.h"
#include "interrupt.h"
#include "drivers.h"

#define PIT_CH0 0x40
#define PIT_CMD 0x43
#define PIT_DIVISOR 11932  // 1193182 Hz / 100, rounded

static volatile uint32_t ticks;
static volatile uint64_t ticks64;
// Local APIC timer counts per tick, or 0 when the PIT is the tick.
static uint32_t apic_period;
static const char* pit_why;  // why not the APIC timer, when the APIC is in use

static void timer_irq(void) {
    ticks++;
    ticks64++;
    need_resched = 1;  // one tick is one time slice
}

static uint32_t pit_count(void);

// Returns once the PIT has gone round (its count went up), or 0 when
// it has not after far longer than a tick.
static int pit_wrap(void) {
    uint32_t last = pit_count();
    for (uint32_t i = 0; i < 10000000; i++) {
        uint32_t c = pit_count();
        if (c > last) return 1;
        last = c;
    }
    return 0;
}

#define CALIBRATE_TICKS 3

// The APIC timer runs at the bus clock (over 16 here), which nothing
// tells us, so it is counted against the PIT over a few of its periods.
// Interrupts are off; the PIT's wrap-arounds are seen by polling. Both
// ends are read the same way, straight after a wrap is seen, so the
// delay in seeing one cancels out (under emulation the first pass
// through new code is slow, which would otherwise come off the count).
static uint32_t measure(void) {
    if (!pit_wrap()) return 0;
    uint32_t start = apic_timer_count();
    for (int i = 0; i < CALIBRATE_TICKS; i++)
        if (!pit_wrap()) return 0;
    uint32_t end = apic_timer_count();
    return (start - end + CALIBRATE_TICKS / 2) / CALIBRATE_TICKS;
}

// The middle of three measurements, 90 ms in all: an emulator's host
// can stall the guest at the wrong moment, and that spoils one of them,
// not two.
static uint32_t calibrate(void) {
    apic_timer_oneshot(0xFFFFFFFFu);
    uint32_t a = measure(), b = measure(), c = measure();
    apic_timer_stop();
    if (a > b) { uint32_t x = a; a = b; b = x; }
    if (b > c) b = c;
    return a > b ? a : b;
}

void timer_init(void) {
    // Rate generator rather than square wave: same interrupts, but the
    // counter runs down once per tick, so timer_micros can read it.
    outb(PIT_CMD, 0x34);  // channel 0, lo/hi byte, mode 2
    outb(PIT_CH0, PIT_DIVISOR & 0xFF);
    outb(PIT_CH0, PIT_DIVISOR >> 8);
    if (irq_uses_apic()) {
        uint32_t period = calibrate();
        // Under 1 MHz after the divide by 16 would make a poor clock, and
        // means the count went wrong more likely than the bus is slow.
        if (period >= 10000) {
            apic_period = period;
            // On the same vector the PIT would use, so the rest of the
            // kernel cannot tell; the PIT's own input stays masked.
            irq_set_handler(IRQ_TIMER, timer_irq);
            apic_timer_periodic(apic_period, IRQ_BASE + IRQ_TIMER);
            return;
        }
        pit_why = "the local APIC timer did not count";
    }
    irq_register(IRQ_TIMER, timer_irq);
}

const char* timer_source(char* buf, uint32_t size) {
    if (apic_period) {
        uint32_t khz = (uint32_t)((uint64_t)apic_period * 16 * TIMER_HZ / 1000);
        snprintf(buf, size, "local APIC, %u.%u MHz bus", khz / 1000, khz % 1000 / 100);
    } else {
        if (pit_why) snprintf(buf, size, "PIT (%s)", pit_why);
        else snprintf(buf, size, "PIT");
    }
    return buf;
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
    uint32_t count, period;
    if (apic_period) {
        count = apic_timer_count();
        period = apic_period;
    } else {
        outb(PIT_CMD, 0x00);  // latch channel 0's count
        count = inb(PIT_CH0);
        count |= (uint32_t)inb(PIT_CH0) << 8;
        period = PIT_DIVISOR;
    }
    uint32_t t = ticks;
    // The counter may have rolled over with its interrupt not yet
    // taken (interrupts are off): then the tick count is one behind.
    int pending = irq_pending(IRQ_TIMER);
    irq_restore(f);
    uint32_t done = count > period ? 0 : period - count;
    if (pending && done < period / 2) t++;
    return t * (1000000u / TIMER_HZ) + (uint32_t)((uint64_t)done * (1000000u / TIMER_HZ) / period);
}

static uint32_t pit_count(void) {
    uint32_t f = irq_save();  // the latch and both reads in one piece
    outb(PIT_CMD, 0x00);
    uint32_t count = inb(PIT_CH0);
    count |= (uint32_t)inb(PIT_CH0) << 8;
    irq_restore(f);
    return count;
}

void stopwatch_start(stopwatch_t* w) {
    w->start_ticks = ticks;
    w->last_count = pit_count();
    w->wraps = 0;
}

uint32_t stopwatch_ms(stopwatch_t* w) {
    // Mode 2 counts down and reloads, so a count above the last one
    // seen means it went round (at least once) in between.
    uint32_t count = pit_count();
    if (count > w->last_count) w->wraps++;
    w->last_count = count;
    uint32_t t = ticks - w->start_ticks;
    if (w->wraps > t) t = w->wraps;
    return t * (1000u / TIMER_HZ);
}
