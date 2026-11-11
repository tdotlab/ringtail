// Host test for certificates and path validation (src/programs/lib/x509.c)
// against C2SP's x509-limbo and cases of our own: for each case, the
// trusted roots (read through the PEM loader), the intermediates the
// server would send, the server's certificate, the name and moment to
// check them for, and whether the path must be accepted, or with which
// error (x509_cases.h, which tests/gen_x509_cases.py makes; where we
// refuse what limbo accepts or the other way round, on purpose, the
// generator says why). Also: every root in disk/roots.pem reads. Run
// under the address and undefined-behaviour sanitizers.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "x509.h"
#include "x509_cases.h"

static int failures, checks;

static void check(int ok, const char* what, const char* id) {
    checks++;
    if (ok) return;
    failures++;
    if (failures <= 40) printf("FAIL: %s (%s)\n", what, id);
}

#define N(a) (sizeof(a) / sizeof *(a))

static uint8_t* unbase64(const char* s, uint32_t* n) {
    static const char abc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    uint8_t* out = malloc(strlen(s) * 3 / 4 + 3);
    uint32_t acc = 0, bits = 0;
    *n = 0;
    for (; *s && *s != '='; s++) {
        acc = acc << 6 | (uint32_t)(strchr(abc, *s) - abc);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[(*n)++] = (uint8_t)(acc >> bits);
        }
    }
    return out;
}

static const char* const errs[] = { "bad", "unsupported", "expired", "unknown CA", "name", "signature" };

static const char* err_name(int r) {
    if (r == 0) return "accepted";
    if (r <= TLS_E_CERT_BAD && r >= TLS_E_CERT_SIGNATURE) return errs[TLS_E_CERT_BAD - r];
    return "other";
}

static void run_case(const x509_case_t* c, unsigned* accepted) {
    // The roots, as one PEM file, 64 characters a line.
    x509_roots_t roots;
    memset(&roots, 0, sizeof roots);
    size_t cap = 64;
    for (const char* p = c->roots; *p;) {
        unsigned i = (unsigned)strtoul(p, (char**)&p, 10);
        cap += strlen(x509_certs[i]) * 66 / 64 + 64;
    }
    char* pem = malloc(cap);
    size_t len = 0;
    unsigned want_roots = 0;
    for (const char* p = c->roots; *p;) {
        unsigned i = (unsigned)strtoul(p, (char**)&p, 10);
        const char* b = x509_certs[i];
        len += (size_t)sprintf(pem + len, "-----BEGIN CERTIFICATE-----\n");
        for (size_t at = 0, n = strlen(b); at < n; at += 64) len += (size_t)sprintf(pem + len, "%.64s\n", b + at);
        len += (size_t)sprintf(pem + len, "-----END CERTIFICATE-----\n");
        want_roots++;
    }
    int added = x509_roots_add_pem(&roots, pem, (uint32_t)len);
    check(added >= 0 && (unsigned)added <= want_roots, "root count", c->id);
    free(pem);

    // The leaf, then the intermediates.
    const uint8_t* cert[32];
    uint32_t cert_len[32], n = 1;
    cert[0] = unbase64(x509_certs[c->leaf], &cert_len[0]);
    for (const char* p = c->inter; *p && n < 32;) {
        unsigned i = (unsigned)strtoul(p, (char**)&p, 10);
        cert[n] = unbase64(x509_certs[i], &cert_len[n]);
        n++;
    }
    unsigned y, mo, d, h, mi, s;
    sscanf(c->when, "%4u%2u%2u%2u%2u%2u", &y, &mo, &d, &h, &mi, &s);
    x509_policy_t pol = { &roots, x509_time(y, mo, d, h, mi, s), c->max_depth, c->leaf_ku };
    uint8_t ip[16];
    uint32_t ip_len = (uint32_t)strlen(c->ip) / 2;
    for (uint32_t i = 0; i < ip_len; i++) {
        unsigned v;
        sscanf(c->ip + 2 * i, "%2x", &v);
        ip[i] = (uint8_t)v;
    }
    int r = x509_verify_chain(&pol, cert, cert_len, n, *c->dns ? c->dns : 0, ip, ip_len);
    char what[96];
    snprintf(what, sizeof what, "%s, should be %s", err_name(r), c->ok ? "accepted" : c->err ? err_name(c->err) : "refused");
    check(c->err ? r == c->err : (r == 0) == c->ok, what, c->id);
    if (r == 0) (*accepted)++;
    for (uint32_t i = 0; i < n; i++) free((void*)cert[i]);
    x509_roots_free(&roots);
}

// The roots fetch trusts (disk/roots.pem): every one of them reads.
static void test_disk_roots(void) {
    FILE* f = fopen("../disk/roots.pem", "rb");
    check(f != 0, "disk/roots.pem opens", "roots");
    if (!f) return;
    static char pem[1 << 20];
    size_t len = fread(pem, 1, sizeof pem - 1, f);
    fclose(f);
    pem[len] = 0;
    unsigned want = 0;
    for (const char* p = pem; (p = strstr(p, "-----BEGIN CERTIFICATE-----")); p++) want++;
    x509_roots_t roots;
    memset(&roots, 0, sizeof roots);
    int added = x509_roots_add_pem(&roots, pem, (uint32_t)len);
    char what[64];
    snprintf(what, sizeof what, "%d of %u roots read", added, want);
    check(want > 100 && added == (int)want, what, "roots");
    x509_roots_free(&roots);
}

// The PEM reader: what is between the markers, any text around them, and
// certificates that do not read are left out without stopping the rest.
static void test_pem(void) {
    const char* good = x509_certs[x509_cases[0].leaf];
    char* pem = malloc(4 * strlen(good) + 1024);
    sprintf(pem,
            "# a comment\n-----BEGIN CERTIFICATE-----\n%s\n-----END CERTIFICATE-----\n"
            "-----BEGIN CERTIFICATE-----\nnot base64!\n-----END CERTIFICATE-----\n"
            "-----BEGIN CERTIFICATE-----\nAAAA\n-----END CERTIFICATE-----\n"
            "-----BEGIN CERTIFICATE-----\r\n  %s \r\n-----END CERTIFICATE-----\r\n"
            "-----BEGIN CERTIFICATE-----\n%s\n",
            good, good, good);
    x509_roots_t roots;
    memset(&roots, 0, sizeof roots);
    check(x509_roots_add_pem(&roots, pem, (uint32_t)strlen(pem)) == 2, "PEM: two of five read", "pem");
    check(x509_roots_add_pem(&roots, "", 0) == 0, "PEM: nothing in nothing", "pem");
    check(roots.n == 2 && roots.cert[0].der.n == roots.cert[1].der.n, "PEM: both the same", "pem");
    x509_roots_free(&roots);
    free(pem);
}

int main(void) {
    test_disk_roots();
    test_pem();
    unsigned accepted = 0, differs = 0;
    for (unsigned i = 0; i < N(x509_cases); i++) {
        run_case(&x509_cases[i], &accepted);
        differs += (unsigned)x509_cases[i].differs;
    }
    printf("x509: %u cases, %u accepted, %u answered unlike limbo on purpose\n", (unsigned)N(x509_cases),
           accepted, differs);
    printf("test_x509: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
