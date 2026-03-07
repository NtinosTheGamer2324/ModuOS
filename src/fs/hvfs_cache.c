#include "moduos/fs/hvfs_cache.h"
#include "moduos/kernel/spinlock.h"
#include "moduos/kernel/memory/memory.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/COM/com.h"

/* Simple fixed-entry cache with refcount.
 * Policy: on insert, evict LRU entries with refcount==0 until under cap.
 */

#ifndef HVFS_CACHE_MAX_ENTRIES
#define HVFS_CACHE_MAX_ENTRIES 128
#endif

typedef struct {
    int in_use;
    int mount_slot;
    char path[256];
    void *buf;
    size_t size;
    uint32_t refcnt;
    uint64_t last_use; /* monotonic counter */
} hvfs_cache_entry_t;

static hvfs_cache_entry_t g_cache[HVFS_CACHE_MAX_ENTRIES];
static hvfs_cache_mode_t g_mode = HVFS_CACHE_128M;
static uint64_t g_use_counter = 1;
static uint64_t g_hits = 0;
static uint64_t g_misses = 0;

static uint64_t cap_bytes(void) {
    switch (g_mode) {
        case HVFS_CACHE_32M: return 32ULL * 1024ULL * 1024ULL;
        case HVFS_CACHE_128M: return 128ULL * 1024ULL * 1024ULL;
        case HVFS_CACHE_UNLIMITED: default: return (uint64_t)-1;
    }
}

uint64_t hvfs_cache_bytes_used(void) {
    uint64_t sum = 0;
    for (int i = 0; i < HVFS_CACHE_MAX_ENTRIES; i++) {
        if (g_cache[i].in_use) sum += (uint64_t)g_cache[i].size;
    }
    return sum;
}

uint64_t hvfs_cache_hits(void) { return g_hits; }
uint64_t hvfs_cache_misses(void) { return g_misses; }

void hvfs_cache_set_mode(hvfs_cache_mode_t mode) {
    g_mode = mode;
}

hvfs_cache_mode_t hvfs_cache_get_mode(void) {
    return g_mode;
}

static int match(int mount_slot, const char *path, const hvfs_cache_entry_t *e) {
    if (!e->in_use) return 0;
    if (e->mount_slot != mount_slot) return 0;
    return (strcmp(e->path, path) == 0);
}

int hvfs_cache_lookup(int mount_slot, const char *path, void **outbuf, size_t *out_size) {
    if (outbuf) *outbuf = NULL;
    if (out_size) *out_size = 0;
    if (!path || !*path) return 0;

    for (int i = 0; i < HVFS_CACHE_MAX_ENTRIES; i++) {
        hvfs_cache_entry_t *e = &g_cache[i];
        if (!match(mount_slot, path, e)) continue;
        e->refcnt++;
        e->last_use = g_use_counter++;
        if (outbuf) *outbuf = e->buf;
        if (out_size) *out_size = e->size;
        g_hits++;
        return 1;
    }

    g_misses++;
    return 0;
}

static int find_free_slot(void) {
    for (int i = 0; i < HVFS_CACHE_MAX_ENTRIES; i++) {
        if (!g_cache[i].in_use) return i;
    }
    return -1;
}

static int find_lru_evictable(void) {
    int best = -1;
    uint64_t best_use = (uint64_t)-1;
    for (int i = 0; i < HVFS_CACHE_MAX_ENTRIES; i++) {
        hvfs_cache_entry_t *e = &g_cache[i];
        if (!e->in_use) continue;
        if (e->refcnt != 0) continue;
        if (e->last_use < best_use) {
            best_use = e->last_use;
            best = i;
        }
    }
    return best;
}

static void evict_one(int idx) {
    hvfs_cache_entry_t *e = &g_cache[idx];
    if (!e->in_use) return;
    if (e->refcnt != 0) return;
    if (e->buf) kfree(e->buf);
    memset(e, 0, sizeof(*e));
}

int hvfs_cache_store(int mount_slot, const char *path, void *buf, size_t size) {
    if (!path || !*path || !buf) return 0;

    /* Don’t store if unlimited is off and file is bigger than the whole cap. */
    uint64_t cap = cap_bytes();
    if (cap != (uint64_t)-1 && (uint64_t)size > cap) return 0;

    /* Ensure enough space (evict LRU entries with refcnt==0). */
    if (cap != (uint64_t)-1) {
        while (hvfs_cache_bytes_used() + (uint64_t)size > cap) {
            int ev = find_lru_evictable();
            if (ev < 0) return 0;
            evict_one(ev);
        }
    }

    int slot = find_free_slot();
    if (slot < 0) {
        /* try to evict one */
        int ev = find_lru_evictable();
        if (ev < 0) return 0;
        evict_one(ev);
        slot = find_free_slot();
        if (slot < 0) return 0;
    }

    hvfs_cache_entry_t *e = &g_cache[slot];
    e->in_use = 1;
    e->mount_slot = mount_slot;
    strncpy(e->path, path, sizeof(e->path) - 1);
    e->path[sizeof(e->path) - 1] = 0;
    e->buf = buf;
    e->size = size;
    e->refcnt = 1; /* caller gets a reference */
    e->last_use = g_use_counter++;
    return 1;
}

int hvfs_cache_release(int mount_slot, const char *path, void *buf) {
    (void)mount_slot;
    (void)path;
    if (!buf) return 0;

    for (int i = 0; i < HVFS_CACHE_MAX_ENTRIES; i++) {
        hvfs_cache_entry_t *e = &g_cache[i];
        if (!e->in_use) continue;
        if (e->buf != buf) continue;
        if (e->refcnt > 0) e->refcnt--;
        e->last_use = g_use_counter++;
        return 1;
    }

    return 0;
}
