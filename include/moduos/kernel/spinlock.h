#pragma once
#include <stdint.h>

/* Very small spinlock for SMP. */
typedef struct {
    volatile uint32_t v;
} spinlock_t;

static inline void spinlock_init(spinlock_t *l) {
    l->v = 0;
}

static inline void spinlock_lock(spinlock_t *l) {
    while (__atomic_test_and_set(&l->v, __ATOMIC_ACQUIRE)) {
        __asm__ volatile("pause");
    }
}

static inline void spinlock_unlock(spinlock_t *l) {
    __atomic_clear(&l->v, __ATOMIC_RELEASE);
}
