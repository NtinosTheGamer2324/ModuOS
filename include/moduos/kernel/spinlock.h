#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "moduos/kernel/interrupts/irq_lock.h"

/*
 * Production-grade SMP spinlock for ModuOS
 * - Test-and-test-and-set (reduces cache thrashing)
 * - IRQ-safe variants
 * - Cacheline aligned
 * - Optional debug owner tracking
 */

#ifndef CACHELINE_SIZE
#define CACHELINE_SIZE 64
#endif

typedef struct {
    volatile uint32_t v;

#ifdef SPINLOCK_DEBUG
    uint64_t owner_cpu;
#endif

} __attribute__((aligned(CACHELINE_SIZE))) spinlock_t;


/* ===================== */
/* Low-level primitives  */
/* ===================== */

static inline void cpu_relax(void) {
    __asm__ volatile("pause");
}


/* ===================== */
/* Basic Locking         */
/* ===================== */

static inline void spinlock_init(spinlock_t *l) {
    l->v = 0;
#ifdef SPINLOCK_DEBUG
    l->owner_cpu = (uint64_t)-1;
#endif
}

/*
 * Test-and-test-and-set spinlock.
 * Reduces locked bus traffic under contention.
 */
static inline void spinlock_lock(spinlock_t *l) {
    for (;;) {

        /* Fast path */
        if (!__atomic_test_and_set(&l->v, __ATOMIC_ACQUIRE))
            break;

        /* Contended path (relaxed spin) */
        while (__atomic_load_n(&l->v, __ATOMIC_RELAXED)) {
            cpu_relax();
        }
    }

#ifdef SPINLOCK_DEBUG
    extern uint64_t get_cpu_id(void);
    l->owner_cpu = get_cpu_id();
#endif
}

static inline bool spinlock_trylock(spinlock_t *l) {
    if (!__atomic_test_and_set(&l->v, __ATOMIC_ACQUIRE)) {
#ifdef SPINLOCK_DEBUG
        extern uint64_t get_cpu_id(void);
        l->owner_cpu = get_cpu_id();
#endif
        return true;
    }
    return false;
}

static inline void spinlock_unlock(spinlock_t *l) {

#ifdef SPINLOCK_DEBUG
    l->owner_cpu = (uint64_t)-1;
#endif

    __atomic_clear(&l->v, __ATOMIC_RELEASE);
}

static inline void spinlock_lock_irqsave(spinlock_t *l, uint64_t *flags) {
    *flags = irq_save();
    spinlock_lock(l);
}

static inline void spinlock_unlock_irqrestore(spinlock_t *l, uint64_t flags) {
    spinlock_unlock(l);
    irq_restore(flags);
}