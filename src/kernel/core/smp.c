#include "moduos/kernel/smp.h"
#include "moduos/kernel/percpu.h"
#include "moduos/arch/AMD64/cpu.h"
#include "moduos/kernel/memory/string.h"

#define SMP_MAX_CPUS 256

static cpu_local_t g_bsp_cpu;
static cpu_state_t g_cpu_states[SMP_MAX_CPUS];
static cpu_local_t *g_cpus[SMP_MAX_CPUS];
static uint32_t g_cpu_count = 1;

/*
 * Early BSP initialization.
 * Sets up GS base and registers the BSP's per-CPU structure.
 */
void smp_init_bsp_early(void) {
    /* Initialize BSP's per-CPU structure */
    memset(&g_bsp_cpu, 0, sizeof(g_bsp_cpu));

    g_bsp_cpu.self = (uint64_t)(uintptr_t)&g_bsp_cpu;
    g_bsp_cpu.cpu_num = 0;
    g_bsp_cpu.apic_id = 0;
    g_bsp_cpu.syscall_rsp0 = 0;
    g_bsp_cpu.current_process = 0;
    g_bsp_cpu.resched = 0;
    g_bsp_cpu.user_rsp = 0;
    g_bsp_cpu.user_rip = 0;    /* Will be set per-syscall */
    g_bsp_cpu.user_rflags = 0; /* Will be set per-syscall */

    /*
     * Set GS base to point at BSP's per-CPU structure.
     * Initially GS_BASE and KERNEL_GS_BASE are set to the same value.
     * This keeps swapgs harmless until user TLS/GS usage is introduced.
     */
    amd64_cpu_set_gs_base((uint64_t)(uintptr_t)&g_bsp_cpu);
    amd64_cpu_set_kernel_gs_base((uint64_t)(uintptr_t)&g_bsp_cpu);

    /* Register BSP in the CPU array */
    g_cpus[0] = &g_bsp_cpu;
    g_cpu_states[0] = CPU_STATE_RUNNING;

    /* Clear state for remaining CPUs */
    for (uint32_t i = 1; i < SMP_MAX_CPUS; i++) {
        g_cpus[i] = NULL;
        g_cpu_states[i] = CPU_STATE_OFFLINE;
    }
}

/*
 * Returns the number of online CPUs.
 * Currently always 1 (BSP only) until APIC AP bring-up is implemented.
 */
uint32_t smp_cpu_count(void) {
    return g_cpu_count;
}

/*
 * Get the current CPU's per-CPU structure via GS base.
 */
cpu_local_t *smp_current_cpu(void) {
    return cpu_local_get();
}

/*
 * Get per-CPU structure for a specific CPU ID.
 * Returns NULL if invalid or offline.
 */
cpu_local_t *smp_get_cpu(uint32_t cpu_id) {
    if (cpu_id >= SMP_MAX_CPUS)
        return NULL;
    return g_cpus[cpu_id];
}

/*
 * Register a per-CPU data block during early initialization.
 * Called by AP bring-up code once APIC is available.
 */
void smp_register_percpu(uint32_t cpu_id, cpu_local_t *pcpu) {
    if (cpu_id >= SMP_MAX_CPUS || pcpu == NULL)
        return;

    if (cpu_id >= g_cpu_count)
        g_cpu_count = cpu_id + 1;

    g_cpus[cpu_id] = pcpu;

    /* Set the per-CPU structure's self-pointer and CPU number */
    pcpu->self = (uint64_t)(uintptr_t)pcpu;
    pcpu->cpu_num = cpu_id;
}

/*
 * Query the state of a CPU.
 */
cpu_state_t smp_get_cpu_state(uint32_t cpu_id) {
    if (cpu_id >= SMP_MAX_CPUS)
        return CPU_STATE_OFFLINE;
    return g_cpu_states[cpu_id];
}

/*
 * Set the state of a CPU.
 * Used by AP bring-up code once APIC is initialized.
 */
void smp_set_cpu_state(uint32_t cpu_id, cpu_state_t state) {
    if (cpu_id >= SMP_MAX_CPUS)
        return;
    g_cpu_states[cpu_id] = state;
}
