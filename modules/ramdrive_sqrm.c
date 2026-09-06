#include "sqrm_sdk.h"

/*
 * ramdrive_sqrm.c -- minimal SQRM_TYPE_DRIVE example module.
 *
 * Demonstrates the DRIVE-module contract end to end: exposes a
 * kmalloc-backed RAM disk through blockdev_ops_t, calls block_register(),
 * and the kernel attaches the result as a new vdrive automatically. No
 * hardware is involved, so this is a template for the block_register
 * side of a real controller driver (AHCI, NVMe, etc.), not a substitute
 * for one -- swap RAMDRIVE_* for real port/MMIO/DMA access against your
 * hardware and the rest of this file stays the same.
 *
 */

SQRM_DEFINE_MODULE(SQRM_TYPE_DRIVE, "ramdrive");

#define RAMDRIVE_SECTOR_SIZE   512u
#define RAMDRIVE_SECTOR_COUNT  32768u   /* 16 MiB */
#define RAMDRIVE_BYTES         ((size_t)RAMDRIVE_SECTOR_SIZE * RAMDRIVE_SECTOR_COUNT)

static const uint16_t COM1_PORT = 0x3F8;
static const sqrm_kernel_api_t *g_api;
static uint8_t *g_backing_store;

typedef struct {
    int dummy; /* ctx is unused here since there's only ever one ramdrive;
                  a real controller driver would put its port/channel index
                  or an hba_port_t* here instead. */
} ramdrive_ctx_t;

static ramdrive_ctx_t g_ctx;

static int ramdrive_get_info(void *ctx, blockdev_info_t *out) {
    (void)ctx;
    if (!out) return -1;

    out->sector_size = RAMDRIVE_SECTOR_SIZE;
    out->sector_count = RAMDRIVE_SECTOR_COUNT;
    out->flags = 0;

    const char *model = "SQRM RAMDRIVE";
    int i = 0;
    for (; model[i] && i < (int)sizeof(out->model) - 1; i++) out->model[i] = model[i];
    out->model[i] = 0;

    return 0;
}

static int ramdrive_read(void *ctx, uint64_t lba, uint32_t count, void *buf, size_t buf_sz) {
    (void)ctx;
    if (!buf) return -1;
    if (lba + count > RAMDRIVE_SECTOR_COUNT) return -2;

    size_t off = (size_t)lba * RAMDRIVE_SECTOR_SIZE;
    size_t len = (size_t)count * RAMDRIVE_SECTOR_SIZE;
    if (len > buf_sz) return -3;

    uint8_t *dst = (uint8_t *)buf;
    for (size_t i = 0; i < len; i++) dst[i] = g_backing_store[off + i];
    return 0;
}

static int ramdrive_write(void *ctx, uint64_t lba, uint32_t count, const void *buf, size_t buf_sz) {
    (void)ctx;
    if (!buf) return -1;
    if (lba + count > RAMDRIVE_SECTOR_COUNT) return -2;

    size_t off = (size_t)lba * RAMDRIVE_SECTOR_SIZE;
    size_t len = (size_t)count * RAMDRIVE_SECTOR_SIZE;
    if (len > buf_sz) return -3;

    const uint8_t *src = (const uint8_t *)buf;
    for (size_t i = 0; i < len; i++) g_backing_store[off + i] = src[i];
    return 0;
}

static const blockdev_ops_t g_ramdrive_ops = {
    .get_info = ramdrive_get_info,
    .read = ramdrive_read,
    .write = ramdrive_write,
};

int sqrm_module_init(const sqrm_kernel_api_t *api) {
    if (!api || api->abi_version != SQRM_ABI_VERSION) return -1;
    g_api = api;

    if (!api->kmalloc || !api->block_register) {
        if (api->com_write_string) {
            api->com_write_string(COM1_PORT, "[ramdrive] kernel API missing required entries\n");
        }
        return -2;
    }

    g_backing_store = (uint8_t *)api->kmalloc(RAMDRIVE_BYTES);
    if (!g_backing_store) {
        api->com_write_string(COM1_PORT, "[ramdrive] failed to allocate backing store\n");
        return -3;
    }
    for (size_t i = 0; i < RAMDRIVE_BYTES; i++) g_backing_store[i] = 0;

    blockdev_handle_t handle = BLOCKDEV_INVALID_HANDLE;
    int rc = api->block_register(&g_ramdrive_ops, &g_ctx, &handle);
    if (rc != 0) {
        api->com_write_string(COM1_PORT, "[ramdrive] block_register failed\n");
        return -4;
    }

    api->com_write_string(COM1_PORT, "[ramdrive] 16MiB RAM drive attached as vdrive\n");
    return 0;
}