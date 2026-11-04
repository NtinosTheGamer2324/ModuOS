// wait_new.c - POSIX wait/waitpid implementation

#include "moduos/kernel/process/process_new.h"
#include "moduos/kernel/spinlock.h"
#include "moduos/kernel/COM/com.h"
#include "moduos/kernel/errno.h"
#include "moduos/kernel/debug.h"
#include "moduos/kernel/memory/usercopy.h"
#include "moduos/kernel/memory/phys.h"
#include "moduos/kernel/memory/paging.h"

extern char *itoa(int value, char *str, int base);
extern spinlock_t children_lock;

int do_wait(int *status) {
    return do_waitpid(-1, status, 0);
}

int do_waitpid_full(int32_t pid, int *kernel_status, int options,
                    void *user_status_ptr);

int do_waitpid(int32_t pid, int *status, int options) {
    return do_waitpid_full(pid, status, options, NULL);
}

int do_waitpid_full(int32_t pid, int *kernel_status, int options,
                    void *user_status_ptr) {
    process_t *parent = process_get_current();

    if (!parent) {
        com_write_string(COM1_PORT, "[WAIT] ERROR: current is NULL!\n");
        return -1;
    }
    if ((uint64_t)parent < 0xFFFF800000000000ULL) {
        com_write_string(COM1_PORT, "[WAIT] ERROR: current pointer invalid\n");
        return -ESRCH;
    }

    char buf[16];

    while (1) {
        spinlock_lock(&children_lock);

        if (!parent->children) {
            spinlock_unlock(&children_lock);
            return -ECHILD;
        }

        if (pid > 0) {
            int found_match = 0;
            process_t *c = parent->children;
            while (c) {
                if (c->pid == (uint32_t)pid) { found_match = 1; break; }
                c = c->sibling_next;
            }
            if (!found_match) {
                spinlock_unlock(&children_lock);
                return -ECHILD;
            }
        }

        process_t *child = parent->children;
        process_t *found = NULL;

        while (child) {
            int match = (pid == -1) ||
                        (pid >  0 && (uint32_t)pid == child->pid) ||
                        (pid == 0 && child->pgid == parent->pgid) ||
                        (pid < -1 && child->pgid == (uint32_t)(-pid));
            if (match && child->state == PROCESS_STATE_ZOMBIE) {
                found = child;
                break;
            }
            child = child->sibling_next;
        }

        if (found) {
            if (kernel_debug_is_on()) {
                com_write_string(COM1_PORT, "[WAIT] Reaping zombie PID ");
                itoa((int)found->pid, buf, 10);
                com_write_string(COM1_PORT, buf);
                com_write_string(COM1_PORT, "\n");
            }

            uint32_t child_pid = found->pid;
            int      exit_code = found->exit_code;

            /* Unlink from parent's children list. */
            if (found->sibling_prev)
                found->sibling_prev->sibling_next = found->sibling_next;
            else
                parent->children = found->sibling_next;
            if (found->sibling_next)
                found->sibling_next->sibling_prev = found->sibling_prev;

            spinlock_unlock(&children_lock);

            /*
             * Step 1 — write exit status NOW, before any invlpg fires.
             *
             * process_free_user_memory() calls paging_unmap_page() which
             * issues invlpg for every page it frees.  invlpg evicts TLB
             * entries by virtual address regardless of CR3, so it can shoot
             * the parent's own stack/heap entries even after we restore CR3.
             * Writing the status here, while the parent's TLB is still warm
             * and page_table is valid, avoids that race entirely.
             */
            if (kernel_status)
                *kernel_status = exit_code;

            if (user_status_ptr) {
                if (usercopy_to_user(user_status_ptr, &exit_code,
                                     sizeof(exit_code)) != 0) {
                    com_write_string(COM1_PORT,
                        "[WAIT] WARNING: usercopy to user_status_ptr failed\n");
                    /* Non-fatal — parent asked for status but mapping is gone.
                     * Still reap the child so it doesn't stay zombie forever. */
                }
            }

            /*
             * Step 2/3 -- free the child's user-space mappings and PML4,
             * if that has not already happened.
             *
             * do_exit() now frees a process's own address space and page
             * tables at exit time (see exit_new.c:process_teardown_resources),
             * switching to the kernel master CR3 once done. That covers
             * every normal termination path (exit(), signal-kill,
             * fault-kill), so by the time a child shows up here as a
             * zombie, found->page_table already just points at the kernel
             * master mapping (switch_to() records whatever CR3 was live
             * when a process was last scheduled away from, which by then
             * is the master one, not the process's own freed PML4) and
             * there is nothing left to free.
             *
             * We still check rather than assuming: if we called
             * process_free_user_memory() unconditionally here on an
             * already-freed process, and it somehow still recorded its
             * *own* old PML4 (found->is_user is untouched by do_exit(), so
             * the function would not simply no-op), it would walk the
             * *parent's* live page tables instead whenever dying_cr3 reads
             * as 0. Comparing against the master CR3 (rather than just
             * checking for non-zero) avoids both that and a duplicate call
             * into paging_free_process_pml4(), and keeps this path as a
             * defensive fallback for any future caller that reaps a child
             * without going through do_exit() first.
             */
            {
                uint64_t kernel_cr3 = paging_get_master_cr3() & ~0xFFFULL;
                uint64_t pml4_phys  = found->page_table & ~0xFFFULL;

                if (pml4_phys && pml4_phys != kernel_cr3) {
                    extern void process_free_user_memory(process_t *p);
                    process_free_user_memory(found);

                    paging_switch_cr3(kernel_cr3);
                    paging_free_process_pml4(pml4_phys);
                }
                found->page_table = 0;
                found->cr3        = 0;
            }

            /* Step 4 — verify CR3 is still the parent's (paranoia check). */
            {
                uint64_t verify_cr3;
                __asm__ volatile("mov %%cr3, %0" : "=r"(verify_cr3));
                uint64_t expected = parent->page_table & ~0xFFFULL;
                if (expected && (verify_cr3 & ~0xFFFULL) != expected) {
                    com_write_string(COM1_PORT,
                        "[WAIT] BUG: CR3 wrong after free, restoring\n");
                    __asm__ volatile("mov %0, %%cr3"
                                     :: "r"(parent->page_table) : "memory");
                }
            }

            /*
             * Step 5 — destroy the process struct.
             *
             * page_table is already 0 so process_destroy()'s sanity check
             * will not fire and will not attempt a second free.
             */
            extern void process_destroy(process_t *p);
            process_destroy(found);

            if (kernel_debug_is_on()) {
                com_write_string(COM1_PORT, "[WAIT] Reap complete, child_pid=");
                itoa((int)child_pid, buf, 10);
                com_write_string(COM1_PORT, buf);
                com_write_string(COM1_PORT, "\n");
            }

            return (int)child_pid;
        }

        spinlock_unlock(&children_lock);

        if (options & 1)  /* WNOHANG */
            return 0;

        sleep_on(parent);
    }
}