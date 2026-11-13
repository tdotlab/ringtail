// sntp [-q] [-b] [-p PORT] [SERVER]: asks a time server what time it
// is and sets the clock to the answer. SERVER is a name or an address;
// without one it asks the time servers DHCP named, or pool.ntp.org if
// it named none. A name with several addresses is tried an address at a
// time, up to four. -q only says how far off the clock is. PORT is for a
// server not on 123, like a test one.
//
// -b is the run init makes at boot: one try of a second each at two
// addresses at most, and a single line, starting "Clock:", whatever
// happens.
//
// The CMOS clock counts whole seconds, so ours is made from it and the
// timer: wait for the CMOS second to turn over, then count ticks from
// there. That is good to a tick (10 ms), and so is the clock it sets:
// the chip is set at the half second, when its next second starts half
// a second on.
//
// An answer counts only if it comes from the address and port asked
// and echoes the request's transmit timestamp, which is 64 random bits
// (a fresh one for each try, so a late answer to an earlier try cannot
// stretch the round trip). Nothing here can tell a true answer from a
// lie told by someone in the middle: NTP without NTS is not
// authenticated. The kernel's floor is what stops a liar from setting
// the clock back before this system was built.
#include "ulib.h"
#include "civil.h"
#include "dns.h"
#include "ntpmsg.h"

#define DEFAULT_SERVER "pool.ntp.org"
#define HZ 100
#define TRIES 2
#define WAIT_TICKS 200  // 2 s for each try
#define MAX_ADDRS 4
#define BOOT_TRIES 1
#define BOOT_WAIT_TICKS 100
#define BOOT_ADDRS 2

static int usage(void) {
    dprintf(STDERR_FILENO, "usage: sntp [-q] [-b] [-p PORT] [SERVER]\n"
                           "  -q      only say how far off the clock is; do not set it\n"
                           "  -b      as at boot: quick, and one line whatever happens\n"
                           "  SERVER  a time server's name or address (default: the ones DHCP\n"
                           "          named, or " DEFAULT_SERVER ")\n");
    return 2;
}

// Our clock: the CMOS second that began at tick base_ticks.
static uint32_t base_unix, base_ticks;

static void align(void) {
    rtc_time_t a, b;
    sys_time(&a);
    uint32_t until = sys_ticks() + 2 * HZ;  // a stopped chip should not hang us
    do sys_time(&b);
    while (b.sec == a.sec && (int32_t)(until - sys_ticks()) > 0);
    base_ticks = sys_ticks();
    // A clock that reads nonsense counts as 1970, which still puts the
    // server's answer in the right era until 2038.
    base_unix = civil_valid(&b) ? civil_to_unix(&b) : 0;
}

static uint64_t now_local(void) {
    uint32_t dt = sys_ticks() - base_ticks;
    return ((uint64_t)base_unix + dt / HZ) << 32 | (uint32_t)(((uint64_t)(dt % HZ) << 32) / HZ);
}

// Waits out the given number of ticks on fd, throwing away whatever
// arrives meanwhile.
static void wait_ticks(int fd, uint32_t n) {
    uint8_t junk[NTP_SIZE];
    uint32_t until = sys_ticks() + n;
    for (int32_t left; (left = (int32_t)(until - sys_ticks())) > 0;) {
        udp_msg_t in = { 0, 0, junk, sizeof junk, left * 10 };
        int r = sys_recvfrom(fd, &in);
        if (r < 0 && r != -ETIMEDOUT) break;
    }
}

// One server: up to tries requests, each given wait ticks to be
// answered. 0 with r, t1 and t4 filled in, or why not.
static int query(int fd, uint32_t ip, int port, int tries, uint32_t wait, ntp_reply_t* r, uint64_t* t1, uint64_t* t4) {
    uint8_t q[NTP_SIZE], buf[UDP_MAX_SIZE];
    for (int t = 0; t < tries; t++) {
        uint64_t nonce = 0;
        while (!nonce) sys_random(&nonce, sizeof nonce);
        ntp_build_request(q, nonce);
        udp_msg_t m = { ip, port, q, sizeof q, 0 };
        *t1 = now_local();
        int e = sys_sendto(fd, &m);
        if (e < 0) return e;
        uint32_t until = sys_ticks() + wait;
        for (int32_t left; (left = (int32_t)(until - sys_ticks())) > 0;) {
            udp_msg_t in = { 0, 0, buf, sizeof buf, left * 10 };
            int n = sys_recvfrom(fd, &in);
            *t4 = now_local();
            if (n == -ETIMEDOUT) break;
            if (n < 0) return n;
            if (in.ip != ip || in.port != port) continue;
            e = ntp_parse(buf, n, nonce, r);
            if (e != NTP_E_NOTOURS) return e;
        }
    }
    return NTP_E_TIMEOUT;
}

static const char* why(int e, const ntp_reply_t* r) {
    static char msg[64];
    switch (e) {
    case NTP_E_TIMEOUT: return "no answer";
    case NTP_E_UNSYNC: return "the server does not know the time itself";
    case NTP_E_BADREPLY: return "the answer made no sense";
    case NTP_E_FAR: return "the server's time is more than 68 years from ours (or outside 1970 to 2106)";
    case NTP_E_KISS: {
        // Four bytes the server chose: printable ones as they are, the
        // rest as \DDD, so a terminal never sees them raw.
        int n = snprintf(msg, sizeof msg, "the server sent a kiss-o'-death (");
        for (int i = 0; i < 4; i++) {
            uint8_t c = (uint8_t)r->kiss[i];
            n += snprintf(msg + n, sizeof msg - (size_t)n, c > 32 && c < 127 && c != '\\' ? "%c" : "\\%03u", c);
        }
        snprintf(msg + n, sizeof msg - (size_t)n, ")");
        return msg;
    }
    default: return strerror(e);
    }
}

// v in fixed point as seconds to the millisecond, with a sign if asked.
static const char* secs(char* buf, size_t size, int64_t v, int sign) {
    uint64_t a = v < 0 ? -(uint64_t)v : (uint64_t)v;
    uint32_t ms = (uint32_t)(((a & 0xFFFFFFFFu) * 1000) >> 32);
    snprintf(buf, size, "%s%u.%03u", sign ? (v < 0 ? "-" : "+") : "", (uint32_t)(a >> 32), ms);
    return buf;
}

static const char* fmt_time(char* buf, size_t size, const rtc_time_t* t) {
    snprintf(buf, size, "%04u-%02u-%02u %02u:%02u:%02u", t->year, t->month, t->day, t->hour, t->min, t->sec);
    return buf;
}

// With -b everything is said in one line on stdout, starting "Clock:";
// otherwise trouble goes to stderr.
static int boot;

// The clock was not set: who said what, or what went wrong.
static int failed(const char* who, const char* what) {
    if (boot) {
        rtc_time_t t;
        char b[24];
        sys_time(&t);
        printf("Clock: %s, as the CMOS has it (%s: %s)\n", fmt_time(b, sizeof b, &t), who, what);
    } else dprintf(STDERR_FILENO, "sntp: %s: %s\n", who, what);
    return 1;
}

int main(int argc, char** argv) {
    const char* server = 0;
    uint32_t port = NTP_PORT;
    int query_only = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-q")) query_only = 1;
        else if (!strcmp(argv[i], "-b")) boot = 1;
        else if (!strcmp(argv[i], "-p")) {
            if (++i == argc || parse_uint(argv[i], &port) || port < 1 || port > 65535) return usage();
        } else if (argv[i][0] == '-') return usage();
        else if (!server) server = argv[i];
        else return usage();
    }

    // Whom to ask: the server named, else the ones DHCP named (as
    // addresses, so there is no name to look up), else the pool.
    uint32_t addrs[MAX_ADDRS];
    int naddr = 0;
    if (!server) {
        netconf_t nc;
        if (sys_netconf(NETCONF_GET, &nc) == 0)
            for (int i = 0; i < 2; i++)
                if (nc.ntp[i]) addrs[naddr++] = nc.ntp[i];
        if (!naddr) server = DEFAULT_SERVER;
    }
    if (server) {
        dns_result_t dr;
        int n = dns_lookup(server, 0, 0, &dr);
        if (n < 0) return failed(server, dns_strerror(n));
        for (int i = 0; i < n && naddr < MAX_ADDRS; i++) addrs[naddr++] = dr.a.addrs[i];
    }
    if (boot && naddr > BOOT_ADDRS) naddr = BOOT_ADDRS;
    int fd = sys_socket(SOCK_DGRAM), bound = fd < 0 ? fd : sys_bind(fd, 0);
    if (bound < 0) {
        if (fd >= 0) sys_close(fd);
        return failed("socket", strerror(bound));
    }
    align();

    ntp_reply_t r;
    uint64_t t1 = 0, t2 = 0, t3 = 0, t4 = 0;
    uint32_t ip = 0;
    int e = NTP_E_TIMEOUT;
    char who[96];  // the server asked last: its name and address, or the address
    for (int i = 0; i < naddr; i++) {
        ip = addrs[i];
        char addr[16];
        snprintf(addr, sizeof addr, IPV4_FMT, IPV4_ARGS(ip));
        if (server && strcmp(addr, server)) snprintf(who, sizeof who, "%s (%s)", server, addr);
        else snprintf(who, sizeof who, "%s", addr);
        e = query(fd, ip, (int)port, boot ? BOOT_TRIES : TRIES, boot ? BOOT_WAIT_TICKS : WAIT_TICKS, &r, &t1, &t4);
        if (e == 0) {
            // The server's times, in the era nearest ours.
            t2 = ntp_to_unix(r.recv, base_unix);
            t3 = ntp_to_unix(r.xmit, base_unix);
            if (!t2 || !t3) e = NTP_E_FAR;
        }
        if (e == 0) break;
        if (!boot) failed(who, why(e, &r));
        if (e == -ENETDOWN || e == -ENETUNREACH || e == -EHOSTUNREACH) break;  // nor will the next address do better
    }
    if (e) {
        sys_close(fd);
        return boot ? failed(who, why(e, &r)) : 1;
    }

    int64_t offset, delay;
    char a[24], b[24];
    ntp_offset(t1, t2, t3, t4, &offset, &delay);
    if (!boot)
        printf(IPV4_FMT ": stratum %d, offset %s s, delay %s s\n", IPV4_ARGS(ip), r.stratum,
               secs(a, sizeof a, offset, 1), secs(b, sizeof b, delay, 0));
    int close_enough = offset < NTP_ONE / 2 && offset > -NTP_ONE / 2;
    if (query_only || close_enough) {
        sys_close(fd);
        rtc_time_t t;
        sys_time(&t);
        if (boot && close_enough) printf("Clock: %s, right to within half a second by %s\n", fmt_time(a, sizeof a, &t), who);
        else if (boot) printf("Clock: %s, %s s %s by %s; left as it is\n", fmt_time(a, sizeof a, &t),
                              secs(b, sizeof b, offset, 0), offset > 0 ? "slow" : "fast", who);
        else if (!query_only) printf("the clock is right to within half a second; left as it is\n");
        return 0;
    }

    // The clock takes whole seconds and starts its next one half a
    // second after it is set (see SYS_SETTIME), so set it to t at t + 0.5.
    uint32_t frac = (uint32_t)(now_local() + (uint64_t)offset);
    wait_ticks(fd, (uint32_t)(((uint64_t)(uint32_t)(0x80000000u - frac) * HZ + 0xFFFFFFFFu) >> 32));
    sys_close(fd);
    uint64_t when = now_local() + (uint64_t)offset;
    rtc_time_t was, now;
    sys_time(&was);
    unix_to_civil((uint32_t)(when >> 32), &now);
    e = now.year > 2099 ? -ERANGE : sys_settime(&now);
    if (e == -ERANGE) {
        char what[96];
        snprintf(what, sizeof what, "says it is %s, %s", fmt_time(a, sizeof a, &now),
                 now.year > 2099 ? "after 2099, which the clock cannot hold" : "before this system was built");
        if (boot) return failed(who, what);
        dprintf(STDERR_FILENO, "sntp: the server %s; the clock is left alone\n", what);
        return 1;
    }
    if (e < 0) return failed("setting the clock", strerror(e));
    if (boot) printf("Clock: set to %s by %s (it was %s s %s)\n", fmt_time(b, sizeof b, &now), who,
                     secs(a, sizeof a, offset, 0), offset > 0 ? "slow" : "fast");
    else printf("the clock was %s and is now %s\n", fmt_time(a, sizeof a, &was), fmt_time(b, sizeof b, &now));
    return 0;
}
