#ifndef MODUOS_KERNEL_PERCPU_HEAP_H
#define MODUOS_KERNEL_PERCPU_HEAP_H

#include <stddef.h>
#include <stdint.h>

/**
 * @file percpu_heap.h
 * @brief Per-CPU heap allocators for lock-free performance
 * 
 * Each CPU has its own heap, eliminating lock contention.
 * Falls back to shared heap if per-CPU heap is exhausted.
 * 
 * Expected speedup: 5-10x for kmalloc/kfree operations.
 */

#define MAX_CPUS 64
#define PERCPU_HEAP_SIZE (4 * 1024 * 1024)  /* 4MB per CPU */

/**
 * @brief Initialize per-CPU heaps
 * Must be called after CPU detection
 */
void percpu_heap_init(void);

/**
 * @brief Allocate from current CPU's heap (lock-free!)
 * 
 * @param size Bytes to allocate
 * @return Pointer to allocated memory, or NULL on failure
 * 
 * Fast path: O(1) allocation from per-CPU heap, no locks
 * Slow path: Falls back to global heap if per-CPU heap full
 */
void *percpu_kmalloc(size_t size);

/**
 * @brief Free memory allocated by percpu_kmalloc
 * 
 * @param ptr Pointer to free
 * 
 * Fast path: O(1) free to per-CPU heap, no locks
 * Slow path: Routes to global heap if not from per-CPU heap
 */
void percpu_kfree(void *ptr);

/**
 * @brief Get statistics for per-CPU heaps
 * 
 * @param cpu CPU ID
 * @param total_bytes Output: total heap size
 * @param used_bytes Output: bytes allocated
 * @param free_bytes Output: bytes available
 */
void percpu_heap_stats(int cpu, size_t *total_bytes, size_t *used_bytes, size_t *free_bytes);

#endif /* MODUOS_KERNEL_PERCPU_HEAP_H */
