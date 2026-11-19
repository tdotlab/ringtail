// ATA disk, primary channel master, polled PIO with LBA28.
//
// Polling, not interrupts: nIEN is set so the drive never raises
// IRQ 14. Every wait has a timeout in real time, and status is read in
// the order the spec wants: wait for BSY to clear, then look at ERR/DF,
// then DRQ. A command that fails or never finishes is followed by a
// software reset, so the next one does not land on a drive still busy
// with it, and is tried again: reads, writes of whole sectors and
// FLUSH are all safe to repeat. A drive that stays busy through every
// try is taken as gone, like Linux taking a device offline, and every
// later command fails at once: otherwise each of the several commands
// in one file write would wait out its own tries. Every 10 s a command
// that comes along looks at a gone drive again, with a reset and a
// short wait, and one that answers is used again (a host that stalled
// for a minute, say). One that answers with an error (a bad sector) is
// still there and stays in use. Callers (the filesystem) serialise
// access with their own lock, so the controller never sees two
// commands at once.
#include "kernel.h"
#include "arch.h"
#include "drivers.h"
#include "console.h"
#include "ringtail/abi.h"

#define ATA_DATA    0x1F0
#define ATA_ERROR   0x1F1
#define ATA_COUNT   0x1F2
#define ATA_LBA0    0x1F3
#define ATA_LBA1    0x1F4
#define ATA_LBA2    0x1F5
#define ATA_DRIVE   0x1F6
#define ATA_STATUS  0x1F7
#define ATA_CMD     0x1F7
#define ATA_CTRL    0x3F6   // write: device control; read: alternate status

#define ST_ERR 0x01
#define ST_DRQ 0x08
#define ST_DF  0x20
#define ST_BSY 0x80

#define CMD_READ     0x20
#define CMD_WRITE    0x30
#define CMD_FLUSH    0xE7
#define CMD_IDENTIFY 0xEC

// How long the drive may stay busy before it counts as hung. ATA sets
// no bound; Linux gives most commands 30 s, and a disk spinning up from
// standby takes several. This was once 2 million polls, about 0.1 s
// under QEMU, and one slow fsync on the host made a write fail.
#define TIMEOUT_MS 30000
#define TRIES 3
// How often a drive taken as gone is looked at again, and how long it
// then gets to show it is not busy.
#define RECHECK_MS 10000
#define RECHECK_WAIT_MS 1000
#define CTRL_NIEN 0x02
#define CTRL_SRST 0x04

static int present;
static int gone;  // stopped answering: see the top of the file
static uint64_t gone_at;  // the tick it was last found gone
static uint32_t sectors;

// Reading the alternate status four times takes the 400 ns the drive
// needs before its status is meaningful after a command or select.
static void delay400(void) {
    for (int i = 0; i < 4; i++) (void)inb(ATA_CTRL);
}

// Waits until BSY clears, then: -EIO on ERR or DF, 0 if DRQ is set or
// not wanted, else keeps waiting. -ETIMEDOUT when time runs out. The clock
// is read every 256 polls: often enough to see each 10 ms tick with
// interrupts off, rarely enough to keep the loop tight.
static int wait_status(int want_drq) {
    stopwatch_t w;
    stopwatch_start(&w);
    for (uint32_t i = 1;; i++) {
        uint8_t s = inb(ATA_CTRL);
        if (!(s & ST_BSY)) {
            if (s & (ST_ERR | ST_DF)) return -EIO;
            if (!want_drq || (s & ST_DRQ)) return 0;
        }
        if (i % 256 == 0 && stopwatch_ms(&w) >= TIMEOUT_MS) return -ETIMEDOUT;
    }
}

static int wait_ready(void) { return wait_status(0); }
static int wait_drq(void) { return wait_status(1); }

// BSY clear, whatever the other bits say.
static int wait_not_busy(uint32_t ms) {
    stopwatch_t w;
    stopwatch_start(&w);
    for (uint32_t i = 1;; i++) {
        if (!(inb(ATA_CTRL) & ST_BSY)) return 0;
        if (i % 256 == 0 && stopwatch_ms(&w) >= ms) return -EIO;
    }
}

static void pause_ms(uint32_t ms) {
    stopwatch_t w;
    stopwatch_start(&w);
    for (uint32_t i = 1;; i++) {
        (void)inb(ATA_CTRL);
        if (i % 256 == 0 && stopwatch_ms(&w) >= ms) return;
    }
}

// Software reset of the channel. SRST is held for well over the 5 us
// the spec asks, then the drive gets 2 ms (here a tick or two) before
// its status means anything, then up to ms to stop being busy.
static int reset_wait(uint32_t ms) {
    outb(ATA_CTRL, CTRL_SRST | CTRL_NIEN);
    for (int i = 0; i < 64; i++) (void)inb(ATA_CTRL);
    outb(ATA_CTRL, CTRL_NIEN);
    pause_ms(10);
    return wait_not_busy(ms);
}

static void reset(void) { (void)reset_wait(TIMEOUT_MS); }

static void set_gone(void) {
    if (!gone) kprintf_color(COLOR_ERROR, "Disk: stopped answering, trying it again every %d s\n", RECHECK_MS / 1000);
    gone = 1;
    gone_at = timer_ticks64();
}

// Whether a command may go to the drive. A gone one is tried again once
// RECHECK_MS have passed since it was last found gone.
static int usable(void) {
    if (!present) return 0;
    if (!gone) return 1;
    if (timer_ticks64() - gone_at < (uint64_t)RECHECK_MS * TIMER_HZ / 1000) return 0;
    if (reset_wait(RECHECK_WAIT_MS)) {
        gone_at = timer_ticks64();
        return 0;
    }
    gone = 0;
    kprintf("Disk: answering again\n");
    return 1;
}

static void issue(uint8_t cmd, uint32_t lba, uint8_t count) {
    outb(ATA_DRIVE, (uint8_t)(0xE0 | ((lba >> 24) & 0x0F)));
    delay400();
    outb(ATA_COUNT, count);
    outb(ATA_LBA0, lba & 0xFF);
    outb(ATA_LBA1, (lba >> 8) & 0xFF);
    outb(ATA_LBA2, (lba >> 16) & 0xFF);
    outb(ATA_CMD, cmd);
    delay400();
}

int ata_init(void) {
    outb(ATA_CTRL, 0x02);  // nIEN: no interrupts
    outb(ATA_DRIVE, 0xA0);
    delay400();
    outb(ATA_COUNT, 0);
    outb(ATA_LBA0, 0);
    outb(ATA_LBA1, 0);
    outb(ATA_LBA2, 0);
    outb(ATA_CMD, CMD_IDENTIFY);
    delay400();
    uint8_t s = inb(ATA_STATUS);
    if (s == 0 || s == 0xFF) return -ENOENT;     // nothing on the bus
    (void)wait_not_busy(TIMEOUT_MS);
    if (inb(ATA_LBA1) || inb(ATA_LBA2)) return -ENOENT;  // ATAPI or SATA, not ATA
    if (wait_drq()) return -EIO;
    uint16_t id[256];
    for (int i = 0; i < 256; i++) id[i] = inw(ATA_DATA);
    sectors = id[60] | ((uint32_t)id[61] << 16);
    if (sectors == 0) return -EIO;
    present = 1;
    return 0;
}

int ata_present(void) { return present; }
uint32_t ata_sectors(void) { return sectors; }

static int check_range(uint32_t lba, uint32_t count) {
    if (count == 0 || lba >= sectors || count > sectors - lba) return -EIO;
    if (lba + count > 0x10000000) return -EIO;  // LBA28 limit
    return 0;
}

// One command of 1 to 256 sectors, data moved by PIO.
static int pio(uint8_t cmd, uint32_t lba, uint32_t n, uint16_t* p) {
    issue(cmd, lba, (uint8_t)(n & 0xFF));  // 0 means 256
    for (uint32_t s = 0; s < n; s++) {
        int r = wait_drq();
        if (r) return r;
        if (cmd == CMD_READ) {
            insw(ATA_DATA, p, 256);
            p += 256;
        } else {
            for (int i = 0; i < 256; i++) outw(ATA_DATA, *p++);
        }
    }
    // A write is not done when the last word is in: the drive is busy
    // until the sectors are stored (under QEMU, a write to the host file).
    return cmd == CMD_WRITE ? wait_ready() : 0;
}

static int transfer(uint8_t cmd, uint32_t lba, uint32_t count, uint16_t* p) {
    if (!present || check_range(lba, count) || !usable()) return -EIO;
    while (count) {
        uint32_t n = count > 256 ? 256 : count;
        int r = -EIO;
        for (int t = 0; t < TRIES && r; t++) {
            if (t) reset();
            r = pio(cmd, lba, n, p);
        }
        if (r == -ETIMEDOUT) set_gone();
        if (r) return -EIO;
        lba += n;
        count -= n;
        p += n * 256;
    }
    return 0;
}

int ata_read(uint32_t lba, uint32_t count, void* buf) {
    return transfer(CMD_READ, lba, count, buf);
}

int ata_write(uint32_t lba, uint32_t count, const void* buf) {
    // pio only reads through the pointer for a write.
    return transfer(CMD_WRITE, lba, count, (uint16_t*)(uintptr_t)buf);
}

int ata_flush(void) {
    if (!usable()) return -EIO;
    int r = -EIO;
    for (int t = 0; t < TRIES && r; t++) {
        if (t) reset();
        outb(ATA_DRIVE, 0xE0);
        delay400();
        outb(ATA_CMD, CMD_FLUSH);
        delay400();
        r = wait_ready();
    }
    if (r == -ETIMEDOUT) set_gone();
    return r ? -EIO : 0;
}
