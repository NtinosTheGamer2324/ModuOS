#pragma once

#include <stdint.h>

/*
 * Very small IRQ save/restore helpers for critical sections.
 * These are safe to use in both IRQ and process context.
 */
static inline uint64_t irq_save(void) {
    uint64_t flags;
    __asm__ volatile(
        "pushfq\n"
        "pop %0\n"
        "cli\n"
        : "=r"(flags)
        :
        : "memory"
    );
    return flags;
}

static inline void irq_restore(uint64_t flags) {
    /* Restore IF from saved flags */
    if (flags & (1ULL << 9)) {
        __asm__ volatile("sti" ::: "memory");
    } else {
        __asm__ volatile("cli" ::: "memory");
    }
}
