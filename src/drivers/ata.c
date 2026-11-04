// v0.7.0: ATA disk driver, polling PIO, primary master only.
// The disk is not memory: the OS talks to it through I/O ports,
// one sector (512 bytes) at a time, by number (LBA, logical block
// address: sector 0 is the first sector on the disk, sector 1 the
// next, and so on, with no cylinders or heads to think about).
// Ports used here: 0x1F0 data (16 bits at a time, 256 words per
// sector), 0x1F2 sector count, 0x1F3-0x1F5 LBA bytes, 0x1F6
// drive/head (0xE0 selects master + LBA mode, plus the top LBA
// bits), 0x1F7 status on read / command on write. The dance for
// a read: wait until the drive is not busy, tell it which drive
// and which sector, tell it how many sectors (1 here, always),
// send command 0x20 (read), wait until it says data is ready,
// read 256 words. A write is the mirror: command 0x30, wait for
// ready, write 256 words, then command 0xE7 (flush cache) so the
// data is really on the disk and not just in the drive's cache
// when the OS later reboots and expects it there. Polling means
// the CPU waits in a loop for the drive, instead of the drive
// interrupting when it is ready. That is slower and simpler, and
// simpler wins for a first disk driver: no IRQ routing, no
// interrupt handler racing the sector loop, and a timeout that
// turns a missing disk into an error instead of a hang. Timeout
// matters: QEMU without a drive attached answers these ports
// with floating status, and an unbounded wait would freeze the
// boot. With a timeout, diskfs_init just reports "no disk" and
// the OS still boots, with file commands unavailable. Only the
// primary master is supported: QEMU's default hard disk, which
// is what the test image attaches as. A second disk, a slave, or
// ATAPI (CD-ROM) would need more drive/head values and more
// commands, and none of them are needed for this piece.

#include <stdint.h>

#define ATA_DATA 0x1F0
#define ATA_COUNT 0x1F2
#define ATA_LBA_LO 0x1F3
#define ATA_LBA_MID 0x1F4
#define ATA_LBA_HI 0x1F5
#define ATA_DRIVE 0x1F6
#define ATA_STATUS 0x1F7
#define ATA_CMD 0x1F7

#define ATA_SR_BSY 0x80
#define ATA_SR_DRQ 0x08
#define ATA_SR_ERR 0x01

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    __asm__ volatile("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

// Wait until BSY clears, or give up. Returns 0 on ready, -1 on
// timeout. The loop bound is generous for QEMU and for real
// spinning disks, and finite, which is the point.
static int ata_wait_not_busy(void) {
    for (int i = 0; i < 1000000; i++) {
        if (!(inb(ATA_STATUS) & ATA_SR_BSY)) return 0;
    }
    return -1;
}

static int ata_wait_drq(void) {
    for (int i = 0; i < 1000000; i++) {
        uint8_t s = inb(ATA_STATUS);
        if (s & ATA_SR_ERR) return -1;
        if (s & ATA_SR_DRQ) return 0;
    }
    return -1;
}

static int ata_select_lba(uint32_t lba) {
    if (ata_wait_not_busy() != 0) return -1;
    outb(ATA_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));
    outb(ATA_COUNT, 1);
    outb(ATA_LBA_LO, lba & 0xFF);
    outb(ATA_LBA_MID, (lba >> 8) & 0xFF);
    outb(ATA_LBA_HI, (lba >> 16) & 0xFF);
    return 0;
}

int ata_read_sector(uint32_t lba, uint8_t* buf) {
    if (ata_select_lba(lba) != 0) return -1;
    outb(ATA_CMD, 0x20);
    if (ata_wait_drq() != 0) return -1;
    for (int i = 0; i < 256; i++) {
        uint16_t w = inw(ATA_DATA);
        buf[i * 2] = w & 0xFF;
        buf[i * 2 + 1] = (w >> 8) & 0xFF;
    }
    return 0;
}

int ata_write_sector(uint32_t lba, const uint8_t* buf) {
    if (ata_select_lba(lba) != 0) return -1;
    outb(ATA_CMD, 0x30);
    if (ata_wait_drq() != 0) return -1;
    for (int i = 0; i < 256; i++) {
        uint16_t w = buf[i * 2] | (buf[i * 2 + 1] << 8);
        outw(ATA_DATA, w);
    }
    outb(ATA_CMD, 0xE7); // flush cache
    if (ata_wait_not_busy() != 0) return -1;
    return 0;
}
