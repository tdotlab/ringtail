// The CMOS real-time clock, read through ports 0x70/0x71.
//
// The chip keeps the time in BCD or binary, in 24-hour or 12-hour
// form, as register B says. It updates once a second, and a read that
// lands in the middle of an update can be wrong, so a read waits out
// the update and is taken again until two reads agree. There is no
// century register worth trusting: the year is 2000 plus the chip's.
#include "kernel.h"
#include "arch.h"
#include "drivers.h"
#include "fs.h"
#include "ringtail/abi.h"

#define CMOS_INDEX 0x70
#define CMOS_DATA  0x71

#define REG_SEC   0x00
#define REG_MIN   0x02
#define REG_HOUR  0x04
#define REG_DAY   0x07
#define REG_MONTH 0x08
#define REG_YEAR  0x09
#define REG_A     0x0A
#define REG_B     0x0B

static uint8_t cmos(uint8_t reg) {
    outb(CMOS_INDEX, reg);  // bit 7 clear: NMIs stay enabled
    return inb(CMOS_DATA);
}

static void raw_read(uint8_t r[6]) {
    for (int spin = 0; spin < 100000 && (cmos(REG_A) & 0x80); spin++) { }  // update in progress
    r[0] = cmos(REG_SEC);
    r[1] = cmos(REG_MIN);
    r[2] = cmos(REG_HOUR);
    r[3] = cmos(REG_DAY);
    r[4] = cmos(REG_MONTH);
    r[5] = cmos(REG_YEAR);
}

static unsigned bcd(unsigned v) { return (v & 0x0F) + (v >> 4) * 10; }

void rtc_read(rtc_time_t* t) {
    uint8_t a[6], b[6];
    raw_read(a);
    for (int i = 0; i < 5; i++) {  // until two reads agree
        raw_read(b);
        if (memcmp(a, b, sizeof a) == 0) break;
        memcpy(a, b, sizeof a);
    }
    uint8_t mode = cmos(REG_B);
    unsigned pm = 0, v[6];
    for (int i = 0; i < 6; i++) v[i] = a[i];
    if (!(mode & 0x02) && (v[2] & 0x80)) { pm = 1; v[2] &= 0x7F; }  // 12-hour clock
    if (!(mode & 0x04))
        for (int i = 0; i < 6; i++) v[i] = bcd(v[i]);
    if (!(mode & 0x02)) v[2] = (v[2] % 12) + (pm ? 12 : 0);
    t->sec = v[0]; t->min = v[1]; t->hour = v[2];
    t->day = v[3]; t->month = v[4]; t->year = 2000 + v[5];
}

// The filesystem's view of the clock: FAT date and time words.
void fs_clock(uint16_t* date, uint16_t* time) {
    rtc_time_t t;
    rtc_read(&t);
    unsigned y = t.year < 1980 ? 1980 : t.year > 2107 ? 2107 : t.year;
    unsigned mo = t.month >= 1 && t.month <= 12 ? t.month : 1;
    unsigned d = t.day >= 1 && t.day <= 31 ? t.day : 1;
    *date = (uint16_t)((y - 1980) << 9 | mo << 5 | d);
    *time = (uint16_t)((t.hour % 24) << 11 | (t.min % 60) << 5 | (t.sec % 60) / 2);
}
