// mkdir.c - create directories (supports -p)
#include "libc.h"
#include "string.h"

static void usage(void) {
    printf("Usage: mkdir [-p] <path>\n");
}

static int is_sep(char c) {
    return c == '/';
}

static int do_mkdir_p(const char *path) {
    if (!path || !*path) return -1;

    // Work on a mutable copy.
    char tmp[256];
    memset(tmp, 0, sizeof(tmp));
    strncpy(tmp, path, sizeof(tmp) - 1);

    // For $/ paths, we still create by progressively adding components.
    // We do NOT try to create at "$" or "$\/mnt" levels; kernel will reject DEVFS.

    size_t n = strlen(tmp);
    if (n == 0) return -1;

    // Strip trailing slashes (except keep root forms intact).
    while (n > 1 && tmp[n - 1] == '/') {
        tmp[n - 1] = 0;
        n--;
    }

    // Iterate components: create each prefix.
    // Skip initial "$" or "$/" or leading '/'.
    for (size_t i = 0; i < n; i++) {
        if (i == 0) continue;
        if (!is_sep(tmp[i])) continue;

        // Temporarily terminate string at this slash.
        char saved = tmp[i];
        tmp[i] = 0;

        // Avoid trying to mkdir on "$" or "$\/mnt" or "$\/dev" pseudo roots.
        if (strcmp(tmp, "$") != 0 && strcmp(tmp, "$/") != 0 && strcmp(tmp, "$/mnt") != 0 && strcmp(tmp, "$/dev") != 0) {
            int rc = mkdir(tmp);
            // Ignore errors: the directory may already exist.
            (void)rc;
        }

        tmp[i] = saved;
    }

    // Finally create full path
    return mkdir(tmp);
}

int md_main(long argc, char **argv) {
    int pflag = 0;
    const char *path = NULL;

    for (long i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0) {
            pflag = 1;
        } else if (!path) {
            path = argv[i];
        }
    }

    if (!path) {
        usage();
        return 1;
    }

    int rc = pflag ? do_mkdir_p(path) : mkdir(path);
    if (rc != 0) {
        printf("mkdir: failed rc=%d path='%s'\n", rc, path);
        return 1;
    }

    return 0;
}
