// process.c - Process owns its arguments
// Compiled against process_new.h so struct layouts match the rest of the new
// process subsystem. process.h is intentionally NOT included here to avoid
// the old process_t definition conflicting with the new one.
//
// REFACTOR — unified current pointer
// ------------------------------------
// The old file kept a file-local  static process_t *current_process  in
// addition to the global  volatile process_t *current  that the scheduler
// owns.  Having two pointers for the same thing created a race:
//
//   - A timer IRQ calls scheduler_tick() → sets need_resched.
//   - The IRQ return path calls schedule() → updates global 'current'.
//   - Any kernel code that ran between those two steps and read
//     current_process saw the *old* process, giving the wrong ppid, uid,
//     cwd, etc. to the newly created child.
//
// Fix: delete static current_process entirely.  set_curproc() writes only
// the global 'current'.  Every former use of current_process or get_curproc()
// is replaced with a plain cast of the global:  (process_t *)current.
//
// REFACTOR — embedded scheduler node zero-init
// ---------------------------------------------
// process_new.h now embeds rbtree_node_t sched_node inside process_t.
// We memset it to zero immediately after the context zero-init so that
// sched_node_in_tree() in scheduler.c correctly returns false for a
// brand-new process before its first enqueue.

#include "moduos/kernel/process/process_new.h"
#include "moduos/kernel/memory/paging.h"

#ifndef KERNEL_STACK_SIZE
#define KERNEL_STACK_SIZE 16384
#endif
#ifndef USER_STACK_SIZE
#define USER_STACK_SIZE 65536
#endif

extern void scheduler_add_process(process_t *proc);
extern void scheduler_remove_process(process_t *proc);
extern uint32_t scheduler_nice_to_weight(int nice);
extern uint64_t scheduler_get_min_vruntime(void);
extern uint64_t scheduler_get_clock_ticks(void);
extern void debug_print_ready_queue(void);

extern void fpu_lazy_on_context_switch(process_t *next);
extern void fpu_lazy_on_process_exit(process_t *p);
extern void fpu_lazy_handle_nm(void);

#include "moduos/kernel/user_identity.h"
#include "moduos/kernel/memory/memory.h"
#include "moduos/kernel/memory/paging.h"
#include "moduos/kernel/memory/phys.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/macros.h"
#include "moduos/kernel/debug.h"
#include "moduos/kernel/spinlock.h"
#include "moduos/arch/AMD64/cpu.h"
#include "moduos/arch/AMD64/syscall/syscall64_stack.h"
#include <stdint.h>

#ifndef PAGE_SIZE
#define PAGE_SIZE 0x1000
#endif
#ifndef HIGHER_HALF_OFFSET
#define HIGHER_HALF_OFFSET 0xFFFF800000000000ULL
#endif

#define PHYS_TO_VIRT(addr) ((uint64_t)(addr) + HIGHER_HALF_OFFSET)
#define VIRT_TO_PHYS(addr) ((uint64_t)(addr) - HIGHER_HALF_OFFSET)

extern uint32_t process_alloc_pid(void);
extern int process_register(process_t *proc);
extern int process_unregister(uint32_t pid);
extern void process_table_init(void);
extern void process_return_trampoline(void);

// ---------------------------------------------------------------------------
// current — single authoritative pointer
//
// The global 'current' is declared as  volatile process_t *current  in
// process_new.h / process_table.c.  We do NOT maintain a separate local
// copy here any more.  All code in this file reads  (process_t *)current
// and writes through set_curproc().
// ---------------------------------------------------------------------------

// Global current pointer declared in process_table.c
extern volatile process_t *current;

void set_curproc(process_t *p) {
    // Single write — the scheduler and all process subsystem callers share
    // this one pointer.  No secondary static copy to drift out of sync.
    current = p;
}

// ---------------------------------------------------------------------------
// Argv helpers
// ---------------------------------------------------------------------------

static char **copy_argv(int argc, char **argv) {
    if (argc <= 0 || !argv) return NULL;
    char **out = (char **)kzalloc(sizeof(char *) * (argc + 1));
    if (!out) return NULL;
    for (int i = 0; i < argc; i++) {
        const char *src = argv[i] ? argv[i] : "";
        size_t len = strlen(src) + 1;
        out[i] = (char *)kzalloc(len);
        if (!out[i]) {
            for (int j = 0; j < i; j++) { if (out[j]) kfree(out[j]); }
            kfree(out);
            return NULL;
        }
        memcpy(out[i], src, len);
    }
    out[argc] = NULL;
    return out;
}

static void free_argv(int argc, char **argv) {
    if (!argv) return;
    for (int i = 0; i < argc; i++) {
        if (argv[i]) kfree(argv[i]);
    }
    kfree(argv);
}

// ---------------------------------------------------------------------------
// CR3 accessor used by the user-mode entry trampoline
// ---------------------------------------------------------------------------

uint64_t process_get_current_cr3(void) {
    if (!current) return 0;
    return current->cr3;
}

// ---------------------------------------------------------------------------
// process_destroy
// ---------------------------------------------------------------------------

void process_destroy(process_t *proc) {
    if (!proc) return;

    if (proc->page_table) {
        com_write_string(COM1_PORT, "[PROC] BUG: destroy called with live page_table PID=");
        char b[12]; itoa((int)proc->pid, b, 10);
        com_write_string(COM1_PORT, b);
        com_write_string(COM1_PORT, "\n");
    }

    (void)process_unregister(proc->pid);

    proc->user_image_base = 0; proc->user_image_end  = 0;
    proc->user_heap_end   = proc->user_heap_base;
    proc->user_mmap_end   = proc->user_mmap_base;
    proc->user_stack_top  = 0; proc->user_stack_low  = 0;

    if (proc->kernel_stack) { kfree(proc->kernel_stack); proc->kernel_stack = NULL; }
    proc->context.rsp = 0;
    proc->context.rip = 0;

    if (proc->argv) { free_argv(proc->argc, proc->argv); proc->argv = NULL; }
    if (proc->envp) { free_argv(proc->envc, proc->envp); proc->envp = NULL; }

    char _db[12];
    com_write_string(COM1_PORT, "[PROC] PID ");
    itoa((int)proc->pid, _db, 10);
    com_write_string(COM1_PORT, _db);
    com_write_string(COM1_PORT, " destroyed\n");
    kfree(proc);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static inline uint64_t stack_top(void *stack_base) {
    return (uint64_t)stack_base + KERNEL_STACK_SIZE;
}

static uint64_t *g_build_pml4      = NULL;
static uint64_t  g_build_pml4_phys = 0;

void process_set_build_pml4(uint64_t *pml4_virt, uint64_t pml4_phys) {
    g_build_pml4      = pml4_virt;
    g_build_pml4_phys = pml4_phys;
}

uint64_t *process_get_build_pml4(void) {
    return g_build_pml4;
}

static void free_user_range(uint64_t start, uint64_t end) {
    if (end <= start) return;
    start &= ~0xFFFULL;
    end    = (end + 0xFFFULL) & ~0xFFFULL;

    for (uint64_t cur = start; cur < end; cur += 0x1000ULL) {
        uint64_t phys = paging_virt_to_phys(cur);
        if (phys != 0) {
            paging_unmap_page(cur);
            phys_ref_dec(phys & ~0xFFFULL);
        }
    }
}

void process_free_user_memory(process_t *p) {
    if (!p || !p->is_user) return;

    uint64_t saved_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(saved_cr3));
    uint64_t dying_cr3 = p->page_table & ~0xFFFULL;

    int switched = 0;
    if (dying_cr3 && (saved_cr3 & ~0xFFFULL) != dying_cr3) {
        paging_switch_cr3(dying_cr3);
        switched = 1;
    }

    if (p->user_image_base && p->user_image_end > p->user_image_base)
        free_user_range(p->user_image_base, p->user_image_end);

    free_user_range(p->user_heap_base, p->user_heap_end);
    free_user_range(p->user_mmap_base, p->user_mmap_end);

    if (p->user_stack_top && p->user_stack_low && p->user_stack_top > p->user_stack_low)
        free_user_range(p->user_stack_low, p->user_stack_top);
    else if (p->user_stack) {
        uint64_t base = (uint64_t)(uintptr_t)p->user_stack;
        free_user_range(base, base + USER_STACK_SIZE);
    }

    if (switched)
        __asm__ volatile("mov %0, %%cr3" :: "r"(saved_cr3) : "memory");
}

// ---------------------------------------------------------------------------
// process_exit
// ---------------------------------------------------------------------------

void process_exit(int exit_code) {
    fpu_lazy_on_process_exit((process_t *)current);
    process_t *cp = (process_t *)current;
    if (!cp) {
        com_write_string(COM1_PORT, "[PROC] process_exit: no current process!\n");
        for (;;) { __asm__ volatile("hlt"); }
    }

    char pidbuf[12];
    if (kernel_debug_is_on()) {
        com_write_string(COM1_PORT, "[PROC] process_exit: PID=");
        itoa((int)cp->pid, pidbuf, 10);
        com_write_string(COM1_PORT, pidbuf);
        com_write_string(COM1_PORT, " code=");
        itoa(exit_code, pidbuf, 10);
        com_write_string(COM1_PORT, pidbuf);
        com_write_string(COM1_PORT, "\n");
    }

    cp->state     = PROCESS_STATE_ZOMBIE;
    cp->exit_code = exit_code;

    scheduler_remove_process(cp);

    process_t *parent = process_get_by_pid(cp->ppid);
    if (parent) wakeup(parent);

    if (kernel_debug_is_on())
        com_write_string(COM1_PORT, "[PROC] process_exit: calling schedule()\n");
    __asm__ volatile("sti" ::: "memory");
    schedule();

    for (;;) { __asm__ volatile("hlt"); }
}

// ---------------------------------------------------------------------------
// process_wake
// ---------------------------------------------------------------------------

void process_wake(uint32_t pid) {
    process_t *p = process_get_by_pid(pid);
    if (!p || p->state != PROCESS_STATE_SLEEPING) return;
    p->state = PROCESS_STATE_READY;
    scheduler_add_process(p);
}

// ---------------------------------------------------------------------------
// process_create / process_create_with_args
// ---------------------------------------------------------------------------

process_t *process_create(const char *name, void (*entry_point)(void), int priority) {
    return process_create_with_args(name, entry_point, priority, 0, NULL);
}

extern void amd64_enter_user_trampoline(void);

process_t *process_create_with_args(const char *name, void (*entry_point)(void),
                                    int priority, int argc, char **argv) {
    uint32_t pid = process_alloc_pid();
    if (pid >= MAX_PROCESSES) {
        COM_LOG_ERROR(COM1_PORT, "Process table full");
        return NULL;
    }

    process_t *proc = (process_t *)kzalloc(sizeof(process_t));
    if (!proc) {
        COM_LOG_ERROR(COM1_PORT, "Failed to allocate process structure");
        return NULL;
    }

    proc->pid  = pid;
    // Use the single authoritative 'current' pointer for ppid — no local copy.
    proc->ppid = current ? current->pid : 0;
    strncpy(proc->name, name, PROCESS_NAME_MAX - 1);
    proc->state    = PROCESS_STATE_READY;
    proc->priority = priority;

    proc->uid = current ? current->uid : 0;
    if (!current || uid_is_kernel(proc->uid))
        proc->uid = 0;
    proc->gid = current ? current->gid : 0;

    if (argc > 0 && argv) {
        proc->argv = copy_argv(argc, argv);
        if (!proc->argv) {
            COM_LOG_ERROR(COM1_PORT, "Failed to copy arguments");
            kfree(proc);
            return NULL;
        }
        proc->argc = argc;

        com_write_string(COM1_PORT, "[PROC] Copied ");
        char buf[12];
        itoa(argc, buf, 10);
        com_write_string(COM1_PORT, buf);
        com_write_string(COM1_PORT, " arguments for process\n");
    } else {
        proc->argc = 0;
        proc->argv = NULL;
    }

    proc->kernel_stack = kmalloc(KERNEL_STACK_SIZE);
    if ((uint64_t)(uintptr_t)proc->kernel_stack < 0xFFFF800000000000ULL)
        COM_LOG_ERROR(COM1_PORT, "Kernel stack allocation not in higher half");
    if (!proc->kernel_stack) {
        COM_LOG_ERROR(COM1_PORT, "Failed to allocate kernel stack");
        if (proc->argv) free_argv(proc->argc, proc->argv);
        kfree(proc);
        return NULL;
    }
    memset(proc->kernel_stack, 0, KERNEL_STACK_SIZE);

    uint64_t kernel_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(kernel_cr3));
    proc->page_table = kernel_cr3;
    proc->cr3        = kernel_cr3;

    // Zero the CPU context and the embedded scheduler node.
    // The sched_node zero-init is critical: sched_node_in_tree() in
    // scheduler.c checks whether left/right/parent are non-NULL or whether
    // the node is the current tree root.  A fresh allocation from kzalloc is
    // already zeroed, but we do this explicitly after the context memset to
    // make the invariant obvious at the call site.
    memset(&proc->context,   0, sizeof(cpu_context_t));
    memset(&proc->sched_node, 0, sizeof(rbtree_node_t));

    proc->is_user  = 0;
    proc->user_rip = 0;
    proc->user_rsp = 0;

    uint64_t ep = (uint64_t)entry_point;
    if (ep >= 0x0000000000400000ULL && ep < 0x0000800000000000ULL) {
        proc->is_user  = 1;
        proc->user_rip = ep;

        if (g_build_pml4_phys) {
            proc->page_table = g_build_pml4_phys;
            proc->cr3        = g_build_pml4_phys;
        } else {
            uint64_t new_pml4_phys = paging_create_process_pml4();
            if (!new_pml4_phys) {
                COM_LOG_ERROR(COM1_PORT, "Failed to create process page table");
                if (proc->argv) free_argv(proc->argc, proc->argv);
                kfree(proc->kernel_stack);
                kfree(proc);
                return NULL;
            }
            proc->page_table = new_pml4_phys;
            proc->cr3        = new_pml4_phys;
        }

        proc->context.rbx = 0;

        const uint64_t user_stack_top  = 0x00007FFFFFF00000ULL;
        const uint64_t user_stack_base = user_stack_top - USER_STACK_SIZE;

        size_t   pages    = USER_STACK_SIZE / PAGE_SIZE;
        uint64_t phys_base = phys_alloc_contiguous(pages);
        if (!phys_base) {
            COM_LOG_ERROR(COM1_PORT, "Failed to allocate user stack");
            if (proc->argv) free_argv(proc->argc, proc->argv);
            kfree(proc->kernel_stack);
            kfree(proc);
            return NULL;
        }

        com_write_string(COM1_PORT, "[PROC] Mapping stack: base=0x");
        com_write_hex64(COM1_PORT, user_stack_base);
        com_write_string(COM1_PORT, " phys=0x");
        com_write_hex64(COM1_PORT, phys_base);
        com_write_string(COM1_PORT, " size=0x");
        com_write_hex64(COM1_PORT, USER_STACK_SIZE);
        com_write_string(COM1_PORT, " into PML4=0x");
        com_write_hex64(COM1_PORT, proc->page_table);
        com_write_string(COM1_PORT, "\n");

        uint64_t *proc_pml4 = (uint64_t *)phys_to_virt_kernel(proc->page_table);
        if (!proc_pml4) {
            COM_LOG_ERROR(COM1_PORT, "Failed to get process PML4");
            for (size_t p = 0; p < pages; p++) phys_free_frame(phys_base + p * PAGE_SIZE);
            if (proc->argv) free_argv(proc->argc, proc->argv);
            kfree(proc->kernel_stack);
            kfree(proc);
            return NULL;
        }

        int map_rc = paging_map_range_to_pml4(proc_pml4, user_stack_base, phys_base,
                                               USER_STACK_SIZE,
                                               PFLAG_PRESENT | PFLAG_WRITABLE | PFLAG_USER);
        if (map_rc == 0) {
            com_write_string(COM1_PORT, "[PROC] Stack mapping SUCCESS\n");
        } else {
            com_write_string(COM1_PORT, "[PROC] Stack mapping FAILED\n");
            COM_LOG_ERROR(COM1_PORT, "Failed to map user stack");
            for (size_t p = 0; p < pages; p++) phys_free_frame(phys_base + p * PAGE_SIZE);
            if (proc->argv) free_argv(proc->argc, proc->argv);
            kfree(proc->kernel_stack);
            kfree(proc);
            return NULL;
        }

        proc->user_stack      = (void *)(uintptr_t)user_stack_base;
        proc->user_stack_top  = user_stack_top;
        proc->user_stack_low  = user_stack_base;
        proc->user_stack_limit = user_stack_top - (2048 * PAGE_SIZE);

        proc->user_heap_base  = 0x0000005000000000ULL;
        proc->user_heap_end   = proc->user_heap_base;
        proc->user_heap_limit = proc->user_heap_base + 64ULL * 1024ULL * 1024ULL;

        proc->user_mmap_base  = 0x0000006000000000ULL;
        proc->user_mmap_end   = proc->user_mmap_base;
        proc->user_mmap_limit = proc->user_mmap_base + 256ULL * 1024ULL * 1024ULL;

        {
            uint64_t sp = user_stack_top - 8;
            *(uint64_t *)phys_to_virt_kernel(phys_base + (sp - user_stack_base)) = 0;

            proc->user_rsp    = sp;
            proc->context.r12 = (uint64_t)proc->argc;
            proc->context.r13 = 0;

            if (proc->argc > 0 && proc->argv) {
                int argc_clamped = proc->argc < 64 ? proc->argc : 64;
                uint64_t *user_str_ptrs = (uint64_t *)kzalloc(
                    (size_t)argc_clamped * sizeof(uint64_t));
                if (!user_str_ptrs) {
                    COM_LOG_ERROR(COM1_PORT, "Failed to allocate argv staging buffer");
                    goto argv_stack_done;
                }

                for (int i = argc_clamped - 1; i >= 0; i--) {
                    const char *s = proc->argv[i] ? proc->argv[i] : "";
                    size_t len = strlen(s) + 1;
                    sp -= len;
                    memcpy(phys_to_virt_kernel(phys_base + (sp - user_stack_base)), s, len);
                    user_str_ptrs[i] = sp;
                }

                sp &= ~0x7ULL;
                sp -= 8;
                *(uint64_t *)phys_to_virt_kernel(phys_base + (sp - user_stack_base)) = 0;
                for (int i = argc_clamped - 1; i >= 0; i--) {
                    sp -= 8;
                    *(uint64_t *)phys_to_virt_kernel(phys_base + (sp - user_stack_base)) =
                        user_str_ptrs[i];
                }

                kfree(user_str_ptrs);

                uint64_t argv_va = sp;
                if ((sp & 0xFULL) != 8) sp -= 8;

                proc->user_rsp    = sp;
                proc->context.r12 = (uint64_t)argc_clamped;
                proc->context.r13 = argv_va;
            }
argv_stack_done:;
        }

        proc->context.r14 = proc->user_rip;
        proc->context.r15 = proc->user_rsp;
        proc->context.rip = (uint64_t)(uintptr_t)amd64_enter_user_trampoline;

    } else {
        proc->context.rip = ep;
    }

    uint64_t top = (stack_top(proc->kernel_stack) - 16) & ~0xFULL;
    if ((top >> 48) != 0xFFFF)
        COM_LOG_ERROR(COM1_PORT, "Invalid kernel stack top canonicality");

    uint64_t  initial_rsp = top - 8;
    uint64_t *ret_slot    = (uint64_t *)initial_rsp;
    *ret_slot = (uint64_t)process_return_trampoline;

    proc->context.rsp    = initial_rsp;
    proc->context.rbp    = initial_rsp;
    proc->context.rflags = 0x202;

    /* fxrstor64 requires 16-byte alignment. Allocate extra and align. */
    void *fpu_raw = kzalloc(512 + 16);
    if (!fpu_raw) {
        COM_LOG_ERROR(COM1_PORT, "Failed to allocate FPU state");
        if (proc->argv) free_argv(proc->argc, proc->argv);
        kfree(proc->kernel_stack);
        kfree(proc);
        return NULL;
    }
    if (!proc->is_user) {
        if (proc->argc > 0 && proc->argv) {
            proc->context.r12 = (uint64_t)proc->argc;
            proc->context.r13 = (uint64_t)proc->argv;
        } else {
            proc->context.r12 = 0;
            proc->context.r13 = 0;
        }
    } else {
        if (proc->argc <= 0) {
            proc->context.r12 = 0;
            proc->context.r13 = 0;
        }
        proc->context.rbx = 0;
    }

    if (current) {
        proc->current_slot = current->current_slot;
        strncpy(proc->cwd, (const char *)current->cwd, sizeof(proc->cwd) - 1);
        proc->cwd[sizeof(proc->cwd) - 1] = 0;
    } else {
        proc->current_slot = -1;
        proc->cwd[0]       = 0;
    }

    proc->total_time  = 0;
    proc->nice        = 0;
    proc->weight      = scheduler_nice_to_weight(0);
    proc->vruntime    = scheduler_get_min_vruntime();
    proc->exec_start  = scheduler_get_clock_ticks();

    com_write_string(COM1_PORT, "[PROC] Created process: ");
    com_write_string(COM1_PORT, name);
    com_write_string(COM1_PORT, " (PID ");
    char pidbuf[12];
    itoa(pid, pidbuf, 10);
    com_write_string(COM1_PORT, pidbuf);
    com_write_string(COM1_PORT, ")\n");

    process_register(proc);
    scheduler_add_process(proc);

    return proc;
}

// ---------------------------------------------------------------------------
// process_yield (debug variant — only compiled with PROCESS_YIELD_DEBUG)
// ---------------------------------------------------------------------------

#ifdef PROCESS_YIELD_DEBUG
void process_yield_debug(void) {
    process_t *cp = (process_t *)current;
    if (!cp) return;
    if (kernel_debug_is_on()) {
        com_write_string(COM1_PORT, "[YIELD] Process ");
        char buf[12];
        itoa(cp->pid, buf, 10);
        com_write_string(COM1_PORT, buf);
        com_write_string(COM1_PORT, " yielding\n");
        debug_print_ready_queue();
    }
    schedule();
}
#endif