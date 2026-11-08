/* process_init_new.c - Process management bring-up: idle (PID 0) and init */

#include "moduos/kernel/tty/tty.h"
#include "moduos/kernel/process/process_new.h"
#include "moduos/kernel/memory/kheap.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/COM/com.h"
#include <stddef.h>

/* Must match KERNEL_STACK_SIZE in context_switch_new.c; that file derives
 * TSS RSP0 and the initial kernel RSP from it. */
#define KERNEL_STACK_SIZE 16384

/* Offsets are consumed by assembly (context switch, enter_user_trampoline). */
_Static_assert(offsetof(process_t, context) == 192,
    "process_t::context offset changed");
_Static_assert(offsetof(process_t, fpu_state) == 272,
    "process_t::fpu_state offset changed");
_Static_assert(offsetof(process_t, cr3) == 784,
    "process_t::cr3 offset changed - update enter_user_trampoline.asm [rax+784]");

extern uint64_t read_cr3(void);
extern void amd64_syscall_set_kernel_stack(uint64_t stack_top);
extern int exec_run(const char *args, int wait_for_exit);
extern char *itoa(int value, char *str, int base);
extern void process_subsystem_init(void);
extern void exit_subsystem_init(void);
extern void set_curproc(process_t *p);
extern int boot_drive_slot;
extern uint32_t next_pid;

static void process_idle_entry(void) {
    com_write_string(COM1_PORT, "[IDLE] Idle process running\n");
    for (;;) {
        /* sti must immediately precede hlt so a wakeup IRQ cannot be lost
         * between enabling interrupts and halting. */
        __asm__ volatile("sti; hlt" ::: "memory");
    }
}

void process_management_init(void) {
    com_write_string(COM1_PORT,
        "[PROCESS] Initializing new process management system\n");

    process_subsystem_init();
    exit_subsystem_init();

    process_t *idle = process_alloc();
    if (!idle) {
        com_write_string(COM1_PORT,
            "[PROCESS] Failed to create idle process\n");
        return;
    }

    /* process_alloc() is PID-agnostic and advances next_pid past whatever it
     * hands out. Idle is relocated to slot 0 by hand, so the cursor has to be
     * rewound as well, otherwise init lands on PID 2. Safe without
     * ptable_lock: boot is single-threaded and no other process exists yet. */
    uint32_t alloc_pid = idle->pid;
    if (alloc_pid < MAX_PROCESSES && process_table[alloc_pid] == idle)
        process_table[alloc_pid] = NULL;

    idle->pid = 0;
    process_table[0] = idle;
    next_pid = 1;

    idle->ppid = 0;
    idle->pgid = 0;
    idle->sid = 0;
    idle->controlling_tty = -1;
    strncpy(idle->name, "idle", PROCESS_NAME_MAX - 1);

    idle->kernel_stack = kmalloc(KERNEL_STACK_SIZE);
    if (!idle->kernel_stack) {
        com_write_string(COM1_PORT,
            "[PROCESS] Failed to allocate idle stack\n");
        process_free(idle); /* clears process_table[0] since pid == 0 */
        return;
    }
    memset(idle->kernel_stack, 0, KERNEL_STACK_SIZE);

    memset(&idle->context, 0, sizeof(cpu_context_t));
    idle->context.rip = (uint64_t)process_idle_entry;
    idle->context.rsp = (uint64_t)idle->kernel_stack + KERNEL_STACK_SIZE - 16;
    idle->context.rflags = 0x202;

    idle->cr3 = read_cr3();

    /* Children copy current_slot from their creator; without a valid slot
     * here, automan and the shell would start with current_slot = -1. */
    idle->current_slot = boot_drive_slot;
    idle->cwd[0] = '/';
    idle->cwd[1] = 0;

    idle->nice = 0;
    idle->weight = 1024;
    idle->vruntime = 0;
    idle->state = PROCESS_STATE_RUNNING;

    /* set_curproc() keeps 'current' and the legacy 'current_process' in sync;
     * assigning 'current' directly leaves process_create_with_args() seeing
     * current_process == NULL. */
    set_curproc(idle);

    amd64_syscall_set_kernel_stack(
        (uint64_t)idle->kernel_stack + KERNEL_STACK_SIZE - 16);

    com_write_string(COM1_PORT, "[PROCESS] Idle process created (PID 0)\n");
    com_write_string(COM1_PORT, "[PROCESS] Process management initialized\n");
}

/* exec_run() handles allocation, ELF loading and scheduler insertion. */
process_t *create_init_process(const char *path) {
    com_write_string(COM1_PORT, "[PROCESS] Creating init process\n");

    int pid = exec_run(path, 0);
    if (pid < 0) {
        com_write_string(COM1_PORT, "[PROCESS] Failed to exec init\n");
        return NULL;
    }

    char buf[16];
    itoa(pid, buf, 10);
    com_write_string(COM1_PORT, "[PROCESS] Init process created (PID ");
    com_write_string(COM1_PORT, buf);
    com_write_string(COM1_PORT, ")\n");

    /* do_exit() reparents orphans to PID 1; anything else breaks that. */
    if (pid != 1) {
        com_write_string(COM1_PORT,
            "[PROCESS] WARNING: init is not PID 1\n");
    }

    process_t *init = process_find((uint32_t)pid);
    if (!init) {
        com_write_string(COM1_PORT,
            "[PROCESS] Init created but not found in process table\n");
        return NULL;
    }

    /* PID 1 is the session leader of the system console (VT1). */
    init->sid = 1;
    init->pgid = 1;
    init->controlling_tty = TTY_CTTY_CONSOLE;

    return init;
}