#include "moduos/arch/AMD64/syscall/syscall64_stack.h"
#include "moduos/arch/AMD64/gdt.h"
#include <stdint.h>

uint64_t g_syscall_rsp0 = 0;

/* exported for asm via RIP-relative */

void amd64_syscall_set_kernel_stack(uint64_t rsp0) {
    g_syscall_rsp0 = rsp0;
    amd64_tss_set_rsp0(rsp0);
}

uint64_t amd64_syscall_get_kernel_stack(void) {
    return g_syscall_rsp0;
}
