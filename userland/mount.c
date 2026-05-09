#include "libc.h"
#include "string.h"
#include "../include/moduos/kernel/syscall/syscall_numbers.h"

int md_main(long argc, char** argv) {
    if (argc != 4) {
        printf("Usage: mount <vdrive> <lba> <type>\n");
        printf("  type: 0=UNKNOWN 1=FAT32 2=ISO9660 3=EXTERNAL 4=MDFS\n");
        return 1;
    }

    int vdrive = atoi(argv[1]);
    uint32_t lba = (uint32_t)atoi(argv[2]);
    int type = atoi(argv[3]);

    int slot = mount_drive(vdrive, lba, type);
    if (slot < 0) {
        printf("Mount failed: %d\n", slot);
        return 1;
    }

    printf("Mounted slot %d\n", slot);
    return 0;
}
