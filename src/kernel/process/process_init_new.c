// process_init_new.c - Initialize the new process management system

#include "moduos/kernel/process/process_new.h"
#include "moduos/kernel/memory/kheap.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/COM/com.h"
#include <stddef.h>

/* Must match KERNEL_STACK_SIZE in context_switch_new.c (16 KiB).
 * context_switch_new.c uses this constant to compute the TSS RSP0 and the
 * initial kernel RSP, so the two files must agree on the stack size. */
#define KERNEL_STACK_SIZE 16384

// Compile-time guards: if these fail, update the hardcoded offsets in
// enter_user_trampoline.asm and context_switch_new.c to match.
_Static_assert(offsetof(process_t, cr3)     == 248,
    "process_t::cr3 offset changed - update enter_user_trampoline.asm [rax+248]");
_Static_assert(offsetof(process_t, context) == 168,
    "process_t::context offset changed - update context_switch_new.c");
_Static_assert(offsetof(process_t, fpu_state) == 240,
    "process_t::fpu_state offset changed - update context_switch_new.c");

// External functions
extern uint64_t read_cr3(void);
extern void amd64_syscall_set_kernel_stack(uint64_t stack_top);
extern int exec_run(const char *args, int wait_for_exit);
extern char *itoa(int value, char *str, int base);
extern void process_subsystem_init(void);
extern void exit_subsystem_init(void);

/* new POSIX process subsystem initializer */
/* process_subsystem_init is no longer used */

// Idle process entry point
// Based on Linux arch/x86/kernel/process.c - default_idle()
static void idle_process(void) {
    com_write_string(COM1_PORT, "[IDLE] Idle process running\n");
    for (;;) {
        // Linux idle loop:
        // 1. Check if there's work to do (we do this in scheduler)
        // 2. If not, enable interrupts and halt
        // 3. On interrupt, check again
        
        // safe_halt() - atomically enable interrupts and halt
        // This is what Linux does: sti; hlt in one instruction sequence
        __asm__ volatile("sti; hlt" ::: "memory");
        
        // After waking from interrupt, check if we should schedule
        // In Linux, this happens in the interrupt handler via need_resched flag
        // For now, we just loop - scheduler will be called from timer IRQ
    }
}

// Initialize process management subsystem
void process_management_init(void) {
    com_write_string(COM1_PORT, "[PROCESS] Initializing new process management system\n");

    // Initialize v_NTOSIUX_human process subsystem (tables, locks, scheduler)
    process_subsystem_init();

    // Initialize exit subsystem locks
    exit_subsystem_init();

    // Create idle process (PID 0)
    process_t *idle = process_alloc();
    if (!idle) {
        com_write_string(COM1_PORT, "[PROCESS] Failed to create idle process\n");
        return;
    }
    
    // Override PID to be 0 and fix up the process table slot (same as before)
    uint32_t alloc_pid = idle->pid;
    if (alloc_pid < MAX_PROCESSES && process_table[alloc_pid] == idle)
        process_table[alloc_pid] = NULL;
    idle->pid = 0;
    process_table[0] = idle;
    idle->ppid = 0;
    idle->pgid = 0;
    idle->sid = 0;
    idle->controlling_tty = -1;  // No controlling terminal
    strncpy(idle->name, "idle", PROCESS_NAME_MAX - 1);
    
    // Allocate kernel stack
    idle->kernel_stack = kmalloc(KERNEL_STACK_SIZE);
    if (!idle->kernel_stack) {
        com_write_string(COM1_PORT, "[PROCESS] Failed to allocate idle stack\n");
        process_free(idle);
        return;
    }
    memset(idle->kernel_stack, 0, KERNEL_STACK_SIZE);
    
    // Set up context
    memset(&idle->context, 0, sizeof(cpu_context_t));
    idle->context.rip = (uint64_t)idle_process;
    idle->context.rsp = (uint64_t)idle->kernel_stack + KERNEL_STACK_SIZE - 16;
    idle->context.rflags = 0x202;  // IF=1
    
    // Use current CR3 (kernel page table)
    idle->cr3 = read_cr3();

    // Inherit the boot drive slot so child processes (automan, shell, etc.)
    // get a valid current_slot when process_create_with_args() copies it.
    extern int boot_drive_slot;
    idle->current_slot = boot_drive_slot;
    idle->cwd[0] = '/';
    idle->cwd[1] = 0;

    // Scheduler parameters
    idle->nice = 0;
    idle->weight = 1024;
    idle->vruntime = 0;
    idle->state = PROCESS_STATE_RUNNING;
    
    // Set as current process — use set_curproc() to keep both the new-system
    // 'current' pointer AND the legacy 'current_process' pointer in sync.
    // If 'current = idle' is used directly, process_create_with_args() sees
    // current_process == NULL and assigns current_slot = -1 to all new processes.
    extern void set_curproc(process_t *p);
    set_curproc(idle);

    // Set up kernel stack for interrupts/syscalls
    uint64_t kstack_top = (uint64_t)idle->kernel_stack + KERNEL_STACK_SIZE - 16;
    amd64_syscall_set_kernel_stack(kstack_top);
    
    com_write_string(COM1_PORT, "[PROCESS] Idle process created (PID 0)\n");
    com_write_string(COM1_PORT, "[PROCESS] Process management initialized\n");
}

// Create init process (PID 1) - first user process.
// exec_run() handles process allocation, ELF loading, and scheduler insertion.
void create_init_process(const char *path) {
    com_write_string(COM1_PORT, "[PROCESS] Creating init process\n");

    int pid = exec_run(path, 0);
    if (pid < 0) {
        com_write_string(COM1_PORT, "[PROCESS] Failed to exec init\n");
        return;
    }

    com_write_string(COM1_PORT, "[PROCESS] Init process created (PID ");
    char buf[16];
    itoa(pid, buf, 10);
    com_write_string(COM1_PORT, buf);
    com_write_string(COM1_PORT, ")\n");
}
