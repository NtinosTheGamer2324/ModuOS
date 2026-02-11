#include "moduos/kernel/smp.h"
#include "moduos/kernel/percpu.h"
#include "moduos/arch/AMD64/cpu.h"

static cpu_local_t g_bsp_cpu;
static uint32_t g_cpu_count = 1;

void smp_init_bsp_early(void) {
    /* Minimal safe defaults; APIC ID filled later once LAPIC is up. */
    g_bsp_cpu.self = (uint64_t)(uintptr_t)&g_bsp_cpu;
    g_bsp_cpu.cpu_num = 0;
    g_bsp_cpu.apic_id = 0;
    g_bsp_cpu.syscall_rsp0 = 0;
    g_bsp_cpu.current_process = 0;
    g_bsp_cpu.resched = 0;

    /*
     * For now, set both GS_BASE and KERNEL_GS_BASE to the same value.
     * This keeps swapgs harmless until we introduce user TLS/GS usage.
     */
    amd64_cpu_set_gs_base((uint64_t)(uintptr_t)&g_bsp_cpu);
    amd64_cpu_set_kernel_gs_base((uint64_t)(uintptr_t)&g_bsp_cpu);
}

uint32_t smp_cpu_count(void) {
    return g_cpu_count;
}
