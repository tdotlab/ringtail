// Device drivers.
#ifndef RINGTAIL_DRIVERS_H
#define RINGTAIL_DRIVERS_H

#include <stdint.h>
#include "ringtail/abi.h"  // rtc_time_t
#include "lib.h"           // sbuf_t

// ---- PIT timer ----
#define TIMER_HZ 100
void timer_init(void);
uint32_t timer_ticks(void);
// Time since boot in microseconds, from the tick count and the PIT's
// own counter. Wraps after about 71 minutes, so only differences mean
// anything.
uint32_t timer_micros(void);

// ---- Real-time clock (CMOS) ----
void rtc_read(rtc_time_t* t);

// ---- Serial port (COM1) ----
void serial_init(void);
void serial_enable_input(void);
int serial_present(void);
void serial_putc(char c);
void serial_write(const char* s, uint32_t len);

// ---- PS/2 keyboard ----
void keyboard_init(void);
// Raw scancode queue, filled only while graphics mode is active,
// for the graphics owner. Returns -1 when empty.
int keyboard_raw_get(void);
int keyboard_raw_pending(void);
void keyboard_raw_flush(void);

// ---- PS/2 mouse ----
int mouse_init(void);
void mouse_get(int* x, int* y, int* buttons);

// ---- ATA disk (primary master, PIO) ----
int ata_init(void);
int ata_present(void);
uint32_t ata_sectors(void);
int ata_read(uint32_t lba, uint32_t count, void* buf);
int ata_write(uint32_t lba, uint32_t count, const void* buf);
int ata_flush(void);

// ---- PCI ----
typedef struct {
    uint8_t bus, dev, fn;
    uint16_t vendor, device;
    uint8_t class_code, subclass, prog_if;
    uint8_t irq;         // interrupt line the firmware routed it to; 0xFF for none
    uint32_t bar[6];     // as read, flag bits included
} pci_dev_t;
void pci_init(void);     // scan every bus, once, at boot
int pci_count(void);
const pci_dev_t* pci_get(int i);
uint32_t pci_read32(const pci_dev_t* d, uint8_t off);
void pci_write32(const pci_dev_t* d, uint8_t off, uint32_t v);
void pci_enable(const pci_dev_t* d);  // memory and I/O decoding, bus mastering
// "Intel ethernet" and the like, in buf.
const char* pci_describe(const pci_dev_t* d, char* buf, uint32_t size);
void pci_format(sbuf_t* sb);          // the device list, for KINFO_PCI

// ---- Intel 8254x gigabit ethernet (e1000) ----
struct netdev;
// Finds and starts the first supported card. Returns it, or 0.
struct netdev* e1000_probe(void);

// ---- VGA graphics (mode 13h) ----
// One task at a time may own the display. Acquire switches to mode
// 13h, release switches back to text mode and restores the console.
int gfx_acquire(int task_id);   // 0, or -EBUSY
int gfx_release(int task_id);   // 0, or -EPERM if not the owner
int gfx_owner(void);            // owner task id, or -1
void gfx_task_exit(int task_id);
void gfx_emergency_text(void);  // for panic: back to text, no questions
void gfx_pixel(int x, int y, uint8_t color);
void gfx_clear(uint8_t color);
void gfx_line(int x0, int y0, int x1, int y1, uint8_t color);
void gfx_rect(int x, int y, int w, int h, uint8_t color, int filled);
void gfx_text(int x, int y, const char* s, uint8_t color);
void gfx_blit(const uint8_t* pixels);
const uint8_t* gfx_font(void);  // 256 glyphs x 16 rows
void gfx_init_font(void);       // read the BIOS font and turn off blink, while still in text mode

#endif
