#pragma once

#include <stdint.h>

/*
 * Halt until the next interrupt, without changing the caller's interrupt-enable state.
 *
 * - If IF was already enabled, execute plain HLT.
 * - If IF was disabled, temporarily STI, HLT, then CLI to restore IF=0.
 */
static inline void hlt_wait_preserve_if(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0" : "=r"(flags) :: "memory");

    if (flags & (1ULL << 9)) {
        __asm__ volatile("hlt" ::: "memory");
    } else {
        __asm__ volatile("sti; hlt; cli" ::: "memory");
    }
}
