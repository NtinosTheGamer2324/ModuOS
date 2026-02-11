#include "moduos/arch/AMD64/syscall/syscall64_stack.h"
#include "moduos/arch/AMD64/gdt.h"
#include "moduos/arch/AMD64/cpu.h"
#include <stdint.h>

/*
 * In SMP, the syscall stack (RSP0) is per-CPU.
 * We keep the TSS.rsp0 in sync as well (for interrupt/sysret transitions).
 */

// Until SMP cpu-local storage is fully initialized (GS base, cpu_local_get()),
// keep a single global syscall RSP0. The TSS RSP0 is still the authoritative value
// used for privilege transitions.
static uint64_t g_syscall_rsp0;

void amd64_syscall_set_kernel_stack(uint64_t rsp0) {
    g_syscall_rsp0 = rsp0;
    amd64_tss_set_rsp0(rsp0);
}

uint64_t amd64_syscall_get_kernel_stack(void) {
    return g_syscall_rsp0;
}
