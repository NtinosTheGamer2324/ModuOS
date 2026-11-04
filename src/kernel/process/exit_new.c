// exit_new.c - Process exit/termination

#include "moduos/kernel/process/process_new.h"
#include "moduos/kernel/spinlock.h"
#include "moduos/kernel/COM/com.h"
#include "moduos/kernel/memory/paging.h"
#include "moduos/fs/fd.h"
#include "moduos/fs/userfs.h"

extern char *itoa(int value, char *str, int base);
extern void fpu_lazy_on_process_exit(process_t *p);

// Coarse lock protecting all parent-child list manipulations.
// Both do_exit() and do_waitpid() must hold this before touching
// any sibling_next/sibling_prev/children/parent pointer.
// Declared non-static so wait_new.c can reference it via extern.
// Initialised once from process_management_init() — never lazily.
spinlock_t children_lock;

// Called once from process_management_init() before any process is created.
void exit_subsystem_init(void) {
    spinlock_init(&children_lock);
}

/* ---------------------------------------------------------------------------
 * process_teardown_resources
 *
 * Releases everything a dying process owns except the process_t struct
 * itself, which is kept around as a zombie until the parent reaps it with
 * waitpid(). This is the single place all termination paths funnel
 * through: do_exit() (exit() syscall, signal delivery) calls this
 * directly, and process_exit() (fault-induced kills) calls do_exit(). No
 * route out of a process skips it, so nothing leaves its files, address
 * space, or page tables behind.
 *
 * Precondition: called with 'current' == p. All callers (do_exit() via
 * sys_exit()/check_signals(), and process_exit() via the fault handlers)
 * run on the dying process's own kernel stack while its own CR3 is still
 * loaded, so freeing user memory here needs no CR3 switch.
 * --------------------------------------------------------------------- */
static void process_teardown_resources(process_t *p) {
    /* Drop FPU ownership before anything else can observe stale fpu_state
     * (fxsave/fxrstor target fpu_state, which stays live in the struct
     * until process_destroy() frees it at reap time). */
    fpu_lazy_on_process_exit(p);

    /* Real open files: flushes write buffers, releases pipe endpoints,
     * drops VFS/devfs/userfs cache references. */
    fd_close_all((int)p->pid);
    userfs_pid_exited(p->pid);

    /* Opaque fds injected via process_inject_fd() (tty/pipe handles). The
     * referenced object's own lifetime belongs to whichever subsystem
     * injected it; here we only drop this process's slot. */
    for (int i = 0; i < PROCESS_MAX_FDS; i++) {
        p->fd_table[i] = NULL;
    }

    /* User address space. Leaf pages (heap/stack/mmap/image) first, while
     * still on p's own CR3, then the page-table structure. We move to the
     * kernel master mapping before freeing the PML4 itself, since
     * paging_free_process_pml4() refuses to run on a live CR3. schedule()
     * installs the next process's CR3 shortly after anyway, so this is
     * just a safe resting point in between. */
    if (p->is_user) {
        process_free_user_memory(p);

        uint64_t pml4_phys  = p->page_table & ~0xFFFULL;
        uint64_t kernel_cr3 = paging_get_master_cr3() & ~0xFFFULL;

        if (pml4_phys && pml4_phys != kernel_cr3) {
            paging_switch_cr3(kernel_cr3);
            paging_free_process_pml4(pml4_phys);
        }
    }

    p->page_table = 0;
    p->cr3        = 0;
}

// Terminate the current process
void do_exit(int status) {
    process_t *p = (process_t *)current;
    if (!p) return;

    com_write_string(COM1_PORT, "[EXIT] PID ");
    char buf[16];
    itoa(p->pid, buf, 10);
    com_write_string(COM1_PORT, buf);
    com_write_string(COM1_PORT, " exiting with status ");
    itoa(status, buf, 10);
    com_write_string(COM1_PORT, buf);
    com_write_string(COM1_PORT, "\n");

    process_teardown_resources(p);

    // Reparent children to init (PID 1) under the children lock
    spinlock_lock(&children_lock);
    if (p->children) {
        process_t *init = process_find(1);
        if (init) {
            process_t *child = p->children;
            while (child) {
                process_t *next = child->sibling_next;

                child->parent = init;
                child->ppid = init->pid;

                child->sibling_next = init->children;
                child->sibling_prev = NULL;
                if (init->children) {
                    init->children->sibling_prev = child;
                }
                init->children = child;

                child = next;
            }
        }
        p->children = NULL;
    }
    spinlock_unlock(&children_lock);

    // Mark zombie BEFORE removing from scheduler.
    p->state = PROCESS_STATE_ZOMBIE;
    /* Encode exit status in POSIX wait-status format:
     *   Normal exit  (status >= 0): wstatus = (status & 0xFF) << 8
     *   Signal death (status < 0) : wstatus = (-status) & 0x7F  */
    if (status < 0)
        p->exit_code = (-status) & 0x7F;
    else
        p->exit_code = (status & 0xFF) << 8;

    // Remove from the run queue now that state is visibly ZOMBIE.
    scheduler_remove(p);

    // Wake up parent if it is sleeping in waitpid
    if (p->parent) {
        wakeup(p->parent);
    }

    // Yield the CPU - we will never run again
    schedule();

    for (;;) {
        __asm__ volatile("hlt");
    }
}