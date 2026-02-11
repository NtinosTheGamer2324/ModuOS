#pragma once
#include <stdint.h>
#include "moduos/kernel/percpu.h"

/* MSR numbers for GS base handling */
#define MSR_IA32_GS_BASE        0xC0000101u
#define MSR_IA32_KERNEL_GS_BASE 0xC0000102u

/* Set up GS base for the current CPU (kernel mode). */
void amd64_cpu_set_gs_base(uint64_t base);
void amd64_cpu_set_kernel_gs_base(uint64_t base);

/* Read GS base (for debug) */
uint64_t amd64_cpu_get_gs_base(void);

/* Current CPU local pointer (via GS:[0]) */
cpu_local_t *cpu_local_get(void);

