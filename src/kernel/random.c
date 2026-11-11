// The kernel's random numbers: see random.h.
//
// Two parts. A pool collects unpredictability: every interrupt stirs in
// the CPU's cycle counter as it stood at that moment. When the next
// interrupt comes depends on a hand on a keyboard, a packet's trip
// across a network, a disk; to the cycle, nobody outside the machine
// can know it. Where the CPU has its own generator (RDRAND), that goes
// in too.
//
// The generator turns the pool into bytes. Each request first makes a
// fresh key, SHA-256 of the old key, the pool and the time; then
// ChaCha20 under that key gives a stream, whose first 32 bytes replace
// the key and whose rest is the answer. So an answer says nothing of
// the key that made it (it is gone) nor of the next, and someone who
// learned the state at one moment cannot work back to earlier answers.
//
// How good is it? On a real machine, or under QEMU on a busy host,
// interrupt timing is honestly unpredictable and a few hundred
// interrupts (the timer alone makes 100 a second) are plenty. Straight
// after boot on a CPU without RDRAND there is less to go on: the clock,
// and the jitter of the first ticks. That is the weak moment of every
// such design, and the reason to prefer a CPU with RDRAND (QEMU:
// -cpu max).
#include "kernel.h"
#include "arch.h"
#include "drivers.h"
#include "random.h"
#include "sha256.h"
#include "chacha20.h"
#include "ct.h"

static uint32_t pool[16];
static uint32_t pool_at;
static uint8_t key[32];
static uint32_t requests;
static int have_tsc, have_rdrand;

static inline uint32_t rotl(uint32_t x, int n) { return x << n | x >> (32 - n); }

static inline void rdtsc(uint32_t* lo, uint32_t* hi) { __asm__ volatile("rdtsc" : "=a"(*lo), "=d"(*hi)); }

// One word from the CPU's generator; 0 if it has none to give just now.
static int rdrand(uint32_t* v) {
    uint8_t ok;
    for (int tries = 0; tries < 10; tries++) {
        __asm__ volatile("rdrand %0; setc %1" : "=r"(*v), "=qm"(ok) : : "cc");
        if (ok) return 1;
    }
    return 0;
}

static void stir(uint32_t x) {
    uint32_t* p = &pool[pool_at++ & 15];
    *p = rotl(*p, 7) ^ x;
}

void random_event(uint32_t what) {
    uint32_t lo = 0, hi = 0;
    if (have_tsc) rdtsc(&lo, &hi);
    uint32_t f = irq_save();
    stir(lo ^ what * 0x9E3779B1u);
    irq_restore(f);
}

void random_init(void) {
    uint32_t a, b, c, d;
    cpuid(1, &a, &b, &c, &d);
    have_tsc = (d & (1u << 4)) != 0;
    have_rdrand = (c & (1u << 30)) != 0;
    rtc_time_t t;
    rtc_read(&t);
    sha256(&t, sizeof t, key);
    for (int i = 0; i < 16; i++) {
        uint32_t lo = 0, hi = 0, hw = 0;
        if (have_tsc) rdtsc(&lo, &hi);
        if (have_rdrand) rdrand(&hw);
        stir(lo ^ hi ^ hw);
        io_wait();  // a port write: on real hardware and in an emulator alike, of no fixed length in cycles
    }
}

// Up to 256 bytes, with interrupts off throughout so that two callers
// cannot get the same stream.
static void generate(uint8_t* out, uint32_t len) {
    static const uint8_t nonce[CHACHA20_NONCE_LEN];
    struct {
        uint32_t tsc_lo, tsc_hi, micros, requests, hw[4];
    } now = { 0 };
    uint32_t f = irq_save();
    if (have_tsc) rdtsc(&now.tsc_lo, &now.tsc_hi);
    now.micros = timer_micros();
    now.requests = requests++;
    if (have_rdrand)
        for (int i = 0; i < 4; i++) rdrand(&now.hw[i]);
    sha256_t s;
    sha256_init(&s);
    sha256_update(&s, key, sizeof key);
    sha256_update(&s, pool, sizeof pool);
    sha256_update(&s, &now, sizeof now);
    sha256_final(&s, key);
    uint8_t next[sizeof key];
    chacha20_xor(key, 0, nonce, 0, next, sizeof next);
    chacha20_xor(key, 1, nonce, 0, out, len);
    memcpy(key, next, sizeof key);
    ct_wipe(next, sizeof next);
    ct_wipe(&now, sizeof now);
    irq_restore(f);
}

void random_bytes(void* buf, uint32_t len) {
    uint8_t* p = buf;
    while (len) {
        uint32_t n = len < 256 ? len : 256;
        generate(p, n);
        p += n;
        len -= n;
    }
}
