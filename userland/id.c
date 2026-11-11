#include "libc/libc.h"
#include "libc/string.h"
#include "userctl_ipc.h"

static void print_uid(uctl_shm_t *shm, const char *label, int uid) {
    char name[UCTL_NAME_MAX];
    printf("%s=%d", label, uid);
    if (shm && uctl_name_of_uid(shm, (uint64_t)uid, name, sizeof(name)) == 0)
        printf("(%s)", name);
}

int md_main(long argc, char **argv) {
    (void)argc; (void)argv;
    uctl_shm_t *shm = uctl_attach();
    int uid = getuid();
    int euid = geteuid();

    print_uid(shm, "uid", uid);
    printf(" gid=%d", getgid());
    if (euid != uid) {
        printf(" ");
        print_uid(shm, "euid", euid);
        printf(" egid=%d", getegid());
    }
    printf("\n");
    return 0;
}
