#ifndef MODUOS_KERNEL_DEBUG_H
#define MODUOS_KERNEL_DEBUG_H

#include <stdint.h>

/* Runtime-togglable kernel debug level.
 * 0 = off (default): no runtime spam (except boot-time logs)
 * 1 = med: minimal useful debug
 * 2 = on: very verbose (scheduler/syscalls/yield tracing)
 */
typedef enum {
    KDBG_OFF = 0,
    KDBG_MED = 1,
    KDBG_ON  = 2,
} kernel_debug_level_t;

void kernel_debug_set_level(kernel_debug_level_t lvl);
kernel_debug_level_t kernel_debug_get_level(void);

/* Compatibility: treat "enabled" as on/off */
static inline void kernel_debug_set(int enabled) {
    kernel_debug_set_level(enabled ? KDBG_ON : KDBG_OFF);
}
static inline int kernel_debug_get(void) {
    return kernel_debug_get_level() != KDBG_OFF;
}

static inline int kernel_debug_is_med(void) { return kernel_debug_get_level() >= KDBG_MED; }
static inline int kernel_debug_is_on(void)  { return kernel_debug_get_level() >= KDBG_ON; }

#endif /* MODUOS_KERNEL_DEBUG_H */
