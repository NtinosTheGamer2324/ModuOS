#ifndef MODUOS_FS_PART_H
#define MODUOS_FS_PART_H

#include <stdint.h>

// Request/response for SYS_VFS_GETPART

typedef struct {
    int32_t vdrive_id;
    int32_t part_no; // 1..4
} vfs_part_req_t;

typedef struct {
    uint32_t start_lba;
    uint32_t sectors;
    uint8_t type;
    uint8_t _pad[3];
} vfs_part_info_t;

#endif
