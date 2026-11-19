// Host test for the ATA driver (src/drivers/ata.c) against a simulated
// controller: the port reads and writes go to a model of one drive on
// the primary channel, and the clock is the model's, moved on by every
// port access, so the driver's 30 s timeouts take milliseconds here.
// The model can hang (every command stays busy, and a reset does not
// clear it while it is hung), and has one sector that always fails.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fake.h"

#define SECTORS 4096
#define PER_ACCESS_US 50  // what one port access costs, in model time

#define ST_ERR 0x01
#define ST_DRQ 0x08
#define ST_RDY 0x40
#define ST_BSY 0x80

static uint8_t disk[SECTORS][512];
static uint64_t now_us;
static int hung;      // commands do not finish, and a reset does not help
static int stuck;     // a command is in progress that will never end
static uint32_t bad_lba = 0xFFFFFFFF;
static int resets, flushes, commands;
static char last_msg[200];
static int msgs;

static struct {
    uint8_t status, count, lba[3], drive, cmd;
    uint32_t lba_now, left;  // sectors left in the command
    uint32_t word;           // words moved in this sector
} r = { .status = ST_RDY };

static void tick(void) { now_us += PER_ACCESS_US; }

static uint32_t cur_lba(void) {
    return r.lba[0] | (uint32_t)r.lba[1] << 8 | (uint32_t)r.lba[2] << 16 | (uint32_t)(r.drive & 0x0F) << 24;
}

static void start(uint8_t cmd) {
    commands++;
    r.cmd = cmd;
    if (hung) {
        stuck = 1;
        r.status = ST_BSY;
        return;
    }
    r.lba_now = cur_lba();
    r.left = r.count ? r.count : 256;
    r.word = 0;
    if (cmd == 0xEC) {  // IDENTIFY: one block of words
        r.left = 1;
        r.status = ST_RDY | ST_DRQ;
    } else if (cmd == 0xE7) {
        flushes++;
        r.status = ST_RDY;
    } else if (cmd == 0x20 || cmd == 0x30) {
        if (r.lba_now + r.left > SECTORS) {
            r.status = ST_RDY | ST_ERR;
            return;
        }
        r.status = (cmd == 0x20 && r.lba_now == bad_lba) ? (ST_RDY | ST_ERR) : (ST_RDY | ST_DRQ);
    } else {
        r.status = ST_RDY | ST_ERR;
    }
}

static void word_done(void) {
    if (++r.word < 256) return;
    r.word = 0;
    r.lba_now++;
    if (--r.left == 0) {
        r.status = ST_RDY;
        return;
    }
    if (r.cmd == 0x20 && r.lba_now == bad_lba) r.status = ST_RDY | ST_ERR;
}

static uint16_t identify_word(uint32_t i) {
    if (i == 60) return SECTORS & 0xFFFF;
    if (i == 61) return SECTORS >> 16;
    return 0;
}

void outb(uint16_t port, uint8_t v) {
    tick();
    switch (port) {
    case 0x1F2: r.count = v; break;
    case 0x1F3: r.lba[0] = v; break;
    case 0x1F4: r.lba[1] = v; break;
    case 0x1F5: r.lba[2] = v; break;
    case 0x1F6: r.drive = v; break;
    case 0x1F7: start(v); break;
    case 0x3F6:
        if (v & 0x04) {  // SRST
            resets++;
            if (!hung) stuck = 0;
            r.status = stuck ? ST_BSY : ST_RDY;
            r.left = 0;
        }
        break;
    }
}

uint8_t inb(uint16_t port) {
    tick();
    switch (port) {
    case 0x1F4: return 0;  // signature: plain ATA
    case 0x1F5: return 0;
    case 0x1F7: case 0x3F6: return r.status;
    default: return 0;
    }
}

uint16_t inw(uint16_t port) {
    tick();
    if (port != 0x1F0 || !(r.status & ST_DRQ)) return 0xFFFF;
    uint16_t w;
    if (r.cmd == 0xEC) {
        w = identify_word(r.word);
        if (++r.word == 256) r.status = ST_RDY;
        return w;
    }
    const uint8_t* s = disk[r.lba_now];
    w = (uint16_t)(s[r.word * 2] | s[r.word * 2 + 1] << 8);
    word_done();
    return w;
}

void insw(uint16_t port, uint16_t* buf, uint32_t n) {
    while (n--) *buf++ = inw(port);
}

void outw(uint16_t port, uint16_t v) {
    tick();
    if (port != 0x1F0 || r.cmd != 0x30 || !(r.status & ST_DRQ)) return;
    disk[r.lba_now][r.word * 2] = (uint8_t)v;
    disk[r.lba_now][r.word * 2 + 1] = (uint8_t)(v >> 8);
    word_done();
}

uint64_t timer_ticks64(void) { return now_us / 10000; }
void stopwatch_start(stopwatch_t* w) { w->start_us = now_us; }
uint32_t stopwatch_ms(stopwatch_t* w) { return (uint32_t)((now_us - w->start_us) / 1000); }

static int vmsg(const char* fmt, va_list ap) {
    msgs++;
    return vsnprintf(last_msg, sizeof last_msg, fmt, ap);
}
int kprintf(const char* fmt, ...) { va_list ap; va_start(ap, fmt); int n = vmsg(fmt, ap); va_end(ap); return n; }
int kprintf_color(uint8_t c, const char* fmt, ...) { (void)c; va_list ap; va_start(ap, fmt); int n = vmsg(fmt, ap); va_end(ap); return n; }

// ---- the checks ----

static int failures, checks;
static void check(const char* what, int ok) {
    checks++;
    if (!ok) {
        failures++;
        printf("FAIL: %s\n", what);
    }
}

static void fill(uint8_t* b, uint32_t n, uint8_t seed) {
    for (uint32_t i = 0; i < n; i++) b[i] = (uint8_t)(seed + i * 7 + i / 512);
}

// Model time a call took, in ms.
static uint64_t t0;
static void mark(void) { t0 = now_us; }
static uint64_t took_ms(void) { return (now_us - t0) / 1000; }

int main(void) {
    static uint8_t a[300 * 512], b[300 * 512];

    check("the drive is found", ata_init() == 0 && ata_present() && ata_sectors() == SECTORS);

    // Plain work: more than one command's worth (256 sectors), both ways.
    fill(a, sizeof a, 1);
    check("a write of 300 sectors", ata_write(100, 300, a) == 0);
    memset(b, 0, sizeof b);
    check("reads back what was written", ata_read(100, 300, b) == 0 && memcmp(a, b, sizeof a) == 0);
    check("lands where it was sent", memcmp(disk[100], a, 512) == 0 && memcmp(disk[399], a + 299 * 512, 512) == 0);
    check("a flush", ata_flush() == 0 && flushes == 1);
    check("a range past the end is refused", ata_read(SECTORS - 1, 2, b) != 0);
    check("so is no sectors at all", ata_read(0, 0, b) != 0);

    // A bad sector is an error, at once, and the drive stays in use.
    bad_lba = 7;
    mark();
    check("a sector that will not read is an error", ata_read(7, 1, b) != 0);
    check("found out in well under a second", took_ms() < 1000);
    check("and the drive is still used", ata_read(8, 1, b) == 0 && msgs == 0);
    bad_lba = 0xFFFFFFFF;

    // A drive that hangs: every try waits out the timeout, then it is gone.
    hung = 1;
    mark();
    check("a write to a hung drive fails", ata_write(10, 1, a) != 0);
    uint64_t first = took_ms();
    check("after waiting out the timeout on every try", first >= 3 * 30000);
    check("and the console is told", msgs == 1 && strstr(last_msg, "stopped answering"));
    int c = commands;
    mark();
    check("the next command fails too", ata_read(10, 1, b) != 0 && ata_flush() != 0);
    check("at once, without going to the drive", took_ms() == 0 && commands == c);

    // Still hung when the 10 s are up: one short look, then gone again.
    now_us += 10000 * 1000ull;
    mark();
    check("a look after 10 s at a drive still hung fails", ata_read(10, 1, b) != 0);
    check("and gives it only about a second", took_ms() >= 1000 && took_ms() < 2000);
    mark();
    check("then it is left alone for another 10 s", ata_read(10, 1, b) != 0 && took_ms() == 0);
    now_us += 9000 * 1000ull;
    mark();
    check("not looked at again after 9 s", ata_read(10, 1, b) != 0 && took_ms() == 0);

    // It comes back. Nothing goes to it until the 10 s are up; then a
    // reset clears the command it was stuck on and it is used again.
    hung = 0;
    mark();
    check("a drive that has come back is not used before its time", ata_read(10, 1, b) != 0 && took_ms() == 0);
    now_us += 1000 * 1000ull;
    int rs = resets;
    fill(a, 512, 9);
    check("after 10 s a write works again", ata_write(10, 1, a) == 0 && memcmp(disk[10], a, 512) == 0);
    check("after a reset", resets == rs + 1);
    check("and the console is told", msgs == 2 && strstr(last_msg, "answering again"));
    check("reads work too", ata_read(100, 300, b) == 0 && memcmp(disk[100], b, 512) == 0);
    check("and a flush", ata_flush() == 0);

    // A flush that hangs takes the drive away just the same.
    hung = 1;
    check("a flush to a hung drive fails", ata_flush() != 0);
    mark();
    check("and the drive is gone", ata_read(0, 1, b) != 0 && took_ms() == 0);
    hung = 0;
    now_us += 10000 * 1000ull;
    check("and comes back the same way", ata_flush() == 0 && ata_read(0, 1, b) == 0);

    printf("test_ata: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
