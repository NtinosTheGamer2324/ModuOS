// process.c - Process owns its arguments
// Compiled against process_new.h so struct layouts match the rest of the new
// process subsystem. process.h is intentionally NOT included here to avoid
// the old process_t definition conflicting with the new one.
#include "moduos/kernel/process/process_new.h"

// Constants defined in process.h that process_new.h does not carry.
#ifndef KERNEL_STACK_SIZE
#define KERNEL_STACK_SIZE 16384
#endif
#ifndef USER_STACK_SIZE
#define USER_STACK_SIZE 65536
#endif

// Scheduler helpers declared in process.h but not in process_new.h.
extern void scheduler_add_process(process_t *proc);
extern void scheduler_remove_process(process_t *proc);
extern uint32_t scheduler_nice_to_weight(int nice);
extern uint64_t scheduler_get_min_vruntime(void);
extern uint64_t scheduler_get_clock_ticks(void);
extern void debug_print_ready_queue(void);

// FPU lazy-switching hooks (fpu_lazy.c).
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

/* Process table management (process_table.c) */
extern uint32_t process_alloc_pid(void);
extern int process_register(process_t *proc);
extern int process_unregister(uint32_t pid);
extern void process_table_init(void);
extern void process_return_trampoline(void);

static process_t *current_process = NULL;

static char **copy_argv(int argc, char **argv) {
    if (argc <= 0 || !argv) return NULL;
    char **out = (char**)kzalloc(sizeof(char*) * (argc + 1));
    if (!out) return NULL;
    for (int i = 0; i < argc; i++) {
        const char *src = argv[i] ? argv[i] : "";
        size_t len = strlen(src) + 1;
        out[i] = (char*)kzalloc(len);
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

// NOTE: cpu-local storage is not fully initialized yet in current boot flow.
// Until SMP/per-CPU is brought up, use the legacy global pointer.
static inline process_t *get_curproc(void) {
    return current_process;
}

// Global current pointer declared in process_table.c
extern volatile process_t *current;

void set_curproc(process_t *p) {
    current_process = p;
    // Keep the new-system 'current' pointer in sync so process_new.h
    // callers (process_get_current, get_current) see the correct process.
    current = p;
}

/* Used by the user-mode entry trampoline to retrieve the current process CR3
 * without relying on a direct data-symbol relocation from NASM, which can
 * produce incorrect results when linking across separately-compiled objects. */


uint64_t process_get_current_cr3(void) {
    if (!current) return 0;
    return current->cr3;
}

/* process_alloc_pid, process_register, process_unregister moved to process_table.c */

void process_destroy(process_t *proc) {
    if (!proc) return;

    char _db[12];
    (void)process_unregister(proc->pid);

    // Caller is responsible for calling process_free_user_memory() first.
    // Null ranges to prevent any accidental double-free.
    proc->user_image_base = 0;
    proc->user_image_end  = 0;
    proc->user_heap_end   = proc->user_heap_base;
    proc->user_mmap_end   = proc->user_mmap_base;
    proc->user_stack_top  = 0;
    proc->user_stack_low  = 0;

    if (proc->kernel_stack) { kfree(proc->kernel_stack); proc->kernel_stack = NULL; }
    proc->context.rsp = 0;
    proc->context.rip = 0;

    if (proc->fpu_state)    { kfree(proc->fpu_state);    proc->fpu_state    = NULL; }
    if (proc->argv) { free_argv(proc->argc, proc->argv); proc->argv = NULL; }
    if (proc->envp) { free_argv(proc->envc, proc->envp); proc->envp = NULL; }

    com_write_string(COM1_PORT, "[PROC] PID ");
    itoa((int)proc->pid, _db, 10);
    com_write_string(COM1_PORT, _db);
    com_write_string(COM1_PORT, " destroyed\n");
    kfree(proc);
}

/* Helper: top of kernel stack pointer */
static inline uint64_t stack_top(void *stack_base) {
    return (uint64_t)stack_base + KERNEL_STACK_SIZE;
}

static uint64_t *g_build_pml4 = NULL;
static uint64_t g_build_pml4_phys = 0;

void process_set_build_pml4(uint64_t *pml4_virt, uint64_t pml4_phys) {
    g_build_pml4 = pml4_virt;
    g_build_pml4_phys = pml4_phys;
}

uint64_t *process_get_build_pml4(void) {
    return g_build_pml4;
}

static void free_user_range(uint64_t start, uint64_t end) {
    if (end <= start) return;
    start &= ~0xFFFULL;
    end = (end + 0xFFFULL) & ~0xFFFULL;

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

    /* ELF image and user-space allocations: free only user half ranges.
     * We currently use a single global page table, so we must explicitly clean up
     * the ranges we hand out to ring3.
     */

    /* Program image mapped by ELF loader (precise range recorded at exec time). */
    if (p->user_image_base && p->user_image_end && p->user_image_end > p->user_image_base) {
        free_user_range(p->user_image_base, p->user_image_end);
    }

    /* User heap mappings created by sys_sbrk() */
    free_user_range(p->user_heap_base, p->user_heap_end);

    /* User mmap mappings created by sys_mmap() */
    free_user_range(p->user_mmap_base, p->user_mmap_end);

    /* User stack region (including any growth). */
    if (p->user_stack_top && p->user_stack_low && p->user_stack_top > p->user_stack_low) {
        free_user_range(p->user_stack_low, p->user_stack_top);
    } else if (p->user_stack) {
        uint64_t base = (uint64_t)(uintptr_t)p->user_stack;
        free_user_range(base, base + USER_STACK_SIZE);
    }
}

/* process_get_current() is a static inline in process_new.h returning 'current'.
 * set_curproc() keeps 'current' in sync, so callers of either function see the
 * same process pointer. No out-of-line definition needed here. */

void process_exit(int exit_code) {
    fpu_lazy_on_process_exit(get_curproc());
    process_t *cp = get_curproc();
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

    /* Wake parent sleeping in do_waitpid(). */
    process_t *parent = process_get_by_pid(cp->ppid);
    if (parent) wakeup(parent);

    if (kernel_debug_is_on())
        com_write_string(COM1_PORT, "[PROC] process_exit: calling schedule()\n");
    __asm__ volatile("sti" ::: "memory");
    schedule();

    for (;;) { __asm__ volatile("hlt"); }
}

/* process_kill() is a static inline in process_new.h (sends SIGKILL).
 * The full teardown path goes through do_exit() → process_destroy(). */

/* process_yield() is a static inline in process_new.h calling schedule().
 * The debug-logging variant below is kept for instrumented builds but gated
 * so it does not conflict with the inline definition. */
#ifdef PROCESS_YIELD_DEBUG
void process_yield_debug(void) {
    process_t *cp = get_curproc();
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

/* process_sleep() is a static inline in process_new.h (uses sleep_on).
 * No out-of-line definition needed. */

void process_wake(uint32_t pid) {
    process_t *p = process_get_by_pid(pid);
    if (!p || p->state != PROCESS_STATE_SLEEPING) return;
    p->state = PROCESS_STATE_READY;
    scheduler_add_process(p);
}


process_t* process_create(const char *name, void (*entry_point)(void), int priority) {
    return process_create_with_args(name, entry_point, priority, 0, NULL);
}

extern void amd64_enter_user_trampoline(void);

process_t* process_create_with_args(const char *name, void (*entry_point)(void), int priority, int argc, char **argv) {
    uint32_t pid = process_alloc_pid();
    if (pid >= MAX_PROCESSES) {
        COM_LOG_ERROR(COM1_PORT, "Process table full");
        return NULL;
    }

    process_t *proc = (process_t*)kzalloc(sizeof(process_t));
    if (!proc) {
        COM_LOG_ERROR(COM1_PORT, "Failed to allocate process structure");
        return NULL;
    }

    proc->pid = pid;
    proc->ppid = current_process ? current_process->pid : 0;
    strncpy(proc->name, name, PROCESS_NAME_MAX - 1);
    proc->state = PROCESS_STATE_READY;
    proc->priority = priority;

    /* Early boot: allow userland helpers (userman/login) to run with UID 0
     * so they can register devfs nodes. Once user sessions are established,
     * SYS_SETUID can drop privileges.
     */
    proc->uid = current_process ? current_process->uid : 0;
    if (!current_process || uid_is_kernel(proc->uid)) {
        proc->uid = 0;
    }
    proc->gid = current_process ? current_process->gid : 0;

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
    if ((uint64_t)(uintptr_t)proc->kernel_stack < 0xFFFF800000000000ULL) {
        COM_LOG_ERROR(COM1_PORT, "Kernel stack allocation not in higher half");
    }
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

    /* The kernel stack lives in the kernel heap (0xFFFF800000000000+). Every process
     * PML4 inherits the high-half entries from the kernel PML4 via
     * paging_create_process_pml4(), so the kernel stack is already visible in the
     * process address space through the shared PDPT subtree.
     *
     * Calling paging_map_range_to_pml4() on a kernel-space address here is wrong:
     * get_or_create_in_pml4() treats the copied kernel PDPT entry (no PFLAG_USER)
     * as requiring replacement, discards the shared PDPT pointer, and installs a
     * fresh empty PDPT — severing the kernel heap mapping in the process CR3 and
     * causing the next kernel heap access to #PF, which then cascades into a #GP. */

    memset(&proc->context, 0, sizeof(cpu_context_t));

    proc->is_user = 0;
    proc->user_rip = 0;
    proc->user_rsp = 0;

    uint64_t ep = (uint64_t)entry_point;
    if (ep >= 0x0000000000400000ULL && ep < 0x0000800000000000ULL) {
        proc->is_user = 1;
        proc->user_rip = ep;
        
        // Use existing build PML4 if set (ELF already loaded into it),
        // otherwise create a new page table
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

        /* Ensure new user processes start with a clean envp (RBX).
         * The user entry trampoline passes rbx as envp, so leaking kernel
         * values here can crash early in userland startup.
         */
        proc->context.rbx = 0;

        const uint64_t user_stack_top = 0x00007FFFFFF00000ULL;
        const uint64_t user_stack_base = user_stack_top - USER_STACK_SIZE;

        size_t pages = USER_STACK_SIZE / PAGE_SIZE;
        uint64_t phys_base = phys_alloc_contiguous(pages);
        if (!phys_base) {
            COM_LOG_ERROR(COM1_PORT, "Failed to allocate user stack");
            if (proc->argv) free_argv(proc->argc, proc->argv);
            kfree(proc->kernel_stack);
            kfree(proc);
            return NULL;
        }

        // Map stack into the process's own page table
        com_write_string(COM1_PORT, "[PROC] Mapping stack: base=0x");
        com_write_hex64(COM1_PORT, user_stack_base);
        com_write_string(COM1_PORT, " phys=0x");
        com_write_hex64(COM1_PORT, phys_base);
        com_write_string(COM1_PORT, " size=0x");
        com_write_hex64(COM1_PORT, USER_STACK_SIZE);
        com_write_string(COM1_PORT, " into PML4=0x");
        com_write_hex64(COM1_PORT, proc->page_table);
        com_write_string(COM1_PORT, "\n");
        
        uint64_t *proc_pml4 = (uint64_t*)phys_to_virt_kernel(proc->page_table);
        if (!proc_pml4) {
            COM_LOG_ERROR(COM1_PORT, "Failed to get process PML4");
            for (size_t p = 0; p < pages; p++) phys_free_frame(phys_base + p * PAGE_SIZE);
            if (proc->argv) free_argv(proc->argc, proc->argv);
            kfree(proc->kernel_stack);
            kfree(proc);
            return NULL;
        }
        int map_rc = paging_map_range_to_pml4(proc_pml4, user_stack_base, phys_base, USER_STACK_SIZE, PFLAG_PRESENT | PFLAG_WRITABLE | PFLAG_USER);
        if (map_rc == 0) {
            com_write_string(COM1_PORT, "[PROC] Stack mapping SUCCESS\n");
        } else {
            com_write_string(COM1_PORT, "[PROC] Stack mapping FAILED\n");
        }
        if (map_rc != 0) {
            COM_LOG_ERROR(COM1_PORT, "Failed to map user stack");
            for (size_t p = 0; p < pages; p++) phys_free_frame(phys_base + p * PAGE_SIZE);
            if (proc->argv) free_argv(proc->argc, proc->argv);
            kfree(proc->kernel_stack);
            kfree(proc);
            return NULL;
        }

        proc->user_stack = (void*)(uintptr_t)user_stack_base;
        proc->user_stack_top = user_stack_top;
        proc->user_stack_low = user_stack_base;
        // Allow stack to grow down from base - max 2048 pages (8MB) to match execve_impl.c
        proc->user_stack_limit = user_stack_top - (2048 * PAGE_SIZE);

        proc->user_heap_base = 0x0000005000000000ULL;
        proc->user_heap_end = proc->user_heap_base;
        proc->user_heap_limit = proc->user_heap_base + 64ULL * 1024ULL * 1024ULL;

        proc->user_mmap_base  = 0x0000006000000000ULL;
        proc->user_mmap_end   = proc->user_mmap_base;
        proc->user_mmap_limit = proc->user_mmap_base + 256ULL * 1024ULL * 1024ULL;

        /*
         * _start in libc.h is a plain C function: void _start(long argc, char **argv).
         * The compiler generates a standard prologue (push rbp; mov rbp,rsp …) which
         * expects RSP % 16 == 8 at the call site — as if a `call` instruction had
         * pushed an 8-byte return address.  iretq jumps directly without pushing
         * anything, so we must pre-bias RSP by -8 ourselves.
         *
         * argc and argv are passed in rdi/rsi via the register ABI (r12→rdi, r13→rsi
         * in the trampoline).  The stack only needs a valid 8-byte-misaligned top.
         *
         * Also write a zero return-address sentinel at [rsp] so that if _start ever
         * returns (it shouldn't), the fault is obvious rather than silent corruption.
         */
        {
            /* Start from the top of the user stack, push a NULL sentinel, then
             * bias by -8 to satisfy RSP % 16 == 8 at _start entry. */
            uint64_t sp = user_stack_top - 8;   /* sp % 16 == 8 after this */
            *(uint64_t *)phys_to_virt_kernel(phys_base + (sp - user_stack_base)) = 0;

            proc->user_rsp = sp;
            proc->context.r12 = (uint64_t)proc->argc;
            proc->context.r13 = 0;   /* argv built via register; kernel ptr unusable in user */

            /* Copy argv strings onto the stack and build a proper argv[] pointer
             * array so the process can inspect argv[i] at runtime via rsi. */
            if (proc->argc > 0 && proc->argv) {
                int argc_clamped = proc->argc < 64 ? proc->argc : 64;
                /* Heap-allocate the pointer staging array — a 512-byte on-stack
                 * array overflows the 8 KiB kernel stack in this frame. */
                uint64_t *user_str_ptrs = (uint64_t *)kzalloc(
                    (size_t)argc_clamped * sizeof(uint64_t));
                if (!user_str_ptrs) {
                    COM_LOG_ERROR(COM1_PORT, "Failed to allocate argv staging buffer");
                    goto argv_stack_done;
                }

                /* Push strings high-to-low. */
                for (int i = argc_clamped - 1; i >= 0; i--) {
                    const char *s = proc->argv[i] ? proc->argv[i] : "";
                    size_t len = strlen(s) + 1;
                    sp -= len;
                    memcpy(phys_to_virt_kernel(phys_base + (sp - user_stack_base)), s, len);
                    user_str_ptrs[i] = sp;
                }

                /* Align to 8 bytes, then lay out argv[] + NULL terminator. */
                sp &= ~0x7ULL;
                sp -= 8;
                *(uint64_t *)phys_to_virt_kernel(phys_base + (sp - user_stack_base)) = 0;
                for (int i = argc_clamped - 1; i >= 0; i--) {
                    sp -= 8;
                    *(uint64_t *)phys_to_virt_kernel(phys_base + (sp - user_stack_base)) = user_str_ptrs[i];
                }

                kfree(user_str_ptrs);

                /* sp now points at argv[0] — the argv[] array base in user VA. */
                uint64_t argv_va = sp;

                /* Ensure RSP % 16 == 8 at _start entry. */
                if ((sp & 0xFULL) != 8) sp -= 8;

                proc->user_rsp = sp;
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
    if ((top >> 48) != 0xFFFF) {
        COM_LOG_ERROR(COM1_PORT, "Invalid kernel stack top canonicality");
    }

    uint64_t initial_rsp = top - 8;
    uint64_t *ret_slot = (uint64_t *)initial_rsp;
    *ret_slot = (uint64_t)process_return_trampoline;

    proc->context.rsp = initial_rsp;
    proc->context.rbp = initial_rsp;
    proc->context.rflags = 0x202;

    // fpu_state is a void* — allocate 512-byte FXSAVE area.
    proc->fpu_state = kzalloc(512);
    if (!proc->fpu_state) {
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
        /* Ensure envp is a valid NULL pointer for user entry. */
        proc->context.rbx = 0;
    }

    if (current_process) {
        proc->current_slot = current_process->current_slot;
        strncpy(proc->cwd, current_process->cwd, sizeof(proc->cwd) - 1);
        proc->cwd[sizeof(proc->cwd) - 1] = 0;
    } else {
        proc->current_slot = -1;
        proc->cwd[0] = 0;
    }

    proc->total_time = 0;
    proc->nice = 0;
    proc->weight = scheduler_nice_to_weight(0);
    proc->vruntime = scheduler_get_min_vruntime();
    proc->exec_start = scheduler_get_clock_ticks();

    com_write_string(COM1_PORT, "[PROC] Created process: ");
    com_write_string(COM1_PORT, name);
    com_write_string(COM1_PORT, " (PID ");
    char pidbuf[12];
    itoa(pid, pidbuf, 10);
    com_write_string(COM1_PORT, pidbuf);
    com_write_string(COM1_PORT, ")\n");

    process_register(proc);
    
    /* Add the new process to the scheduler's ready queue so it can be scheduled */
    scheduler_add_process(proc);

    return proc;
}