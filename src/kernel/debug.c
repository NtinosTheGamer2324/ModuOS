#include "moduos/kernel/debug.h"

/* Default debug level (can be changed later or made boot-arg controlled) */
static volatile kernel_debug_level_t g_kernel_debug_level = KDBG_MED;

void kernel_debug_set_level(kernel_debug_level_t lvl) {
    if (lvl < KDBG_OFF) lvl = KDBG_OFF;
    if (lvl > KDBG_ON)  lvl = KDBG_ON;
    g_kernel_debug_level = lvl;
}

kernel_debug_level_t kernel_debug_get_level(void) {
    return g_kernel_debug_level;
}
