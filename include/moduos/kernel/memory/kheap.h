#ifndef MODUOS_KERNEL_MEMORY_KHEAP_H
#define MODUOS_KERNEL_MEMORY_KHEAP_H

#include <stddef.h>

/**
 * @brief Initialize kernel heap (MUST be called during boot)
 * 
 * This MUST be called early in boot, before any kmalloc() calls.
 * On-demand initialization has a race condition on SMP systems.
 */
void kheap_init(void);

/**
 * @brief Allocate memory from kernel heap
 */
void *kmalloc(size_t size);

/**
 * @brief Allocate aligned memory from kernel heap
 */
void *kmalloc_aligned(size_t size, size_t alignment);

/**
 * @brief Free memory allocated by kmalloc
 */
void kfree(void *ptr);

#endif /* MODUOS_KERNEL_MEMORY_KHEAP_H */
