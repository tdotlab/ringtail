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

void faultlog_format(sbuf_t* sb) {
    // Copy under cli so a fault logged meanwhile cannot tear an entry.
    char copy[FAULTLOG_SIZE][FAULTLOG_LEN];
    uint32_t f = irq_save();
    uint32_t total = count, n = total < FAULTLOG_SIZE ? total : FAULTLOG_SIZE;
    uint32_t when[FAULTLOG_SIZE];
    for (uint32_t i = 0; i < n; i++) {
        uint32_t slot = (total - n + i) % FAULTLOG_SIZE;
        memcpy(copy[i], entries[slot], FAULTLOG_LEN);
        when[i] = stamps[slot];
    }
    irq_restore(f);
    if (n == 0) sbprintf(sb, "No faults logged.\n");
    for (uint32_t i = 0; i < n; i++)
        sbprintf(sb, "[%u.%02u] %s\n", when[i] / TIMER_HZ, when[i] % TIMER_HZ, copy[i]);
}
