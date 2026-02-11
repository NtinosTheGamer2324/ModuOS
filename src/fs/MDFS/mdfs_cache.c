#include "moduos/fs/MDFS/mdfs_cache.h"
#include "moduos/fs/MDFS/mdfs_disk.h"
#include "moduos/kernel/memory/memory.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/spinlock.h"


/* Global cache statistics */
static mdfs_cache_stats_t g_cache_stats = {0};

/* ============================================================================
 * Buffer Pool Implementation
 * ============================================================================ */

static uint8_t g_buffer_pool[MDFS_BUFFER_POOL_SIZE][MDFS_BLOCK_SIZE] __attribute__((aligned(16)));
static uint8_t g_buffer_in_use[MDFS_BUFFER_POOL_SIZE] = {0};
static spinlock_t g_buffer_lock;

void mdfs_buffer_pool_init(void) {
    memset(g_buffer_in_use, 0, sizeof(g_buffer_in_use));
    spinlock_init(&g_buffer_lock);
}

uint8_t* mdfs_buffer_acquire(void) {
    spinlock_lock(&g_buffer_lock);
    
    // Try to find free buffer in pool
    for (int i = 0; i < MDFS_BUFFER_POOL_SIZE; i++) {
        if (!g_buffer_in_use[i]) {
            g_buffer_in_use[i] = 1;
            spinlock_unlock(&g_buffer_lock);
            g_cache_stats.buffer_pool_hits++;
            return g_buffer_pool[i];
        }
    }
    
    spinlock_unlock(&g_buffer_lock);
    
    // Pool exhausted - fallback to kmalloc
    g_cache_stats.buffer_pool_fallbacks++;
    return (uint8_t*)kmalloc(MDFS_BLOCK_SIZE);
}

void mdfs_buffer_release(uint8_t *buf) {
    if (!buf) return;
    
    spinlock_lock(&g_buffer_lock);
    
    // Check if buffer is from pool
    for (int i = 0; i < MDFS_BUFFER_POOL_SIZE; i++) {
        if (buf == g_buffer_pool[i]) {
            g_buffer_in_use[i] = 0;
            spinlock_unlock(&g_buffer_lock);
            return;
        }
    }
    
    spinlock_unlock(&g_buffer_lock);
    
    // Was kmalloc'd - free it
    kfree(buf);
}

/* ============================================================================
 * Block Cache Implementation
 * ============================================================================ */

static mdfs_block_cache_entry_t g_block_cache[MDFS_BLOCK_CACHE_SIZE];
static spinlock_t g_block_cache_lock;
static uint32_t g_block_cache_clock = 0;

void mdfs_block_cache_init(void) {
    memset(g_block_cache, 0, sizeof(g_block_cache));
    spinlock_init(&g_block_cache_lock);
    g_block_cache_clock = 0;
}

static mdfs_block_cache_entry_t* mdfs_block_cache_find(int fs_handle, uint64_t block_no) {
    for (int i = 0; i < MDFS_BLOCK_CACHE_SIZE; i++) {
        if (g_block_cache[i].block_no == block_no && 
            g_block_cache[i].fs_handle == fs_handle &&
            block_no != 0) {
            return &g_block_cache[i];
        }
    }
    return NULL;
}

static mdfs_block_cache_entry_t* mdfs_block_cache_evict(void) {
    // Find LRU entry (lowest access_count)
    mdfs_block_cache_entry_t *victim = &g_block_cache[0];
    uint32_t min_access = g_block_cache[0].access_count;
    
    for (int i = 1; i < MDFS_BLOCK_CACHE_SIZE; i++) {
        if (g_block_cache[i].block_no == 0) {
            // Empty slot - use it
            return &g_block_cache[i];
        }
        if (g_block_cache[i].access_count < min_access) {
            min_access = g_block_cache[i].access_count;
            victim = &g_block_cache[i];
        }
    }
    
    // Flush if dirty
    if (victim->dirty && victim->block_no != 0) {
        const mdfs_fs_t *fs = mdfs_get_fs(victim->fs_handle);
        if (fs) {
            mdfs_disk_write_block(fs->vdrive_id, fs->start_lba, 
                                 victim->block_no, victim->data);
            g_cache_stats.disk_writes++;
        }
    }
    
    return victim;
}

int mdfs_block_cache_read(int fs_handle, uint64_t block_no, void *out) {
    spinlock_lock(&g_block_cache_lock);
    
    // Check cache
    mdfs_block_cache_entry_t *entry = mdfs_block_cache_find(fs_handle, block_no);
    if (entry) {
        // Cache hit
        entry->access_count = ++g_block_cache_clock;
        memcpy(out, entry->data, MDFS_BLOCK_SIZE);
        spinlock_unlock(&g_block_cache_lock);
        g_cache_stats.block_cache_hits++;
        return 0;
    }
    
    // Cache miss - read from disk
    g_cache_stats.block_cache_misses++;
    
    const mdfs_fs_t *fs = mdfs_get_fs(fs_handle);
    if (!fs) {
        spinlock_unlock(&g_block_cache_lock);
        return -1;
    }
    
    int ret = mdfs_disk_read_block(fs->vdrive_id, fs->start_lba, block_no, out);
    if (ret != 0) {
        spinlock_unlock(&g_block_cache_lock);
        return ret;
    }
    g_cache_stats.disk_reads++;
    
    // Add to cache
    entry = mdfs_block_cache_evict();
    entry->block_no = block_no;
    entry->fs_handle = fs_handle;
    entry->dirty = 0;
    entry->access_count = ++g_block_cache_clock;
    memcpy(entry->data, out, MDFS_BLOCK_SIZE);
    
    spinlock_unlock(&g_block_cache_lock);
    return 0;
}

int mdfs_block_cache_write(int fs_handle, uint64_t block_no, const void *data) {
    spinlock_lock(&g_block_cache_lock);
    
    // Check if already in cache
    mdfs_block_cache_entry_t *entry = mdfs_block_cache_find(fs_handle, block_no);
    if (!entry) {
        // Not in cache - evict and add
        entry = mdfs_block_cache_evict();
        entry->block_no = block_no;
        entry->fs_handle = fs_handle;
    }
    
    // Update cache entry
    entry->access_count = ++g_block_cache_clock;
    entry->dirty = 1;
    memcpy(entry->data, data, MDFS_BLOCK_SIZE);
    
    spinlock_unlock(&g_block_cache_lock);
    return 0;
}

int mdfs_block_cache_flush(int fs_handle) {
    spinlock_lock(&g_block_cache_lock);
    
    for (int i = 0; i < MDFS_BLOCK_CACHE_SIZE; i++) {
        if (g_block_cache[i].block_no == 0) continue;
        if (fs_handle != -1 && g_block_cache[i].fs_handle != fs_handle) continue;
        
        if (g_block_cache[i].dirty) {
            const mdfs_fs_t *fs = mdfs_get_fs(g_block_cache[i].fs_handle);
            if (fs) {
                mdfs_disk_write_block(fs->vdrive_id, fs->start_lba,
                                     g_block_cache[i].block_no, g_block_cache[i].data);
                g_cache_stats.disk_writes++;
            }
            g_block_cache[i].dirty = 0;
        }
    }
    
    spinlock_unlock(&g_block_cache_lock);
    return 0;
}

void mdfs_block_cache_invalidate(int fs_handle) {
    spinlock_lock(&g_block_cache_lock);
    
    for (int i = 0; i < MDFS_BLOCK_CACHE_SIZE; i++) {
        if (g_block_cache[i].fs_handle == fs_handle) {
            g_block_cache[i].block_no = 0;
            g_block_cache[i].dirty = 0;
        }
    }
    
    spinlock_unlock(&g_block_cache_lock);
}

/* ============================================================================
 * Inode Cache Implementation
 * ============================================================================ */

static mdfs_inode_cache_entry_t g_inode_cache[MDFS_INODE_CACHE_SIZE];
static spinlock_t g_inode_cache_lock;
static uint32_t g_inode_cache_clock = 0;

void mdfs_inode_cache_init(void) {
    memset(g_inode_cache, 0, sizeof(g_inode_cache));
    spinlock_init(&g_inode_cache_lock);
    g_inode_cache_clock = 0;
}

static mdfs_inode_cache_entry_t* mdfs_inode_cache_find(int fs_handle, uint32_t ino_num) {
    for (int i = 0; i < MDFS_INODE_CACHE_SIZE; i++) {
        if (g_inode_cache[i].ino_num == ino_num && 
            g_inode_cache[i].fs_handle == fs_handle &&
            ino_num != 0) {
            return &g_inode_cache[i];
        }
    }
    return NULL;
}

static mdfs_inode_cache_entry_t* mdfs_inode_cache_evict(void) {
    // Find LRU entry
    mdfs_inode_cache_entry_t *victim = &g_inode_cache[0];
    uint32_t min_access = g_inode_cache[0].access_count;
    
    for (int i = 1; i < MDFS_INODE_CACHE_SIZE; i++) {
        if (g_inode_cache[i].ino_num == 0) {
            return &g_inode_cache[i];
        }
        if (g_inode_cache[i].access_count < min_access) {
            min_access = g_inode_cache[i].access_count;
            victim = &g_inode_cache[i];
        }
    }
    
    // Flush if dirty
    if (victim->dirty && victim->ino_num != 0) {
        const mdfs_fs_t *fs = mdfs_get_fs(victim->fs_handle);
        if (fs) {
            mdfs_disk_write_inode(fs->vdrive_id, fs->start_lba, &fs->sb,
                                 victim->ino_num, &victim->inode);
        }
    }
    
    return victim;
}

int mdfs_inode_cache_read(int fs_handle, uint32_t ino_num, mdfs_inode_t *out) {
    spinlock_lock(&g_inode_cache_lock);
    
    // Check cache
    mdfs_inode_cache_entry_t *entry = mdfs_inode_cache_find(fs_handle, ino_num);
    if (entry) {
        entry->access_count = ++g_inode_cache_clock;
        memcpy(out, &entry->inode, sizeof(mdfs_inode_t));
        spinlock_unlock(&g_inode_cache_lock);
        g_cache_stats.inode_cache_hits++;
        return 0;
    }
    
    // Cache miss
    g_cache_stats.inode_cache_misses++;
    
    const mdfs_fs_t *fs = mdfs_get_fs(fs_handle);
    if (!fs) {
        spinlock_unlock(&g_inode_cache_lock);
        return -1;
    }
    
    int ret = mdfs_disk_read_inode(fs->vdrive_id, fs->start_lba, &fs->sb, ino_num, out);
    if (ret != 0) {
        spinlock_unlock(&g_inode_cache_lock);
        return ret;
    }
    
    // Add to cache
    entry = mdfs_inode_cache_evict();
    entry->ino_num = ino_num;
    entry->fs_handle = fs_handle;
    entry->dirty = 0;
    entry->access_count = ++g_inode_cache_clock;
    memcpy(&entry->inode, out, sizeof(mdfs_inode_t));
    
    spinlock_unlock(&g_inode_cache_lock);
    return 0;
}

int mdfs_inode_cache_write(int fs_handle, uint32_t ino_num, const mdfs_inode_t *ino) {
    spinlock_lock(&g_inode_cache_lock);
    
    // Check if already in cache
    mdfs_inode_cache_entry_t *entry = mdfs_inode_cache_find(fs_handle, ino_num);
    if (!entry) {
        entry = mdfs_inode_cache_evict();
        entry->ino_num = ino_num;
        entry->fs_handle = fs_handle;
    }
    
    entry->access_count = ++g_inode_cache_clock;
    entry->dirty = 1;
    memcpy(&entry->inode, ino, sizeof(mdfs_inode_t));
    
    spinlock_unlock(&g_inode_cache_lock);
    return 0;
}

int mdfs_inode_cache_flush(int fs_handle) {
    spinlock_lock(&g_inode_cache_lock);
    
    for (int i = 0; i < MDFS_INODE_CACHE_SIZE; i++) {
        if (g_inode_cache[i].ino_num == 0) continue;
        if (fs_handle != -1 && g_inode_cache[i].fs_handle != fs_handle) continue;
        
        if (g_inode_cache[i].dirty) {
            const mdfs_fs_t *fs = mdfs_get_fs(g_inode_cache[i].fs_handle);
            if (fs) {
                mdfs_disk_write_inode(fs->vdrive_id, fs->start_lba, &fs->sb,
                                     g_inode_cache[i].ino_num, &g_inode_cache[i].inode);
            }
            g_inode_cache[i].dirty = 0;
        }
    }
    
    spinlock_unlock(&g_inode_cache_lock);
    return 0;
}

void mdfs_inode_cache_invalidate(int fs_handle) {
    spinlock_lock(&g_inode_cache_lock);
    
    for (int i = 0; i < MDFS_INODE_CACHE_SIZE; i++) {
        if (g_inode_cache[i].fs_handle == fs_handle) {
            g_inode_cache[i].ino_num = 0;
            g_inode_cache[i].dirty = 0;
        }
    }
    
    spinlock_unlock(&g_inode_cache_lock);
}

/* ============================================================================
 * Statistics
 * ============================================================================ */

void mdfs_cache_get_stats(mdfs_cache_stats_t *stats) {
    if (stats) {
        memcpy(stats, &g_cache_stats, sizeof(mdfs_cache_stats_t));
    }
}

void mdfs_cache_reset_stats(void) {
    memset(&g_cache_stats, 0, sizeof(g_cache_stats));
}
