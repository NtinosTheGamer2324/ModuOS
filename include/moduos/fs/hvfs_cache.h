#ifndef HVFS_CACHE_H
#define HVFS_CACHE_H

#include <stddef.h>
#include <stdint.h>

/* HVFS read cache:
 * Caches full-file reads keyed by (mount_slot, path).
 */

typedef enum {
    HVFS_CACHE_32M = 0,
    HVFS_CACHE_128M = 1,
    HVFS_CACHE_UNLIMITED = 2
} hvfs_cache_mode_t;

void hvfs_cache_set_mode(hvfs_cache_mode_t mode);
hvfs_cache_mode_t hvfs_cache_get_mode(void);

/* Stats */
uint64_t hvfs_cache_bytes_used(void);
uint64_t hvfs_cache_hits(void);
uint64_t hvfs_cache_misses(void);

/* Cache get/put (internal use) */
int hvfs_cache_lookup(int mount_slot, const char *path, void **outbuf, size_t *out_size);
int hvfs_cache_store(int mount_slot, const char *path, void *buf, size_t size);

/* Release a cached buffer (decrement refcount). Non-cached buffers can be freed normally. */
int hvfs_cache_release(int mount_slot, const char *path, void *buf);

#endif
