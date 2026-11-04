// The OS finally has a notion of time: a tick counter at ~100 Hz.
// IRQ0 (PIT channel 0) -> interrupt 32, same ISR pattern as the keyboard.

#include <stdint.h>

#define PIT_CHANNEL0 0x40
#define PIT_COMMAND  0x43
#define PIT_HZ       1193182
#define TICKS_PER_SEC 100
#define PIT_DIVISOR  11932 // nearest integer to 1193182/100: 99.998 Hz, error under 0.002% (11931 was 100.007 Hz)

static volatile uint32_t ticks = 0;

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

extern void task_preempt(void);

// Called from timer_isr (isr.asm), like keyboard_handler is from keyboard_isr.
// v0.5.0: the EOI goes out BEFORE the preemptive switch, so the PIC
// can deliver the next tick while the newly switched-in task runs.
// If the EOI waited until the old task resumed, ticks would stall
// for as long as the old task was parked, and sleeps would lag.
// task_preempt runs with interrupts re-enabled (timer_isr does sti
// before calling here), so a tick during the pick is masked only by
// task_preempt's own cli, and fires as soon as it sti's again.
void timer_handler(void) {
    ticks++;
    outb(0x20, 0x20); // EOI to master PIC only: IRQ0 lives on the master
    task_preempt();
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

// Longest sleep whose tick count still fits in half the 32-bit
// range: 21474836 seconds, about 248 days. Longer than that, the
// wrap-safe elapsed comparison below stops being correct.
#define MAX_SLEEP_SECONDS (0x7FFFFFFFu / TICKS_PER_SEC)

// Wait `seconds` seconds. Returns 0 on success, -1 if the duration
// is too large. v0.3.0: the old code computed target = ticks +
// seconds * 100 with no overflow check, so sleep 42949673 became a
// 4-tick sleep, and the ticks < target comparison broke across the
// 497-day tick wraparound. The elapsed comparison (ticks - start)
// is correct across wraparound in unsigned arithmetic, as long as
// the duration is under 2^31 ticks, which MAX_SLEEP_SECONDS ensures.
// Runs in the main loop with interrupts already on (see kernel.c),
// so there is no sti/cli dance and no key-drop guard: keyboard
// input during sleep just queues up as typeahead. v0.5.0: other
// tasks now keep running during this sleep, because the timer
// preempts the main task on every tick inside the hlt loop; in
// v0.4.0 the sleep blocked the scheduler, and that is fixed here
// as a side effect of preemption, not by changing this function.
int timer_sleep(uint32_t seconds) {
    if (seconds > MAX_SLEEP_SECONDS) return -1;
    uint32_t duration = seconds * TICKS_PER_SEC;
    uint32_t start = ticks;
    while ((ticks - start) < duration) {
        __asm__ volatile ("hlt");
    }
    return 0;
}
