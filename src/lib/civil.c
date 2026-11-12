// Dates to Unix time and back: see civil.h.
//
// The day count is Howard Hinnant's days_from_civil, with the year
// starting in March so that the leap day falls at its end. Everything
// stays unsigned and 32-bit: 1970 to 2105 needs no more, and the kernel
// has no 64-bit division to spare.
#include "civil.h"

static int leap(uint32_t y) { return y % 4 == 0 && (y % 100 != 0 || y % 400 == 0); }

int civil_valid(const rtc_time_t* t) {
    static const uint8_t mdays[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (t->year < 1970 || t->year > CIVIL_MAX_YEAR || t->month < 1 || t->month > 12 || t->day < 1) return 0;
    uint32_t most = mdays[t->month - 1] + (t->month == 2 && leap(t->year));
    return t->day <= most && t->hour < 24 && t->min < 60 && t->sec < 60;
}

// Days from 0000-03-01 to the given date.
static uint32_t days(uint32_t y, uint32_t m, uint32_t d) {
    if (m <= 2) y--;
    uint32_t era = y / 400, yoe = y % 400;
    uint32_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    return era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy;
}

#define DAYS_1970 719468u  // days(1970, 1, 1)

uint32_t civil_to_unix(const rtc_time_t* t) {
    return (days(t->year, t->month, t->day) - DAYS_1970) * 86400u + t->hour * 3600u + t->min * 60u + t->sec;
}

void unix_to_civil(uint32_t s, rtc_time_t* t) {
    uint32_t z = s / 86400 + DAYS_1970, secs = s % 86400;
    uint32_t era = z / 146097, doe = z % 146097;
    uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    uint32_t mp = (5 * doy + 2) / 153;
    t->day = doy - (153 * mp + 2) / 5 + 1;
    t->month = mp < 10 ? mp + 3 : mp - 9;
    t->year = era * 400 + yoe + (t->month <= 2);
    t->hour = secs / 3600;
    t->min = secs / 60 % 60;
    t->sec = secs % 60;
}
