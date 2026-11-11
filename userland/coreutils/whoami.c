#include "../libc/libc.h"
#include "../userctl_ipc.h"

int md_main(long argc, char **argv) {
    (void)argc; (void)argv;
    int euid = geteuid();
    char name[UCTL_NAME_MAX];

    uctl_shm_t *shm = uctl_attach();
    if (shm && uctl_name_of_uid(shm, (uint64_t)euid, name, sizeof(name)) == 0)
        printf("%s\n", name);
    else
        printf("%d\n", euid);
    return 0;
}
