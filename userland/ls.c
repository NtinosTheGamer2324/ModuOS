// ls.c - Unix-like directory listing
#include "libc.h"
#include "string.h"

static int is_dot_entry(const char *name) {
    return (strcmp(name, ".") == 0) || (strcmp(name, "..") == 0);
}

static void print_name(const char *name, int is_dir) {
    // Safe printing: never use printf(name)
    printf("%s", name);
    if (is_dir) printf("/");
}

int md_main(long argc, char** argv) {
    // Default to process CWD (Unix semantics)
    const char *path = NULL;
    int show_all = 0; // -a

    // args: ls [-a] [path]
    for (long i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!a) continue;
        if (strcmp(a, "-a") == 0) {
            show_all = 1;
        } else {
            path = a;
        }
    }

    char cwd_buf[256];
    if (!path) {
        char *cwd = getcwd(cwd_buf, sizeof(cwd_buf));
        path = (cwd && cwd[0]) ? cwd : ".";
    }

    int dir_fd = opendir(path);
    if (dir_fd < 0) {
        printf("ls: cannot open '%s'\n", path);
        return 1;
    }

    char name_buf[260];
    int is_dir = 0;
    unsigned int size = 0;

    int first = 1;
    while (1) {
        int rc = readdir(dir_fd, name_buf, sizeof(name_buf), &is_dir, &size);
        if (rc == 0) break;
        if (rc < 0) {
            printf("ls: error reading '%s'\n", path);
            closedir(dir_fd);
            return 1;
        }

        if (!show_all && is_dot_entry(name_buf)) {
            continue;
        }

        if (!first) printf("  ");
        first = 0;
        print_name(name_buf, is_dir);
    }

    printf("\n");
    closedir(dir_fd);
    return 0;
}
