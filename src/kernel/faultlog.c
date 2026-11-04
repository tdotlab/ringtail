// Fault log: the last few fatal events. Graphics mode hides the
// text screen, so a program that dies while drawing would otherwise
// leave no readable trace; `faults` in the shell shows this log.
#include "kernel.h"
#include "arch.h"
#include "console.h"
#include "drivers.h"

#define FAULTLOG_SIZE 8
#define FAULTLOG_LEN  96

static char entries[FAULTLOG_SIZE][FAULTLOG_LEN];
static uint32_t stamps[FAULTLOG_SIZE];
static uint32_t count;  // total ever logged; the newest is count-1

void faultlog_add(const char* fmt, ...) {
    char buf[FAULTLOG_LEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    uint32_t f = irq_save();
    uint32_t slot = count % FAULTLOG_SIZE;
    memcpy(entries[slot], buf, sizeof(buf));
    stamps[slot] = timer_ticks();
    count++;
    irq_restore(f);
}

void faultlog_dump(void) {
    uint32_t n = count < FAULTLOG_SIZE ? count : FAULTLOG_SIZE;
    if (n == 0) {
        kprintf("No faults logged.\n");
        return;
    }
    for (uint32_t i = count - n; i < count; i++) {
        uint32_t slot = i % FAULTLOG_SIZE;
        uint32_t t = stamps[slot];
        kprintf("[%u.%02u] %s\n", t / TIMER_HZ, t % TIMER_HZ, entries[slot]);
    }
}
