#include "libc.h"
#include "string.h"
#include "../include/moduos/kernel/syscall/syscall_numbers.h"

int md_main(long argc, char** argv) {
    (void)argc;
    (void)argv;

    fs_mount_info_t mounts[MAX_MOUNTS];
    int count = list_mounts(mounts, MAX_MOUNTS);
    if (count < 0) {
        printf("Error listing mounts: %d\n", count);
        return 1;
    }

    if (count == 0) {
        printf("No filesystems mounted\n");
        return 0;
    }

    printf("Mounted filesystems (%d):\n", count);
    for (int i = 0; i < count && i < 16; i++) {
        printf("slot=%d vdrive=%d lba=%u type=%d label=%s\n",
               mounts[i].slot,
               mounts[i].vdrive_id,
               mounts[i].partition_lba,
               mounts[i].type,
               mounts[i].label[0] ? mounts[i].label : "<none>");
    }
    return 0;
}
