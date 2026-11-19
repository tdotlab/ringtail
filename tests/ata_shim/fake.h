// What ata.c takes from the kernel, for test_ata: port I/O goes to a
// simulated controller and the clock is the simulation's.
#ifndef ATA_FAKE_H
#define ATA_FAKE_H
#include <stdint.h>
#include <stddef.h>
#include "ringtail/abi.h"

void outb(uint16_t port, uint8_t v);
void outw(uint16_t port, uint16_t v);
uint8_t inb(uint16_t port);
uint16_t inw(uint16_t port);
void insw(uint16_t port, uint16_t* buf, uint32_t n);

#define TIMER_HZ 100
uint64_t timer_ticks64(void);
typedef struct { uint64_t start_us; } stopwatch_t;
void stopwatch_start(stopwatch_t* w);
uint32_t stopwatch_ms(stopwatch_t* w);

#define COLOR_ERROR 0x0C
int kprintf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
int kprintf_color(uint8_t color, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

int ata_init(void);
int ata_present(void);
uint32_t ata_sectors(void);
int ata_read(uint32_t lba, uint32_t count, void* buf);
int ata_write(uint32_t lba, uint32_t count, const void* buf);
int ata_flush(void);
#endif
