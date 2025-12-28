#include "libc.h"
#include "string.h"

#define MAX_DEPTH 8
#define MAX_PATH 256

// Visual markers
const char* VLINE   = "│   ";
const char* BRANCH  = "├── ";
const char* LAST    = "└── ";

typedef struct {
    int is_last[MAX_DEPTH];
} TreeState;

void list_recursive(const char* path, int depth, TreeState state) {
    if (depth >= MAX_DEPTH) return;

    int fd = opendir(path);
    if (fd < 0) {
        printf(" [Error opening %s]\n", path);
        return;
    }

    char name[256];
    int is_dir;
    unsigned int size;

    // Count number of entries to detect last
    int entries = 0;
    int temp_fd = opendir(path);
    while (readdir(temp_fd, name, sizeof(name), &is_dir, &size) > 0) {
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        entries++;
    }
    closedir(temp_fd);

    int index = 0;
    while (readdir(fd, name, sizeof(name), &is_dir, &size) > 0) {
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        index++;

        // Print tree indentation
        for (int i = 0; i < depth; i++) {
            if (state.is_last[i])
                printf("    ");
            else
                printf("%s", VLINE);
        }

        int last = (index == entries);
        state.is_last[depth] = last;

        printf("%s", last ? LAST : BRANCH);

        if (is_dir) {
            printf("%s/\n", name);

            // Build next path
            char next_path[MAX_PATH];
            strcpy(next_path, path);
            int len = strlen(next_path);
            if (len > 0 && next_path[len-1] != '/')
                strcat(next_path, "/");
            strcat(next_path, name);

            list_recursive(next_path, depth + 1, state);
        } else {
            printf("%s (%u bytes)\n", name, size);
        }
    }

    closedir(fd);
}

int md_main(long argc, char** argv) {
    const char* root = (argc > 1) ? argv[1] : "/";

    printf("Squirrel Tree View: %s\n", root);
    printf("%s\n", root);

    TreeState state = {0};
    list_recursive(root, 0, state);

    printf("\nDone.\n");
    return 0;
}
