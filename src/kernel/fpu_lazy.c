#include "moduos/kernel/process/process.h"
#include <stdint.h>

/* Lazy FPU switching support.
 * We keep track of which process currently owns the live FPU state.
 */
static process_t *g_fpu_owner = NULL;

static inline void set_ts(void) {
    uint64_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= (1ULL << 3); /* TS */
    __asm__ volatile("mov %0, %%cr0" :: "r"(cr0) : "memory");
}

static inline void clear_ts(void) {
    __asm__ volatile("clts" ::: "memory");
}

void fpu_lazy_on_context_switch(process_t *next) {
    /* Only use lazy FPU switching for user processes.
     * During kernel boot and in kernel threads, trapping (#NM) is dangerous because
     * many kernel routines (memcpy/printf) may use SSE, and the #NM handler itself
     * uses fxsave/fxrstor.
     */
    if (!next || !next->is_user) {
        clear_ts();
        return;
    }

    /* If next is the current owner, allow FPU instructions without trapping. */
    if (next == g_fpu_owner) {
        clear_ts();
    } else {
        set_ts();
    }
}

void fpu_lazy_on_process_exit(process_t *p) {
    if (p && p == g_fpu_owner) {
        g_fpu_owner = NULL;
        set_ts();
    }
}

void fpu_lazy_handle_nm(void) {
    process_t *cur = process_get_current();
    if (!cur || !cur->is_user) {
        /* Kernel should not be running with TS set; just clear it and continue. */
        clear_ts();
        return;
    }

    /* Enable FPU for this task. */
    clear_ts();

    if (cur == g_fpu_owner) {
        return;
    }

    /* Save old owner state (if any). */
    if (g_fpu_owner) {
        __asm__ volatile("fxsave64 %0" : "=m"(g_fpu_owner->fpu_state));
    }

    /* Restore current. */
    __asm__ volatile("fxrstor64 %0" :: "m"(cur->fpu_state));
    g_fpu_owner = cur;
}
