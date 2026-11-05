// Device drivers.
#ifndef RINGTAIL_DRIVERS_H
#define RINGTAIL_DRIVERS_H

#include <stdint.h>
#include "ringtail/abi.h"  // rtc_time_t

// ---- PIT timer ----
#define TIMER_HZ 100
void timer_init(void);
uint32_t timer_ticks(void);

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
