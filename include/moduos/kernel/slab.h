#ifndef MODUOS_KERNEL_SLAB_H
#define MODUOS_KERNEL_SLAB_H

#include <stddef.h>
#include <stdint.h>

/**
 * @file slab.h
 * @brief SLAB allocator for high-performance object allocation
 * 
 * Pre-allocates common object sizes for instant allocation/deallocation.
 * Expected speedup: 10-50x for kmalloc/kfree of common sizes.
 * 
 * Common sizes:
 * - 16, 32, 64, 128, 256, 512, 1024, 2048, 4096 bytes
 */

typedef struct slab_cache slab_cache_t;

/**
 * @brief Initialize SLAB allocator
 * Must be called during kernel initialization
 */
void slab_init(void);

/**
 * @brief Create a SLAB cache for specific object size
 * 
 * @param name Cache name (for debugging)
 * @param object_size Size of each object
 * @param align Alignment requirement (must be power of 2)
 * @return Cache handle, or NULL on failure
 */
slab_cache_t *slab_cache_create(const char *name, size_t object_size, size_t align);

/**
 * @brief Allocate object from SLAB cache
 * 
 * @param cache SLAB cache
 * @return Pointer to object, or NULL if cache exhausted
 * 
 * Fast path: O(1) allocation from freelist, no locks (per-CPU)
 */
void *slab_alloc(slab_cache_t *cache);

/**
 * @brief Free object back to SLAB cache
 * 
 * @param cache SLAB cache
 * @param obj Object to free
 * 
 * Fast path: O(1) free to freelist, no locks (per-CPU)
 */
void slab_free(slab_cache_t *cache, void *obj);

/**
 * @brief Fast kmalloc using SLAB caches
 * 
 * Automatically routes to appropriate SLAB cache based on size.
 * Falls back to regular kmalloc for large or odd sizes.
 * 
 * @param size Bytes to allocate
 * @return Pointer to memory, or NULL on failure
 */
void *slab_kmalloc(size_t size);

/**
 * @brief Fast kfree using SLAB caches
 * 
 * @param ptr Pointer to free
 */
void slab_kfree(void *ptr);

/**
 * @brief Get SLAB cache statistics
 * 
 * @param cache SLAB cache
 * @param objects_allocated Output: number of allocated objects
 * @param objects_free Output: number of free objects
 * @param total_memory Output: total memory used
 */
void slab_cache_stats(slab_cache_t *cache, size_t *objects_allocated, 
                      size_t *objects_free, size_t *total_memory);

#endif /* MODUOS_KERNEL_SLAB_H */
