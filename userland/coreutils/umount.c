#include "libc.h"
#include "string.h"

int md_main(long argc, char** argv) {
    if (argc != 2) {
        printf("Usage: unmount <slot>\n");
        return 1;
    }

    int slot = atoi(argv[1]);
    int rc = unmount_slot(slot);
    if (rc != 0) {
        printf("Unmount failed: %d\n", rc);
        return 1;
    }

    printf("Unmounted slot %d\n", slot);
    return 0;
}
