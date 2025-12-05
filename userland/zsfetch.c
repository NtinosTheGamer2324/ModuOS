#include "libc.h"

static inline void cpuid(uint32_t code, uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d) {
    __asm__ volatile("cpuid"
                     : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                     : "a"(code));
}

static void get_cpu_vendor(char* buf) {
    uint32_t eax, ebx, ecx, edx;
    cpuid(0, &eax, &edx, &ecx, &ebx);

    *(uint32_t*)&buf[0] = ebx;
    *(uint32_t*)&buf[4] = edx;
    *(uint32_t*)&buf[8] = ecx;
    buf[12] = '\0';
}

void _start(void) {
    char cpu_vendor[13];
    get_cpu_vendor(cpu_vendor);

    printf("\n");
    printf("\\cg----------------------------\\rr\n");
    printf("\\cyOS:\\rr             ModuOS v0.3.23\n");
    printf("\\cyShell:\\rr          Zenith v0.4.1\n");
    printf("\\cyKernel Arch:\\rr    AMD64 (x86_64)\n");
    printf("\\cyCPU:\\rr            %s\n", cpu_vendor);
    printf("\\cyUptime:\\rr         N/A\n");
    printf("\\cyTerminal:\\rr       VGA Console\n");
    printf("\\cyResolution:\\rr     80x25\n");
    printf("\\cg----------------------------\\rr\n");
    printf("\n");
    printf("  \\br    \\rr  \\bg    \\rr  \\bb    \\rr  \\cy    \\rr  \\cp    \\rr  \\cw    \\rr\n");
    printf("  \\br    \\rr  \\bg    \\rr  \\bb    \\rr  \\cy    \\rr  \\cp    \\rr  \\cw    \\rr\n");
    printf("\n");

    exit(0);
}