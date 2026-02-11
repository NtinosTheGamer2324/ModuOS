#ifndef MODUOS_FS_MDFS_CACHE_H
#define MODUOS_FS_MDFS_CACHE_H

#include <stdint.h>
#include <stddef.h>
#include "moduos/fs/MDFS/mdfs.h"

/**
 * @file mdfs_cache.h
 * @brief MDFS caching system for performance optimization
 * 
 * Provides:
 * - Pre-allocated buffer pool (eliminates kmalloc/kfree overhead)
 * - Block cache (reduces disk I/O)
 * - Inode cache (reduces inode read/write cycles)
 */

/* ============================================================================
 * Buffer Pool - Pre-allocated temporary buffers
 * ============================================================================ */

#define MDFS_BUFFER_POOL_SIZE 32  /* Increased from 8 for better performance */

/**
 * @brief Get a temporary buffer from the pool
 * @return Pointer to 4KB buffer, or NULL if pool exhausted
 * 
 * Must call mdfs_buffer_release() when done.
 * Falls back to kmalloc if pool is full.
 */
uint8_t* mdfs_buffer_acquire(void);

/**
 * @brief Release a temporary buffer back to the pool
 * @param buf Buffer to release (can be from pool or kmalloc)
 */
void mdfs_buffer_release(uint8_t *buf);

/**
 * @brief Initialize the buffer pool
 */
void mdfs_buffer_pool_init(void);

/* ============================================================================
 * Block Cache - LRU cache for disk blocks
 * ============================================================================ */

#define MDFS_BLOCK_CACHE_SIZE 1024  // 4MB cache (8x larger for speed)

typedef struct {
    uint64_t block_no;        // Block number (0 = unused entry)
    int      fs_handle;       // Filesystem handle
    uint32_t dirty;           // 1 if needs write-back
    uint32_t access_count;    // For LRU eviction
    uint8_t  data[MDFS_BLOCK_SIZE];
} mdfs_block_cache_entry_t;

/**
 * @brief Initialize block cache
 */
void mdfs_block_cache_init(void);

/**
 * @brief Read a block (uses cache)
 * @param fs_handle Filesystem handle
 * @param block_no Block number to read
 * @param out Output buffer (4KB)
 * @return 0 on success, negative on error
 */
int mdfs_block_cache_read(int fs_handle, uint64_t block_no, void *out);

/**
 * @brief Write a block (uses write-back cache)
 * @param fs_handle Filesystem handle
 * @param block_no Block number to write
 * @param data Data to write (4KB)
 * @return 0 on success, negative on error
 */
int mdfs_block_cache_write(int fs_handle, uint64_t block_no, const void *data);

/**
 * @brief Flush all dirty blocks to disk
 * @param fs_handle Filesystem handle (or -1 for all)
 * @return 0 on success, negative on error
 */
int mdfs_block_cache_flush(int fs_handle);

/**
 * @brief Invalidate cache entries for a filesystem
 * @param fs_handle Filesystem handle
 */
void mdfs_block_cache_invalidate(int fs_handle);

/* ============================================================================
 * Inode Cache - In-memory inode cache
 * ============================================================================ */

#define MDFS_INODE_CACHE_SIZE 512  /* 8x larger for speed */

typedef struct {
    uint32_t ino_num;         // Inode number (0 = unused)
    int      fs_handle;       // Filesystem handle
    uint32_t dirty;           // 1 if needs write-back
    uint32_t access_count;    // For LRU eviction
    mdfs_inode_t inode;       // Cached inode data
} mdfs_inode_cache_entry_t;

/**
 * @brief Initialize inode cache
 */
void mdfs_inode_cache_init(void);

/**
 * @brief Read an inode (uses cache)
 * @param fs_handle Filesystem handle
 * @param ino_num Inode number
 * @param out Output inode
 * @return 0 on success, negative on error
 */
int mdfs_inode_cache_read(int fs_handle, uint32_t ino_num, mdfs_inode_t *out);

/**
 * @brief Write an inode (uses write-back cache)
 * @param fs_handle Filesystem handle
 * @param ino_num Inode number
 * @param ino Inode data
 * @return 0 on success, negative on error
 */
int mdfs_inode_cache_write(int fs_handle, uint32_t ino_num, const mdfs_inode_t *ino);

/**
 * @brief Flush all dirty inodes to disk
 * @param fs_handle Filesystem handle (or -1 for all)
 * @return 0 on success, negative on error
 */
int mdfs_inode_cache_flush(int fs_handle);

/**
 * @brief Invalidate cache entries for a filesystem
 * @param fs_handle Filesystem handle
 */
void mdfs_inode_cache_invalidate(int fs_handle);

/* ============================================================================
 * Statistics
 * ============================================================================ */

typedef struct {
    uint64_t buffer_pool_hits;
    uint64_t buffer_pool_fallbacks;
    uint64_t block_cache_hits;
    uint64_t block_cache_misses;
    uint64_t inode_cache_hits;
    uint64_t inode_cache_misses;
    uint64_t disk_reads;
    uint64_t disk_writes;
} mdfs_cache_stats_t;

/**
 * @brief Get cache statistics
 */
void mdfs_cache_get_stats(mdfs_cache_stats_t *stats);

/**
 * @brief Reset cache statistics
 */
void mdfs_cache_reset_stats(void);

#endif /* MODUOS_FS_MDFS_CACHE_H */
