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
    printf("Usage: adduser <username> <password> <uid>\n");
}

int md_main(long argc, char **argv) {
    if (argc < 4) {
        usage();
        return 1;
    }

    const char *user = argv[1];
    const char *pass = argv[2];
    const char *uid_str = argv[3];

    if (!user || !pass || !uid_str) {
        usage();
        return 1;
    }

    char req[128];
    req[0] = 0;
    req[sizeof(req) - 1] = 0;
    copy_str(req, sizeof(req), user);
    append_str(req, sizeof(req), ":");
    append_str(req, sizeof(req), pass);
    append_str(req, sizeof(req), ":");
    append_str(req, sizeof(req), uid_str);

    int fd = open(USERMAN_DEV_ADD, 0, 0);
    if (fd < 0) {
        printf("adduser: userman not available\n");
        return 2;
    }

    write(fd, req, strlen(req));
    char resp[16];
    int r = read(fd, resp, sizeof(resp) - 1);
    close(fd);

    if (r <= 0) {
        printf("adduser: no response\n");
        return 3;
    }
    resp[r] = 0;
    if (resp[0] != '0') {
        printf("adduser: failed rc=%s\n", resp);
        return 4;
    }

    printf("adduser: ok\n");
    return 0;
}

