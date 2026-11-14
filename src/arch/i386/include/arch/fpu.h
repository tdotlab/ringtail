// Per-task floating-point state, i386: the 108-byte fnsave image of
// the x87 registers. Programs are built without SSE, so that is all
// the state there is.
#ifndef RINGTAIL_ARCH_FPU_H
#define RINGTAIL_ARCH_FPU_H

#include <stdint.h>

typedef struct { uint8_t b[108]; } fpu_state_t;

// fnsave also resets the unit, which does no harm: the next thing to
// touch it is fpu_restore.
static inline void fpu_save(fpu_state_t* s) { __asm__ volatile("fnsave %0" : "=m"(*s)); }
static inline void fpu_restore(const fpu_state_t* s) { __asm__ volatile("frstor %0" :: "m"(*s)); }

// Turn the unit on for programs and reset it (cpu.c).
void fpu_enable(void);

#endif
