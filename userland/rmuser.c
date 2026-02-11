#include "libc.h"
#include "userman.h"
#include "string.h"

static void copy_str(char *dst, size_t dst_sz, const char *src) {
    if (!dst || !src || dst_sz == 0) return;
    size_t i = 0;
    for (; i + 1 < dst_sz && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}



static void append_str(char *dst, size_t dst_sz, const char *src) {
    if (!dst || !src || dst_sz == 0) return;
    size_t len = strlen(dst);
    if (len >= dst_sz - 1) return;
    size_t copy = strlen(src);
    if (copy > dst_sz - 1 - len) copy = dst_sz - 1 - len;
    memcpy(dst + len, src, copy);
    dst[len + copy] = 0;
}

static void usage(void) {
    printf("Usage: rmuser <username>\n");
}

int md_main(long argc, char **argv) {
    if (argc < 2) {
        usage();
        return 1;
    }

    const char *user = argv[1];
    if (!user) {
        usage();
        return 1;
    }

    int fd = open(USERMAN_DEV_RM, 0, 0);
    if (fd < 0) {
        printf("rmuser: userman not available\n");
        return 2;
    }

    write(fd, user, strlen(user));
    char resp[16];
    int r = read(fd, resp, sizeof(resp) - 1);
    close(fd);

    if (r <= 0) {
        printf("rmuser: no response\n");
        return 3;
    }
    resp[r] = 0;
    if (resp[0] != '0') {
        printf("rmuser: failed rc=%s\n", resp);
        return 4;
    }

    printf("rmuser: ok\n");
    return 0;
}

