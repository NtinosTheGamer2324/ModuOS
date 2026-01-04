// rm.c - remove files (and directories with -r)
#include "libc.h"
#include "string.h"

static void usage(void) {
    printf("Usage: rm [-r] <path>\n");
}

static int join_path(const char *base, const char *name, char *out, size_t out_sz) {
    if (!base || !name || !out || out_sz == 0) return -1;
    out[0] = 0;

    size_t bl = strlen(base);
    if (bl + 1 >= out_sz) return -1;
    strncpy(out, base, out_sz - 1);
    out[out_sz - 1] = 0;

    if (bl > 0 && out[bl - 1] != '/') {
        strncat(out, "/", out_sz - strlen(out) - 1);
    }
    strncat(out, name, out_sz - strlen(out) - 1);
    return 0;
}

static int rm_recursive(const char *path) {
    fs_file_info_t st;
    if (stat(path, &st) != 0) {
        return -1;
    }

    if (!st.is_directory) {
        return unlink(path);
    }

    // Directory: iterate entries
    int d = opendir(path);
    if (d < 0) return -2;

    char name[256];
    int is_dir = 0;
    uint32_t sz = 0;

    while (1) {
        int rc = readdir(d, name, sizeof(name), &is_dir, &sz);
        if (rc <= 0) break;

        // Skip . and ..
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;

        char child[512];
        if (join_path(path, name, child, sizeof(child)) != 0) {
            closedir(d);
            return -3;
        }

        int rrc = rm_recursive(child);
        if (rrc != 0) {
            closedir(d);
            return rrc;
        }
    }

    closedir(d);
    return rmdir(path);
}

int md_main(long argc, char **argv) {
    int rflag = 0;
    const char *path = NULL;

    for (long i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "-R") == 0) {
            rflag = 1;
        } else if (!path) {
            path = argv[i];
        }
    }

    if (!path) {
        usage();
        return 1;
    }

    int rc;
    if (rflag) {
        rc = rm_recursive(path);
    } else {
        // Without -r we remove files only
        fs_file_info_t st;
        if (stat(path, &st) == 0 && st.is_directory) {
            printf("rm: '%s' is a directory (use -r)\n", path);
            return 1;
        }
        rc = unlink(path);
    }

    if (rc != 0) {
        printf("rm: failed rc=%d path='%s'\n", rc, path);
        return 1;
    }

    return 0;
}
