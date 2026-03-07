#include "moduos/kernel/blockdev.h"
#include "moduos/drivers/Drive/vDrive.h"
#include "moduos/kernel/memory/memory.h"
#include "moduos/kernel/memory/string.h"

typedef struct {
    uint8_t vdrive_id;
} vdrive_block_ctx_t;

// Map vDrive ID -> blockdev handle for later lookups (SQRM FS modules, etc.).
static blockdev_handle_t g_vdrive_to_handle[256];

static int vdb_get_info(void *ctx, blockdev_info_t *out) {
    if (!ctx || !out) return -1;
    vdrive_block_ctx_t *c = (vdrive_block_ctx_t*)ctx;
    vdrive_t *d = vdrive_get(c->vdrive_id);
    if (!d) return -2;

    memset(out, 0, sizeof(*out));
    out->sector_size = d->sector_size;
    out->sector_count = d->total_sectors;
    out->flags = 0;

    // Treat optical/ATAPI as read-only + removable
    if (d->type == VDRIVE_TYPE_ATA_ATAPI || d->type == VDRIVE_TYPE_SATA_OPTICAL) {
        out->flags |= BLOCKDEV_F_READONLY;
        out->flags |= BLOCKDEV_F_REMOVABLE;
    }

    strncpy(out->model, d->model, sizeof(out->model) - 1);
    out->model[sizeof(out->model) - 1] = 0;
    return 0;
}

static int vdb_read(void *ctx, uint64_t lba, uint32_t count, void *buf, size_t buf_sz) {
    (void)buf_sz;
    if (!ctx || !buf) return -1;
    vdrive_block_ctx_t *c = (vdrive_block_ctx_t*)ctx;
    return vdrive_read(c->vdrive_id, lba, count, buf);
}

static int vdb_write(void *ctx, uint64_t lba, uint32_t count, const void *buf, size_t buf_sz) {
    (void)buf_sz;
    if (!ctx || !buf) return -1;
    vdrive_block_ctx_t *c = (vdrive_block_ctx_t*)ctx;
    return vdrive_write(c->vdrive_id, lba, count, buf);
}

static const blockdev_ops_t g_vdrive_block_ops = {
    .get_info = vdb_get_info,
    .read = vdb_read,
    .write = vdb_write,
};

blockdev_handle_t blockdev_get_vdrive_handle(int vdrive_id) {
    if (vdrive_id < 0 || vdrive_id > 255) return BLOCKDEV_INVALID_HANDLE;
    return g_vdrive_to_handle[(uint8_t)vdrive_id];
}

void blockdev_register_vdrives(void) {
    // Clear mapping
    for (int i = 0; i < 256; i++) g_vdrive_to_handle[i] = BLOCKDEV_INVALID_HANDLE;

    int count = vdrive_get_count();
    for (int i = 0; i < count; i++) {
        vdrive_block_ctx_t *ctx = (vdrive_block_ctx_t*)kmalloc(sizeof(vdrive_block_ctx_t));
        if (!ctx) continue;
        ctx->vdrive_id = (uint8_t)i;
        blockdev_handle_t h = blockdev_register(&g_vdrive_block_ops, ctx);
        if (h != BLOCKDEV_INVALID_HANDLE) {
            g_vdrive_to_handle[(uint8_t)i] = h;
        }
    }
}
