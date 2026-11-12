// Calendar dates and Unix time (seconds since 1970-01-01 00:00:00 UTC),
// shared by the kernel and the programs. Proleptic Gregorian, no leap
// seconds, unsigned 32-bit seconds: 1970 to 2106-02-07 06:28:15.
#ifndef RINGTAIL_CIVIL_H
#define RINGTAIL_CIVIL_H

#include <stdint.h>
#include "ringtail/abi.h"  // rtc_time_t

#define CIVIL_MAX_YEAR 2105  // the last whole year 32-bit seconds hold

// 1 if t is a real moment between 1970 and CIVIL_MAX_YEAR: a month of
// 1 to 12, a day that month has (29 February only in a leap year), an
// hour under 24, a minute and a second under 60.
int civil_valid(const rtc_time_t* t);
// Seconds since 1970 for a valid t.
uint32_t civil_to_unix(const rtc_time_t* t);
// The date and time of s seconds since 1970.
void unix_to_civil(uint32_t s, rtc_time_t* t);

#endif
