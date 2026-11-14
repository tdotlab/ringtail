// The arithmetic of factor: reading a number, telling a prime from a
// composite, and splitting a number below 2^64 into primes. There is no
// 128-bit type on i386, so the multiplication modulo n is done by hand.
#ifndef RINGTAIL_FACTORCORE_H
#define RINGTAIL_FACTORCORE_H

#include <stdint.h>

#define FC_OK 0
#define FC_INVALID 1  // not (spaces, a '+' and) digits
#define FC_TOO_BIG 2  // digits only, but more than 2^64-1

// Reads a number as GNU factor does: any spaces, an optional '+', then digits
// (leading zeros are fine), nothing else. Leaves *out alone for anything but FC_OK.
int fc_parse(const char* s, uint64_t* out);

// 1 if n is prime. (Miller-Rabin with the first twelve primes as bases,
// which is exact for every n below 3.18e23, so for all of 64 bits.)
int fc_is_prime(uint64_t n);

// Writes the prime factors of n, smallest first and repeated as often as
// they divide n, to out (room for 64 is enough). Returns how many: none for
// 0 and 1.
uint32_t fc_factor(uint64_t n, uint64_t* out);

// The number as decimal text (out needs 21 bytes).
void fc_u64_text(uint64_t v, char* out);

#endif
