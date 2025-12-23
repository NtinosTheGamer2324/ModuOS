// ls.c - List directory contents
#include "libc.h"
#include "string.h"

void print_entry(const char* name, int is_dir, unsigned int size) {
    if (is_dir) {
        printf("[DIR]  ");
    } else {
        printf("[FILE] ");
    }
    
    printf(name);
    
    if (!is_dir) {
        printf("  (");
        char size_str[32];
        itoa(size, size_str, 10);
        printf(size_str);
        printf(" bytes)");
    }
    
    printf("\n");
}

int md_main(long argc, char** argv) {
    const char* path = "/";
    
    // Use argument if provided
    if (argc > 1) {
        path = argv[1];
    }
    
    printf("Directory listing: ");
    printf(path);
    printf("\n\n");
    
    // Open directory
    int dir_fd = opendir(path);
    if (dir_fd < 0) {
        printf("Error: Could not open directory\n");
        return 1;
    }
    
    // Read entries
    char name_buf[260];
    int is_dir;
    unsigned int size;
    int count = 0;
    
    while (1) {
        int result = readdir(dir_fd, name_buf, sizeof(name_buf), &is_dir, &size);
        
        if (result == 0) {
            // End of directory
            break;
        } else if (result < 0) {
            // Error
            printf("Error reading directory\n");
            closedir(dir_fd);
            return 1;
        }
        
        print_entry(name_buf, is_dir, size);
        count++;
    }
    
    // Close directory
    closedir(dir_fd);
    
    printf("\nTotal entries: ");
    char count_str[32];
    itoa(count, count_str, 10);
    printf(count_str);
    printf("\n");
    
    return 0;
}
