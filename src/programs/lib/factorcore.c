#include "factorcore.h"

int fc_parse(const char* s, uint64_t* out) {
    while (*s == ' ') s++;
    if (*s == '+') s++;
    if (!*s) return FC_INVALID;
    uint64_t v = 0;
    int big = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return FC_INVALID;
        unsigned d = (unsigned)(*s - '0');
        if (v > (UINT64_MAX - d) / 10) big = 1;
        else v = v * 10 + d;
    }
    if (big) return FC_TOO_BIG;
    *out = v;
    return FC_OK;
}

void fc_u64_text(uint64_t v, char* out) {
    char d[24];
    int nd = 0;
    do {
        d[nd++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (nd) *out++ = d[--nd];
    *out = 0;
}

// ---- arithmetic modulo an odd n ------------------------------------------------
//
// A product of two numbers below n needs up to 128 bits, and reducing it
// mod n would be a 128-by-64 division. (This was written for i386, which
// had no 128-bit type at all; TODO.md has the switch to unsigned
// __int128.) Montgomery's trick avoids the division: numbers are kept as
// x*2^64 mod n ("in Montgomery form"), and a product is the 128-bit result
// (built from four 32-bit multiplications) shifted down by 64 bits with one
// more multiplication.

typedef struct {
    uint64_t n;
    uint64_t ninv;  // n * ninv == 1 mod 2^64
    uint64_t one;   // 2^64 mod n: the number 1, in Montgomery form
} mod_t;

static uint64_t addmod(uint64_t a, uint64_t b, uint64_t n) { return a >= n - b ? a - (n - b) : a + b; }

static uint64_t mulhi(uint64_t a, uint64_t b) {
    uint64_t a0 = (uint32_t)a, a1 = a >> 32, b0 = (uint32_t)b, b1 = b >> 32;
    uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    uint64_t mid = (p00 >> 32) + (uint32_t)p01 + (uint32_t)p10;
    return p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
}

// n must be odd and above 1.
static void mod_init(mod_t* m, uint64_t n) {
    uint64_t inv = n;  // right to 3 bits, and each step doubles that
    for (int i = 0; i < 5; i++) inv *= 2 - n * inv;
    m->n = n;
    m->ninv = inv;
    m->one = (0 - n) % n;
}

static uint64_t mm_mul(const mod_t* m, uint64_t a, uint64_t b) {
    uint64_t hi = mulhi(a, b), q = mulhi(a * b * m->ninv, m->n);
    return hi >= q ? hi - q : hi - q + m->n;
}

// The small number x (below n), in Montgomery form: one added x times.
static uint64_t mm_small(const mod_t* m, uint32_t x) {
    uint64_t r = 0;
    while (x--) r = addmod(r, m->one, m->n);
    return r;
}

static uint64_t mm_pow(const mod_t* m, uint64_t a, uint64_t e) {
    uint64_t r = m->one;
    while (e) {
        if (e & 1) r = mm_mul(m, r, a);
        a = mm_mul(m, a, a);
        e >>= 1;
    }
    return r;
}

static uint64_t gcd(uint64_t a, uint64_t b) {
    while (b) {
        uint64_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

// ---- primes --------------------------------------------------------------------

// No composite number below 3.18e23 passes Miller-Rabin to all of these (the
// first twelve primes), which is far past 2^64. Fewer is not enough: the
// composite 3825123056546413051 passes for the first eleven.
static const uint32_t BASES[12] = { 2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37 };

int fc_is_prime(uint64_t n) {
    if (n < 2) return 0;
    for (int i = 0; i < 12; i++) {
        if (n == BASES[i]) return 1;
        if (n % BASES[i] == 0) return 0;
    }
    mod_t m;
    mod_init(&m, n);
    uint64_t minus1 = n - m.one;  // -1, in Montgomery form
    uint64_t d = n - 1;
    int s = 0;
    while (!(d & 1)) {
        d >>= 1;
        s++;
    }
    for (int i = 0; i < 12; i++) {
        uint64_t x = mm_pow(&m, mm_small(&m, BASES[i]), d);
        if (x == m.one || x == minus1) continue;
        int composite = 1;
        for (int r = 1; r < s; r++) {
            x = mm_mul(&m, x, x);
            if (x == minus1) {
                composite = 0;
                break;
            }
        }
        if (composite) return 0;
    }
    return 1;
}

// ---- finding a factor ------------------------------------------------------------

static uint64_t absdiff(uint64_t a, uint64_t b) { return a > b ? a - b : b - a; }

// Brent's version of Pollard's rho on x -> x*x + c: two walks through the
// numbers mod n meet sooner mod a factor p (after about sqrt(p) steps) than
// mod n, and then x - y is a multiple of p. The differences are multiplied
// together 64 at a time to save on gcds. Returns a factor of n that is
// neither 1 nor n, or 0 when this c finds none (it found both factors at
// once).
static uint64_t rho(uint64_t n, uint32_t c) {
    mod_t md;
    mod_init(&md, n);
    uint64_t cc = mm_small(&md, c);
    uint64_t x = mm_small(&md, 2), y = x, q = md.one, g = 1;
    uint64_t r = 1;
    const uint64_t m = 64;
#define STEP(v) addmod(mm_mul(&md, (v), (v)), cc, n)
    do {
        x = y;
        for (uint64_t i = 0; i < r; i++) y = STEP(y);
        uint64_t k = 0;
        do {
            uint64_t lim = r - k < m ? r - k : m;
            for (uint64_t i = 0; i < lim; i++) {
                y = STEP(y);
                q = mm_mul(&md, q, absdiff(x, y));
            }
            g = gcd(q, n);
            k += m;
        } while (k < r && g == 1);
        r *= 2;
    } while (g == 1);
#undef STEP
    return g == n ? 0 : g;
}

static uint32_t push(uint64_t* out, uint32_t cnt, uint64_t p) {
    out[cnt] = p;
    return cnt + 1;
}

// n is odd and above 1, and has no prime factor below 1000 other than itself.
static uint32_t split(uint64_t n, uint64_t* out, uint32_t cnt) {
    if (fc_is_prime(n)) return push(out, cnt, n);
    uint64_t f = 0;
    for (uint32_t c = 1; !f; c++) f = rho(n, c);
    cnt = split(f, out, cnt);
    return split(n / f, out, cnt);
}

uint32_t fc_factor(uint64_t n, uint64_t* out) {
    uint32_t cnt = 0;
    if (n < 2) return 0;
    while (!(n & 1)) {
        cnt = push(out, cnt, 2);
        n >>= 1;
    }
    for (uint32_t d = 3; d < 1000 && (uint64_t)d * d <= n; d += 2) {
        while (n % d == 0) {
            cnt = push(out, cnt, d);
            n /= d;
        }
    }
    if (n > 1) cnt = split(n, out, cnt);
    // The pieces from split come in no order: sort them (there are few).
    for (uint32_t i = 1; i < cnt; i++) {
        uint64_t v = out[i];
        uint32_t j = i;
        while (j && out[j - 1] > v) {
            out[j] = out[j - 1];
            j--;
        }
        out[j] = v;
    }
    return cnt;
}
