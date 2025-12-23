// tree.c - Recursive directory tree viewer
#include "libc.h"
#include "string.h"

#define MAX_DEPTH 10
#define MAX_PATH 256

void print_indent(int depth) {
    for (int i = 0; i < depth; i++) {
        printf("  ");
    }
}

void print_tree_entry(const char* name, int is_dir, int depth) {
    print_indent(depth);
    
    if (is_dir) {
        printf("[+] ");
    } else {
        printf("    ");
    }
    
    printf(name);
    printf("\n");
}

void list_directory_recursive(const char* path, int depth) {
    if (depth >= MAX_DEPTH) {
        print_indent(depth);
        printf("(max depth reached)\n");
        return;
    }
    
    int dir_fd = opendir(path);
    if (dir_fd < 0) {
        print_indent(depth);
        printf("(error opening directory)\n");
        return;
    }
    
    char name_buf[260];
    int is_dir;
    unsigned int size;
    
    // First pass: list files
    while (1) {
        int result = readdir(dir_fd, name_buf, sizeof(name_buf), &is_dir, &size);
        
        if (result == 0) break;
        if (result < 0) break;
        
        if (!is_dir) {
            print_tree_entry(name_buf, 0, depth);
        }
    }
    
    closedir(dir_fd);
    
    // Second pass: recurse into directories
    dir_fd = opendir(path);
    if (dir_fd < 0) return;
    
    while (1) {
        int result = readdir(dir_fd, name_buf, sizeof(name_buf), &is_dir, &size);
        
        if (result == 0) break;
        if (result < 0) break;
        
        if (is_dir) {
            print_tree_entry(name_buf, 1, depth);
            
            // Build new path
            char new_path[MAX_PATH];
            if (strcmp(path, "/") == 0) {
                new_path[0] = '/';
                strncpy(new_path + 1, name_buf, MAX_PATH - 2);
            } else {
                strncpy(new_path, path, MAX_PATH - 1);
                int len = strlen(new_path);
                if (len < MAX_PATH - 1) {
                    new_path[len] = '/';
                    strncpy(new_path + len + 1, name_buf, MAX_PATH - len - 2);
                }
            }
            new_path[MAX_PATH - 1] = '\0';
            
            // Recurse
            list_directory_recursive(new_path, depth + 1);
        }
    }
    
    closedir(dir_fd);
}

int md_main(long argc, char** argv) {
    const char* path = "/";
    
    if (argc > 1) {
        path = argv[1];
    }
    
    printf("Directory tree: ");
    printf(path);
    printf("\n\n");
    
    list_directory_recursive(path, 0);
    
    printf("\nDone.\n");
    return 0;
}
