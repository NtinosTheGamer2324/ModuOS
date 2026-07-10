// dir.c - MS-DOS style manual formatting
#include "libc.h"
#include "string.h"

// Helper to print padding manually
static void print_padding(int count) {
    for (int i = 0; i < count; i++) printf(" ");
}

int md_main(long argc, char** argv) {
    const char *path = ".";
    int wide_format = 0;

    for (long i = 1; i < argc; i++) {
        if (argv[i][0] == '/' && (argv[i][1] == 'W' || argv[i][1] == 'w')) 
            wide_format = 1;
        else 
            path = argv[i];
    }

    int dir_fd = opendir(path);
    if (dir_fd < 0) {
        printf("File not found\n");
        return 1;
    }

    printf(" Volume in drive C has no label.\n");
    printf(" Directory of %s\n\n", path);

    char name_buf[260];
    int is_dir = 0;
    unsigned int size = 0;
    int file_count = 0;
    unsigned long total_bytes = 0;

    while (1) {
        int rc = readdir(dir_fd, name_buf, sizeof(name_buf), &is_dir, &size);
        if (rc == 0) break;
        if (rc < 0) break;

        // Manual field printing
        int len = strlen(name_buf);
        printf("%s", name_buf);
        print_padding(14 - len); // Ensure 14 chars width

        if (is_dir) {
            printf("<DIR>        ");
        } else {
            // Simple integer to string conversion logic if necessary, 
            // but printf usually supports %u even if widths fail.
            printf("      %u", size);
            total_bytes += size;
            file_count++;
        }

        if (wide_format) {
            if (++file_count % 3 == 0) printf("\n");
            else printf("  ");
        } else {
            printf("\n");
        }
    }

    printf("\n             %d File(s)    %lu bytes\n", file_count, total_bytes);
    closedir(dir_fd);
    return 0;
}