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

// Request for SYS_VFS_MBRINIT: write a minimal MBR with a single primary partition.
// sectors==0 means "use disk size - start_lba".
// flags bit0: force overwrite even if a valid MBR signature exists.
typedef struct {
    int32_t vdrive_id;
    uint32_t start_lba;   // typically 2048
    uint32_t sectors;     // 0=auto
    uint8_t type;         // MBR partition type (0 => default 0x83)
    uint8_t bootable;     // 0/1
    uint16_t flags;       // bit0=force
} vfs_mbrinit_req_t;

#endif
