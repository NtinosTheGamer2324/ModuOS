#include "moduos/kernel/blockdev.h"
#include "moduos/kernel/memory/memory.h"
#include "moduos/kernel/memory/string.h"

#define MAX_BLOCKDEVS 64

typedef struct {
    int in_use;
    const blockdev_ops_t *ops;
    void *ctx;
} blockdev_entry_t;

static blockdev_entry_t g_blockdevs[MAX_BLOCKDEVS];

blockdev_handle_t blockdev_register(const blockdev_ops_t *ops, void *ctx) {
    if (!ops || !ops->get_info || !ops->read) return BLOCKDEV_INVALID_HANDLE;

    for (uint32_t i = 1; i < MAX_BLOCKDEVS; i++) {
        if (!g_blockdevs[i].in_use) {
            g_blockdevs[i].in_use = 1;
            g_blockdevs[i].ops = ops;
            g_blockdevs[i].ctx = ctx;
            return (blockdev_handle_t)i;
        }
    }

    return BLOCKDEV_INVALID_HANDLE;
}

static blockdev_entry_t* blockdev_get(blockdev_handle_t h) {
    if (h == 0 || h >= MAX_BLOCKDEVS) return NULL;
    if (!g_blockdevs[h].in_use) return NULL;
    return &g_blockdevs[h];
}

int blockdev_get_info(blockdev_handle_t h, blockdev_info_t *out) {
    blockdev_entry_t *e = blockdev_get(h);
    if (!e || !out) return -1;
    return e->ops->get_info(e->ctx, out);
}

int blockdev_read(blockdev_handle_t h, uint64_t lba, uint32_t count, void *buf, size_t buf_sz) {
    blockdev_entry_t *e = blockdev_get(h);
    if (!e || !buf) return -1;

    blockdev_info_t info;
    if (e->ops->get_info(e->ctx, &info) != 0) return -2;
    if (info.sector_size == 0) return -3;

    uint64_t need = (uint64_t)count * (uint64_t)info.sector_size;
    if ((uint64_t)buf_sz < need) return -4;
    if (count == 0) return 0;
    if (lba >= info.sector_count) return -5;
    if (lba + count > info.sector_count) return -6;

    return e->ops->read(e->ctx, lba, count, buf, buf_sz);
}

int blockdev_write(blockdev_handle_t h, uint64_t lba, uint32_t count, const void *buf, size_t buf_sz) {
    blockdev_entry_t *e = blockdev_get(h);
    if (!e || !buf) return -1;

    blockdev_info_t info;
    if (e->ops->get_info(e->ctx, &info) != 0) return -2;
    if (info.sector_size == 0) return -3;

    // EROFS-style behavior for read-only devices
    if (info.flags & BLOCKDEV_F_READONLY) return -30;

    uint64_t need = (uint64_t)count * (uint64_t)info.sector_size;
    if ((uint64_t)buf_sz < need) return -4;
    if (count == 0) return 0;
    if (lba >= info.sector_count) return -5;
    if (lba + count > info.sector_count) return -6;

    if (!e->ops->write) return -31;
    return e->ops->write(e->ctx, lba, count, buf, buf_sz);
}
