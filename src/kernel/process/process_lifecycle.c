// process_lifecycle.c - Process creation, exit, and destruction
#include "moduos/kernel/process/process.h"
#include "moduos/kernel/memory/memory.h"
#include "moduos/kernel/memory/paging.h"
#include "moduos/kernel/memory/kheap.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/COM/com.h"
#include "moduos/kernel/debug.h"
#include "moduos/kernel/percpu.h"
#include <stdint.h>

/* External functions from other modules */
extern uint32_t process_alloc_pid(void);
extern int process_register(process_t *proc);
extern int process_unregister(uint32_t pid);
extern void scheduler_add_process(process_t *proc);
extern void scheduler_remove_process(process_t *proc);
extern uint64_t scheduler_get_min_vruntime(void);
extern uint32_t scheduler_nice_to_weight(int nice);

/* External functions from process.c */
extern void set_curproc(process_t *p);
extern process_t *get_curproc(void);

/* Forward declarations */
static void free_argv(int argc, char **argv);

/* NOTE: The actual implementation of process_create_user, process_create_kernel_thread,
 * process_exit, process_kill, and process_free will be moved here from process.c
 * This file is created as a placeholder for now to establish the structure.
 * The actual code movement will happen in the next iteration to avoid duplication errors.
 */

/* Helper: Free argv array */
static void free_argv(int argc, char **argv) {
    if (!argv) return;
    for (int i = 0; i < argc; i++) {
        if (argv[i]) kfree(argv[i]);
    }
    kfree(argv);
}

/* Placeholder - actual implementations will be moved from process.c */
