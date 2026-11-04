// ATA disk, primary channel master, polled PIO with LBA28.
//
// Polling, not interrupts: nIEN is set so the drive never raises
// IRQ 14. Every wait has a timeout, and status is read in the order
// the spec wants: wait for BSY to clear, then look at ERR/DF, then
// DRQ. Callers (the filesystem) serialise access with their own
// lock, so the controller never sees two commands at once.
#include "kernel.h"
#include "arch.h"
#include "drivers.h"
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

#define TIMEOUT 2000000

static int present;
static uint32_t sectors;

// Reading the alternate status four times takes the 400 ns the drive
// needs before its status is meaningful after a command or select.
static void delay400(void) {
    for (int i = 0; i < 4; i++) (void)inb(ATA_CTRL);
}

static int wait_ready(void) {
    for (int i = 0; i < TIMEOUT; i++) {
        uint8_t s = inb(ATA_CTRL);
        if (s & ST_BSY) continue;
        if (s & (ST_ERR | ST_DF)) return -EIO;
        return 0;
    }
    return -EIO;
}

static int wait_drq(void) {
    for (int i = 0; i < TIMEOUT; i++) {
        uint8_t s = inb(ATA_CTRL);
        if (s & ST_BSY) continue;
        if (s & (ST_ERR | ST_DF)) return -EIO;
        if (s & ST_DRQ) return 0;
    }
    return -EIO;
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
    for (int i = 0; i < TIMEOUT && (inb(ATA_CTRL) & ST_BSY); i++) { }
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
    if (!present) return -EIO;
    if (count == 0 || lba >= sectors || count > sectors - lba) return -EIO;
    if (lba + count > 0x10000000) return -EIO;  // LBA28 limit
    return 0;
}

int ata_read(uint32_t lba, uint32_t count, void* buf) {
    if (check_range(lba, count)) return -EIO;
    uint16_t* p = buf;
    while (count) {
        uint32_t n = count > 256 ? 256 : count;
        issue(CMD_READ, lba, (uint8_t)(n & 0xFF));  // 0 means 256
        for (uint32_t s = 0; s < n; s++) {
            if (wait_drq()) return -EIO;
            uint32_t words = 256;
            __asm__ volatile("rep insw" : "+D"(p), "+c"(words) : "d"(ATA_DATA) : "memory");
        }
        lba += n;
        count -= n;
    }
    return 0;
}

int ata_write(uint32_t lba, uint32_t count, const void* buf) {
    if (check_range(lba, count)) return -EIO;
    const uint16_t* p = buf;
    while (count) {
        uint32_t n = count > 256 ? 256 : count;
        issue(CMD_WRITE, lba, (uint8_t)(n & 0xFF));
        for (uint32_t s = 0; s < n; s++) {
            if (wait_drq()) return -EIO;
            for (int i = 0; i < 256; i++) outw(ATA_DATA, *p++);
        }
        if (wait_ready()) return -EIO;
        lba += n;
        count -= n;
    }
    return 0;
}

int ata_flush(void) {
    if (!present) return -EIO;
    outb(ATA_DRIVE, 0xE0);
    delay400();
    outb(ATA_CMD, CMD_FLUSH);
    delay400();
    return wait_ready();
}
