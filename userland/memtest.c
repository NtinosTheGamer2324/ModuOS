#include "libc.h"

void print_hex(unsigned int n) {
    printf("0x");
    print_uint(n, 16, 0);
}

int md_main(long argc, char** argv) {
    printf("=== Memory Diagnostics ===\n\n");

    // Show initial program break
    void* initial_brk = sbrk(0);
    printf("Initial program break : ");
    print_hex((unsigned int)initial_brk);
    printf("\n\n");

    printf("Allocating memory blocks...\n");

    void* blocks[5];
    size_t sizes[5] = { 64, 128, 256, 512, 1024 };

    for (int i = 0; i < 5; i++) {
        blocks[i] = malloc(sizes[i]);
        printf("  Allocated %d bytes at ", (int)sizes[i]);
        print_hex((unsigned int)blocks[i]);
        printf("\n");
    }

    printf("\nCurrent program break : ");
    print_hex((unsigned int)sbrk(0));
    printf("\n\n");

    printf("Writing test pattern into allocated blocks...\n");

    for (int i = 0; i < 5; i++) {
        unsigned char* ptr = (unsigned char*)blocks[i];
        for (size_t j = 0; j < sizes[i]; j++) {
            ptr[j] = (unsigned char)(i + 1);  // pattern
        }
        printf("  Block %d OK\n", i);
    }

    printf("\nFreeing memory blocks...\n");

    for (int i = 0; i < 5; i++) {
        free(blocks[i]);
        printf("  Freed block %d\n", i);
    }

    printf("\nProgram break after free: ");
    print_hex((unsigned int)sbrk(0));
    printf("\n\n");

    printf("Memory diagnostics complete.\n");

    return 0;
}
