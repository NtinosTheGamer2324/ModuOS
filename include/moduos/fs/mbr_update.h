#ifndef MODUOS_FS_MBR_UPDATE_H
#define MODUOS_FS_MBR_UPDATE_H

#include <stdint.h>

// Internal helper for filesystem formatting: update MBR partition type for entry
// whose start LBA matches start_lba.
int fs_mbr_set_type_for_lba(int vdrive_id, uint32_t start_lba, uint8_t new_type);

#endif
