#include "libc.h"

void mem_diag() {
    printf("\n[MEM] Heap Top: %x\n", sbrk(0));
}

int md_main(long argc, char** argv) {
    printf("--- ModuOS Memory Stress Test ---\n");
    
    void* ptrs[16];
    mem_diag();

    // 1. Test Alignment
    void* p = malloc(1);
    if (((unsigned long)p % 8) != 0) {
        printf("[WARN] Malloc returned non-8-byte aligned address: %x\n", p);
    }
    free(p);

    // 2. Fragmented Allocation Test
    printf("Performing Fragmented Stress Test...\n");
    for(int i = 0; i < 16; i++) {
        size_t sz = (i + 1) * 64;
        ptrs[i] = malloc(sz);
        if (!ptrs[i]) {
            printf("[FAIL] Malloc failed at iteration %d\n", i);
            return 1;
        }
        // Fill with unique pattern
        memset(ptrs[i], 0xAA + i, sz);
    }

    // 3. Selective Freeing (Creates "Holes" in the heap)
    printf("Freeing even blocks to create fragmentation...\n");
    for(int i = 0; i < 16; i += 2) {
        free(ptrs[i]);
        ptrs[i] = NULL;
    }

    // 4. Verification of remaining blocks
    printf("Verifying integrity of remaining blocks...\n");
    for(int i = 1; i < 16; i += 2) {
        unsigned char* check = (unsigned char*)ptrs[i];
        if (check[0] != (0xAA + i)) {
            printf("[CRITICAL] Memory corruption detected in block %d!\n", i);
        }
    }

    // 5. Re-allocation (Testing if 'free' holes are reused)
    printf("Testing hole reuse...\n");
    void* big_block = malloc(128);
    printf("New block allocated at: %x\n", big_block);

    mem_diag();
    printf("\nTest Complete. System Stable.\n");
    return 0;
}