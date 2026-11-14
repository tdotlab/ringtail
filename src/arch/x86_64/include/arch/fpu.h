// Per-task floating-point and SSE state, x86_64: the 512-byte fxsave
// image, which holds the x87 registers, the SSE registers and MXCSR.
// Every x86_64 CPU has SSE2, and the ABI passes floating-point values
// in SSE registers, so programs are built with it.
#ifndef RINGTAIL_ARCH_FPU_H
#define RINGTAIL_ARCH_FPU_H

#include <stdint.h>

typedef struct { uint8_t b[512]; } __attribute__((aligned(16))) fpu_state_t;

static inline void fpu_save(fpu_state_t* s) { __asm__ volatile("fxsave64 %0" : "=m"(*s)); }
static inline void fpu_restore(const fpu_state_t* s) { __asm__ volatile("fxrstor64 %0" :: "m"(*s)); }

// Turn the unit on for programs and reset it (cpu.c).
void fpu_enable(void);

#endif
