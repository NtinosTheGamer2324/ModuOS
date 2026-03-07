#include "moduos/arch/AMD64/syscall/syscall64_stack.h"
#include "moduos/arch/AMD64/gdt.h"
#include "moduos/arch/AMD64/cpu.h"
#include "moduos/kernel/percpu.h"
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
    /* Reject non-canonical or user-range values */
    if ((rsp0 >> 48) != 0xFFFF || rsp0 < 0xFFFF800000000000ULL) return;
    g_syscall_rsp0 = rsp0;
    amd64_tss_set_rsp0(rsp0);
    
    /* Update per-CPU structure for SYSCALL entry point */
    cpu_local_t *local = cpu_local_get();
    if (local) {
        local->syscall_rsp0 = rsp0;
    }
    
    // DEBUG: Log TSS RSP0 update
    extern void com_write_string(int port, const char *str);
    extern void utoa(uint32_t val, char *buf, int base);
    com_write_string(0x3F8, "[TSS] Set RSP0=0x");
    char buf[16];
    utoa((uint32_t)(rsp0 >> 32), buf, 16);
    com_write_string(0x3F8, buf);
    utoa((uint32_t)(rsp0 & 0xFFFFFFFF), buf, 16);
    com_write_string(0x3F8, buf);
    com_write_string(0x3F8, "\n");
}

uint64_t amd64_syscall_get_kernel_stack(void) {
    return g_syscall_rsp0;
}
