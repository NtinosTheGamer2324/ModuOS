#pragma once
#include <stdint.h>

/* Early SMP/CPU-local init for the BSP.
 * Sets up GS base to point at cpu_local_t.
 */
void smp_init_bsp_early(void);

/* Returns number of CPUs discovered/online (for now BSP only until AP bring-up is wired). */
uint32_t smp_cpu_count(void);

