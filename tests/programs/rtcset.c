// rtcset: what SYS_SETTIME refuses, then (with a time given) one set,
// read back at once.
//
// usage: rtcset                    the refusals, as name=result pairs
//        rtcset Y M D h m s        sets the clock and prints what it reads
#include "ulib.h"

static void try(const char* name, unsigned y, unsigned mo, unsigned d, unsigned h, unsigned mi, unsigned s) {
    rtc_time_t t = { y, mo, d, h, mi, s };
    printf("%s=%d ", name, sys_settime(&t));
}

int main(int argc, char** argv) {
    if (argc == 7) {
        uint32_t v[6];
        for (int i = 0; i < 6; i++)
            if (parse_uint(argv[i + 1], &v[i])) return 2;
        rtc_time_t t = { v[0], v[1], v[2], v[3], v[4], v[5] }, back;
        int r = sys_settime(&t);
        sys_time(&back);
        printf("set=%d read=%04u-%02u-%02u %02u:%02u:%02u\n", r, back.year, back.month, back.day, back.hour, back.min,
               back.sec);
        return r < 0;
    }
    try("feb29in2100", 2100, 2, 29, 0, 0, 0);
    try("apr31", 2090, 4, 31, 0, 0, 0);
    try("month0", 2090, 0, 1, 0, 0, 0);
    try("month13", 2090, 13, 1, 0, 0, 0);
    try("day0", 2090, 1, 0, 0, 0, 0);
    try("hour24", 2090, 1, 1, 24, 0, 0);
    try("min60", 2090, 1, 1, 0, 60, 0);
    try("sec60", 2090, 1, 1, 0, 0, 60);
    try("year1969", 1969, 12, 31, 23, 59, 59);
    try("year2100", 2100, 1, 1, 0, 0, 0);
    try("year2106", 2106, 1, 1, 0, 0, 0);
    try("year2001", 2001, 1, 1, 0, 0, 0);
    try("year2020", 2020, 6, 1, 12, 0, 0);
    printf("null=%d kernel=%d\n", sys_settime(0), sys_settime((const rtc_time_t*)0x100000));
    return 0;
}
