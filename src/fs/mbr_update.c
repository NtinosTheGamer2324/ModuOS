// mbr_update.c - internal helper for updating MBR partition type after mkfs
#include "moduos/drivers/Drive/vDrive.h"
#include "moduos/kernel/memory/memory.h"
#include "moduos/kernel/memory/string.h"

#define MBR_PARTITION_TABLE_OFFSET 0x1BEu
#define MBR_PARTITION_ENTRY_SIZE   16u

static uint32_t read_le32_u(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

#include "moduos/fs/mbr_update.h"

int fs_mbr_set_type_for_lba(int vdrive_id, uint32_t start_lba, uint8_t new_type) {
    if (start_lba == 0 || new_type == 0) return -1;

    vdrive_t *d = vdrive_get((uint8_t)vdrive_id);
    if (!d || !d->present) return -2;
    if (d->read_only) return -3;
    if (d->sector_size != 512) return -4;

    uint8_t *mbr = (uint8_t*)kmalloc(512);
    if (!mbr) return -5;

    if (vdrive_read_sector((uint8_t)vdrive_id, 0, mbr) != VDRIVE_SUCCESS) { kfree(mbr); return -6; }
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) { kfree(mbr); return -7; }

    int found = 0;
    for (int i = 0; i < 4; i++) {
        uint32_t ent_off = MBR_PARTITION_TABLE_OFFSET + (uint32_t)i * MBR_PARTITION_ENTRY_SIZE;
        uint8_t type = mbr[ent_off + 4];
        uint32_t first_lba = read_le32_u(mbr + ent_off + 8);
        if (type == 0x00) continue;
        if (first_lba == start_lba) {
            mbr[ent_off + 4] = new_type;
            found = 1;
            break;
        }
    }

    if (!found) { kfree(mbr); return -8; }

    if (vdrive_write_sector((uint8_t)vdrive_id, 0, mbr) != VDRIVE_SUCCESS) { kfree(mbr); return -9; }

    kfree(mbr);
    return 0;
}
