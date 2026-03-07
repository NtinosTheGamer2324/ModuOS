#pragma once
#include <stdint.h>
#include "moduos/kernel/percpu.h"

#define SMP_MAX_CPUS 256

/* CPU states for AP bring-up (for future APIC integration) */
typedef enum {
    CPU_STATE_OFFLINE = 0,      /* Not yet online */
    CPU_STATE_INIT,             /* Initialization in progress */
    CPU_STATE_RUNNING,          /* Actively running */
    CPU_STATE_HALTED,           /* Halted but available */
} cpu_state_t;

/*
 * SMP initialization phase 1 (BSP early).
 * Sets up GS base to point at cpu_local_t for the BSP.
 * Safe to call before any APs are brought up.
 */
void smp_init_bsp_early(void);

/*
 * Returns the number of CPUs discovered (online count).
 * For now, returns 1 (BSP only) until APIC AP bring-up is implemented.
 */
uint32_t smp_cpu_count(void);

/*
 * Get current CPU local structure.
 * Uses GS base (already set up for each CPU).
 */
cpu_local_t *smp_current_cpu(void);

/*
 * Get CPU local structure for a specific CPU ID (0..n-1).
 * Returns NULL if CPU ID is invalid or offline.
 */
cpu_local_t *smp_get_cpu(uint32_t cpu_id);

/*
 * Register a per-CPU data block during early initialization.
 * Not called directly by drivers; used internally by SMP infrastructure.
 */
void smp_register_percpu(uint32_t cpu_id, cpu_local_t *pcpu);

/*
 * Query CPU state (offline, init, running, halted).
 */
cpu_state_t smp_get_cpu_state(uint32_t cpu_id);

/*
 * Set CPU state. Used by AP bring-up code once APIC is initialized.
 */
void smp_set_cpu_state(uint32_t cpu_id, cpu_state_t state);

