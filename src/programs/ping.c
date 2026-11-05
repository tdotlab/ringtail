// ping [-c COUNT] [-s SIZE] [-W MS] ADDRESS: send ICMP echo requests
// a second apart and report each reply's round trip, then a summary.
// COUNT 0 means until Ctrl+C. There is no DNS yet, so ADDRESS is a
// dotted IPv4 address like 10.0.2.2.
#include "ulib.h"

static int usage(void) {
    dprintf(STDERR_FILENO, "usage: ping [-c COUNT] [-s SIZE] [-W MS] ADDRESS\n"
                           "  COUNT  how many (default 4, 0 for until Ctrl+C)\n"
                           "  SIZE   data bytes per ping (default 56, at most %d)\n"
                           "  MS     how long to wait for each reply (default 1000)\n", PING_MAX_SIZE);
    return 2;
}

static int parse_ip(const char* s, unsigned* out) {
    unsigned ip = 0;
    for (int part = 0; part < 4; part++) {
        if (*s < '0' || *s > '9') return -1;
        unsigned v = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (unsigned)(*s++ - '0');
            if (++digits > 3 || v > 255) return -1;
        }
        ip = ip << 8 | v;
        if (part < 3 && *s++ != '.') return -1;
    }
    if (*s) return -1;
    *out = ip;
    return 0;
}

static void print_ms(unsigned us) { printf("%u.%03u", us / 1000, us % 1000); }

int main(int argc, char** argv) {
    uint32_t count = 4, size = 56, wait_ms = 1000;
    const char* host = 0;
    for (int i = 1; i < argc; i++) {
        uint32_t* opt = 0;
        if (!strcmp(argv[i], "-c")) opt = &count;
        else if (!strcmp(argv[i], "-s")) opt = &size;
        else if (!strcmp(argv[i], "-W")) opt = &wait_ms;
        else if (argv[i][0] == '-' || host) return usage();
        else { host = argv[i]; continue; }
        if (++i == argc || parse_uint(argv[i], opt)) return usage();
    }
    if (!host || size > PING_MAX_SIZE || wait_ms < 1 || wait_ms > 10000) return usage();
    unsigned ip;
    if (parse_ip(host, &ip)) {
        dprintf(STDERR_FILENO, "ping: %s is not an address like 10.0.2.2 (there are no names yet)\n", host);
        return 2;
    }

    printf("PING %s: %u data bytes\n", host, size);
    unsigned sent = 0, received = 0, min = 0xFFFFFFFFu, max = 0, total = 0;
    for (uint32_t seq = 1; count == 0 || seq <= count; seq++) {
        ping_t p = { ip, (int)seq, (int)size, (int)wait_ms, 0, 0 };
        int r = sys_ping(&p);
        if (r == -EINTR) break;
        if (r == -ENETDOWN || r == -EINVAL) {
            dprintf(STDERR_FILENO, "ping: %s\n", strerror(r));
            return 2;
        }
        sent++;
        if (r == 0) {
            unsigned us = (unsigned)p.rtt_us;
            received++;
            total += us;
            if (us < min) min = us;
            if (us > max) max = us;
            printf("%u bytes from %s: seq=%u ttl=%d time=", size + 8, host, seq, p.ttl);
            print_ms(us);
            printf(" ms\n");
        } else {
            printf("seq=%u: %s\n", seq, r == -EIO ? "the reply came back damaged" : strerror(r));
        }
        // A second between requests; a timeout has waited already.
        if ((count == 0 || seq < count) && r != -ETIMEDOUT) sys_sleep(1);
    }
    printf("--- %s ---\n%u sent, %u received, %u%% lost", host, sent, received,
           sent ? (sent - received) * 100 / sent : 0);
    if (received) {
        printf(", round trip min/avg/max ");
        print_ms(min); printf("/"); print_ms(total / received); printf("/"); print_ms(max);
        printf(" ms");
    }
    printf("\n");
    return received ? 0 : 1;
}
