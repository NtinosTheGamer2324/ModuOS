#ifndef MODUOS_KERNEL_RWLOCK_H
#define MODUOS_KERNEL_RWLOCK_H

#include <stdint.h>

/**
 * @file rwlock.h
 * @brief Read-Write Lock implementation for high-performance concurrent access
 * 
 * RWLocks allow multiple readers OR single writer:
 * - Multiple readers can hold the lock simultaneously
 * - Only one writer can hold the lock
 * - Writers have priority to prevent starvation
 * 
 * Perfect for read-heavy workloads like process table lookups.
 */

typedef struct {
    volatile uint32_t readers;    // Number of active readers
    volatile uint32_t writer;     // 1 if writer active, 0 otherwise
    volatile uint32_t waiting_writers; // Writers waiting for lock
} rwlock_t;

/**
 * @brief Initialize a read-write lock
 */
static inline void rwlock_init(rwlock_t *lock) {
    lock->readers = 0;
    lock->writer = 0;
    lock->waiting_writers = 0;
}

/**
 * @brief Acquire read lock
 * Multiple readers can hold simultaneously
 */
static inline void rwlock_read_lock(rwlock_t *lock) {
    while (1) {
        // Wait if there's a writer or waiting writers
        while (lock->writer || lock->waiting_writers) {
            __asm__ volatile("pause");
        }
        
        // Try to acquire read lock
        __atomic_fetch_add(&lock->readers, 1, __ATOMIC_ACQUIRE);
        
        // Check if writer sneaked in
        if (!lock->writer && !lock->waiting_writers) {
            return; // Success
        }
        
        // Writer came in, release and retry
        __atomic_fetch_sub(&lock->readers, 1, __ATOMIC_RELEASE);
    }
}

/**
 * @brief Release read lock
 */
static inline void rwlock_read_unlock(rwlock_t *lock) {
    __atomic_fetch_sub(&lock->readers, 1, __ATOMIC_RELEASE);
}

/**
 * @brief Acquire write lock
 * Waits for all readers to finish
 */
static inline void rwlock_write_lock(rwlock_t *lock) {
    // Announce we're waiting
    __atomic_fetch_add(&lock->waiting_writers, 1, __ATOMIC_ACQUIRE);
    
    // Wait for current writer to finish
    while (__atomic_test_and_set(&lock->writer, __ATOMIC_ACQUIRE)) {
        __asm__ volatile("pause");
    }
    
    // We have writer lock, no longer waiting
    __atomic_fetch_sub(&lock->waiting_writers, 1, __ATOMIC_RELEASE);
    
    // Wait for all readers to finish
    while (__atomic_load_n(&lock->readers, __ATOMIC_ACQUIRE) > 0) {
        __asm__ volatile("pause");
    }
}

/**
 * @brief Release write lock
 */
static inline void rwlock_write_unlock(rwlock_t *lock) {
    __atomic_clear(&lock->writer, __ATOMIC_RELEASE);
}

/**
 * @brief Try to acquire read lock (non-blocking)
 * @return 1 on success, 0 if would block
 */
static inline int rwlock_try_read_lock(rwlock_t *lock) {
    if (lock->writer || lock->waiting_writers) {
        return 0;
    }
    
    __atomic_fetch_add(&lock->readers, 1, __ATOMIC_ACQUIRE);
    
    if (!lock->writer && !lock->waiting_writers) {
        return 1;
    }
    
    __atomic_fetch_sub(&lock->readers, 1, __ATOMIC_RELEASE);
    return 0;
}

/**
 * @brief Try to acquire write lock (non-blocking)
 * @return 1 on success, 0 if would block
 */
static inline int rwlock_try_write_lock(rwlock_t *lock) {
    if (lock->readers > 0) {
        return 0;
    }
    
    if (!__atomic_test_and_set(&lock->writer, __ATOMIC_ACQUIRE)) {
        if (__atomic_load_n(&lock->readers, __ATOMIC_ACQUIRE) == 0) {
            return 1;
        }
        __atomic_clear(&lock->writer, __ATOMIC_RELEASE);
    }
    
    return 0;
}

#endif /* MODUOS_KERNEL_RWLOCK_H */
