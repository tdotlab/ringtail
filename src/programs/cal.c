// cal [-1 | -3 | -y] [[MONTH] YEAR]    a calendar
//
// With no arguments, this month. One argument is a YEAR and shows the
// whole year; two are MONTH YEAR and show that month. MONTH is 1 to 12
// or a name: three letters or more, of which the first three have to
// match (mar, march and marc are all March). YEAR is 1 to 9999.
//   -1  one month (the default for a month)
//   -3  three months, the one asked for in the middle (this one, with no
//       arguments); needs a month, a year alone is refused
//   -y  the whole year (this one, with no arguments); the month, if given,
//       is ignored
// If more than one of these is given, the last counts.
// The layout is the one BSD cal and ncal print, to the space: weeks start
// on Sunday, and every line has its full width, trailing blanks and all.
//
// The calendar is the one Britain and its colonies used: Julian up to
// 2 September 1752, which was followed by 14 September (the days between
// are missing), Gregorian after that. This is what cal on the BSDs and
// Linux shows, and so what people expect to see; other countries changed
// at other times, from 1582 to 1923, and cal does not know about them.
// The days come from a day count of its own rather than civil.c, which is
// Gregorian only and stops at 2105.
//
// On a terminal, today is shown in reverse; piped or redirected, it is
// plain.
#include "ulib.h"

static int usage(void) {
    dprintf(STDERR_FILENO, "usage: cal [-1 | -3 | -y] [[MONTH] YEAR]\n");
    return 2;
}

static const char* const names[12] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December",
};

static uint32_t today_y, today_m, today_d;  // 0 unless on a terminal and the clock works

static int leap(uint32_t y) {
    if (y < 1752) return y % 4 == 0;  // Julian
    return y % 4 == 0 && (y % 100 != 0 || y % 400 == 0);
}

static uint32_t days_in(uint32_t y, uint32_t m) {
    static const uint8_t d[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return d[m - 1] + (m == 2 && leap(y));
}

// The day of the week of the 1st of the month, 0 for Sunday: from the
// Julian day number (day 0 was a Monday). The 1st of any month up to
// September 1752 is a Julian date; from October 1752 on it is Gregorian.
static uint32_t first_weekday(uint32_t y, uint32_t m) {
    uint32_t a = (14 - m) / 12;
    uint32_t y2 = y + 4800 - a, m2 = m + 12 * a - 3;
    uint32_t jdn = 1 + (153 * m2 + 2) / 5 + 365 * y2 + y2 / 4;
    if (y < 1752 || (y == 1752 && m <= 9)) jdn -= 32083;
    else jdn = jdn + y2 / 400 - y2 / 100 - 32045;
    return (jdn + 1) % 7;
}

// The days of the month in a grid of six weeks by seven: 0 for blank.
static void fill(uint8_t* cells, uint32_t y, uint32_t m) {
    memset(cells, 0, 42);
    uint32_t pos = first_weekday(y, m), n = days_in(y, m);
    for (uint32_t d = 1; d <= n; d++) {
        if (y == 1752 && m == 9 && d > 2 && d < 14) continue;  // the missing days
        cells[pos++] = (uint8_t)d;
    }
}

// A title centred in the 20 columns of a month (one column to the left
// when it cannot be exact), padded to the 22 a month takes with its gap.
static void title(const char* s) {
    int len = (int)strlen(s), pad = (20 - len) / 2;
    printf("%*s%s%*s", pad, "", s, 22 - pad - len, "");
}

// Up to three months side by side: titles, weekday names, six weeks.
// with_year puts the year in each title.
static void row_of_months(const uint32_t* ys, const uint32_t* ms, int n, int with_year) {
    uint8_t cells[3][42];
    uint32_t hl[3];
    for (int i = 0; i < n; i++) {
        fill(cells[i], ys[i], ms[i]);
        hl[i] = (ys[i] == today_y && ms[i] == today_m) ? today_d : 0;
    }
    for (int i = 0; i < n; i++) {
        if (with_year) {
            char t[32];
            snprintf(t, sizeof t, "%s %u", names[ms[i] - 1], ys[i]);
            title(t);
        } else {
            title(names[ms[i] - 1]);
        }
    }
    printf("\n");
    for (int i = 0; i < n; i++) printf("Su Mo Tu We Th Fr Sa  ");
    printf("\n");
    for (int w = 0; w < 6; w++) {
        for (int i = 0; i < n; i++) {
            for (int k = 0; k < 7; k++) {
                uint32_t d = cells[i][w * 7 + k];
                if (k) printf(" ");
                if (!d) printf("  ");
                else if (d == hl[i]) printf("\x1b[7m%2u\x1b[27m", d);
                else printf("%2u", d);
            }
            printf("  ");
        }
        printf("\n");
    }
}

// The year over the middle of the three columns a year takes.
static void year_heading(uint32_t y) {
    char t[16];
    snprintf(t, sizeof t, "%u", y);
    printf("%*s%s\n", (int)(60 - strlen(t)) / 2, "", t);
}

static void show_year(uint32_t y) {
    year_heading(y);
    for (int r = 0; r < 4; r++) {
        uint32_t ys[3] = { y, y, y }, ms[3] = { 3 * r + 1, 3 * r + 2, 3 * r + 3 };
        if (r) printf("\n");
        row_of_months(ys, ms, 3, 0);
    }
}

static void show_three(uint32_t y, uint32_t m) {
    uint32_t ys[3] = { y, y, y }, ms[3] = { m - 1, m, m + 1 };
    if (m == 1) { ys[0] = y - 1; ms[0] = 12; }
    if (m == 12) { ys[2] = y + 1; ms[2] = 1; }
    // When the three are a row of the year (Jan-Mar, Apr-Jun, ...), as BSD
    // does, the year goes above and the titles drop theirs.
    if (m % 3 == 2) {
        year_heading(y);
        row_of_months(ys, ms, 3, 0);
    } else {
        row_of_months(ys, ms, 3, 1);
    }
}

// White space, an optional sign, digits, and nothing after: what BSD cal
// takes for a number (a minus can only get here after "--"). A negative number
// comes out as 0, which is out of range for a month and a year alike. The
// empty string is 0 too, but white space alone is not a number. Large values
// stop growing at 100000, which is out of range for anything, rather than
// wrapping around into range.
static int parse_num(const char* s, uint32_t* out) {
    if (!*s) {
        *out = 0;
        return 1;
    }
    while (*s == ' ' || (*s >= '\t' && *s <= '\r')) s++;
    int neg = *s == '-';
    if (neg || *s == '+') s++;
    uint32_t v = 0;
    int digits = 0;
    for (; *s >= '0' && *s <= '9'; s++, digits++) {
        v = v * 10 + (uint32_t)(*s - '0');
        if (v > 100000) v = 100000;
    }
    if (!digits || *s) return 0;
    *out = neg ? 0 : v;
    return 1;
}

static uint32_t lower(char c) { return (uint32_t)(c >= 'A' && c <= 'Z' ? c + 32 : c); }

static int parse_month(const char* s, uint32_t* out) {
    uint32_t v;
    if (parse_num(s, &v) && v >= 1 && v <= 12) {
        *out = v;
        return 1;
    }
    // Three letters have to match, so a shorter word fails by itself: its
    // end is a NUL, which no month has.
    for (uint32_t m = 0; m < 12; m++) {
        if (lower(s[0]) == lower(names[m][0]) && lower(s[1]) == lower(names[m][1]) &&
            lower(s[2]) == lower(names[m][2])) {
            *out = m + 1;
            return 1;
        }
    }
    return 0;
}

int main(int argc, char** argv) {
    // Of -1, -3 and -y the last one given counts (as in BSD cal), so the
    // order has to be read before getflags takes the options out. After a
    // "--" they would be operands, and an error, so there is no need to stop.
    int last = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-') continue;
        for (const char* p = argv[i] + 1; *p; p++)
            if (*p == '1' || *p == '3' || *p == 'y') last = *p;
    }
    int one = last == '1', three = last == '3', whole = last == 'y';
    char got[32];
    if (getflags(&argc, argv, "13y", got)) return usage();
    if (argc > 3) return usage();

    uint32_t year = 0, month = 0;
    if (argc >= 2) {
        const char* ys = argv[argc - 1];
        if (!parse_num(ys, &year)) {
            dprintf(STDERR_FILENO, "cal: not a valid year %s\n", ys);
            return 2;
        }
        if (year < 1 || year > 9999) {
            dprintf(STDERR_FILENO, "cal: year `%s' not in range 1..9999\n", ys);
            return 2;
        }
        if (argc == 3 && !parse_month(argv[1], &month)) {
            dprintf(STDERR_FILENO, "cal: %s is neither a month number (1..12) nor a name\n", argv[1]);
            return 2;
        }
    }
    if (argc == 2 && (one || three)) {
        dprintf(STDERR_FILENO, "cal: -%c together with a given year but no given month is not supported.\n",
                three ? '3' : '1');
        return 2;
    }

    rtc_time_t now;
    int clock = sys_time(&now) >= 0 && now.year >= 1 && now.year <= 9999;
    if (clock && sys_fdtype(STDOUT_FILENO) == FDTYPE_TTY) {
        today_y = now.year;
        today_m = now.month;
        today_d = now.day;
    }
    if (argc == 1) {  // nothing given: this month, or this year with -y
        if (!clock) {
            dprintf(STDERR_FILENO, "cal: cannot read the clock\n");
            return 1;
        }
        year = now.year;
        month = now.month;
    }

    if (whole || argc == 2) {
        show_year(year);
    } else if (three) {
        show_three(year, month);
    } else {
        uint32_t ys[1] = { year }, ms[1] = { month };
        row_of_months(ys, ms, 1, 1);
    }
    return 0;
}
