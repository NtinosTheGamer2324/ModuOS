// signals_new.c - Signal registration and user fault delivery.

#include <stddef.h>
#include "moduos/kernel/process/process_new.h"
#include "moduos/kernel/interrupts/fault.h"
#include "moduos/kernel/memory/paging.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/memory/usercopy.h"
#include "moduos/kernel/process/signals.h"
#include "moduos/kernel/COM/com.h"
#include "moduos/kernel/errno.h"

extern volatile uint64_t g_syscall_entry_rbp;

typedef struct {
    uint64_t restorer;
    siginfo_t info;
    ucontext_t context;
} user_signal_frame_t;

static int signal_user_mapping(uint64_t address, int writable) {
    if (!address || address >= 0x0000800000000000ULL) return 0;
    uint64_t pte = paging_get_pte(address);
    if (!(pte & PFLAG_PRESENT) || !(pte & PFLAG_USER)) return 0;
    return !writable || (pte & PFLAG_WRITABLE);
}

static int signal_frame_writable(uint64_t address, size_t size) {
    if (!address || !size) return 0;
    uint64_t end = address + size - 1;
    if (end < address || end >= 0x0000800000000000ULL) return 0;
    for (uint64_t page = address & ~0xFFFULL;
         page <= (end & ~0xFFFULL); page += 0x1000ULL) {
        uint64_t pte = paging_get_pte(page);
        if (!(pte & PFLAG_PRESENT) || !(pte & PFLAG_USER) ||
            !(pte & PFLAG_WRITABLE))
            return 0;
    }
    return 1;
}

static void signal_capture_context(ucontext_t *context,
                                   const fault_saved_regs_t *regs,
                                   const fault_user_iret_frame_t *frame) {
    context->r8 = regs->r8;
    context->r9 = regs->r9;
    context->r10 = regs->r10;
    context->r11 = regs->r11;
    context->r12 = regs->r12;
    context->r13 = regs->r13;
    context->r14 = regs->r14;
    context->r15 = regs->r15;
    context->rdi = regs->rdi;
    context->rsi = regs->rsi;
    context->rbp = regs->rbp;
    context->rbx = regs->rbx;
    context->rdx = regs->rdx;
    context->rax = regs->rax;
    context->rcx = regs->rcx;
    context->rip = frame->rip;
    context->rsp = frame->rsp;
    context->rflags = frame->rflags;
}

static int signal_enter_user(process_t *process, int sig,
                             const signal_action_t *action,
                             const siginfo_t *info,
                             const ucontext_t *context,
                             fault_saved_regs_t *regs,
                             fault_user_iret_frame_t *iret_frame) {
    if (process->signal_in_handler || action->handler <= SIGNAL_IGNORE ||
        !signal_user_mapping(action->handler, 0) ||
        !signal_user_mapping(action->restorer, 0))
        return 0;

    uint64_t frame_address = (iret_frame->rsp - sizeof(user_signal_frame_t) - 8) & ~0xFULL;
    frame_address += 8;
    if (iret_frame->rsp < sizeof(user_signal_frame_t) + 8 ||
        frame_address + sizeof(user_signal_frame_t) > iret_frame->rsp ||
        !signal_frame_writable(frame_address, sizeof(user_signal_frame_t)))
        return 0;

    user_signal_frame_t user_frame;
    user_frame.restorer = action->restorer;
    user_frame.info = *info;
    user_frame.context = *context;
    if (usercopy_to_user((void *)(uintptr_t)frame_address, &user_frame,
                         sizeof(user_frame)) != 0)
        return 0;

    regs->rdi = (uint64_t)sig;
    regs->rsi = frame_address + offsetof(user_signal_frame_t, info);
    regs->rdx = frame_address + offsetof(user_signal_frame_t, context);
    regs->rax = 0;
    iret_frame->rip = action->handler;
    iret_frame->rsp = frame_address;
    iret_frame->rflags &= ~((1ULL << 8) | (1ULL << 10) |
                            (1ULL << 16) | (1ULL << 18));
    process->signal_in_handler = true;
    return 1;
}

static int fault_signal_number(uint64_t vector) {
    switch (vector) {
        case 0: return SIGFAULT_DIV0;
        case 1:
        case 3: return SIGFAULT_DEBUG;
        case 4: return SIGFAULT_GENERAL;
        case 5: return SIGFAULT_BOUNDS;
        case 6: return SIGFAULT_INVALID_OPCODE;
        case 10: return SIGFAULT_GENERAL;
        case 11: return SIGFAULT_SEG_NOT_PRESENT;
        case 12: return SIGFAULT_STACK_SEGMENT;
        case 13: return SIGFAULT_GENERAL;
        case 14: return SIGFAULT_PAGE;
        case 16: return SIGFAULT_FPU_ERROR;
        case 17: return SIGFAULT_ALIGNMENT;
        case 19: return SIGFAULT_SIMD_ERROR;
        default: return 0;
    }
}

int fault_try_deliver_user_signal(uint64_t vector, uint64_t error_code,
                                  fault_user_iret_frame_t *iret_frame,
                                  fault_saved_regs_t *regs) {
    if (!iret_frame || !regs || (iret_frame->cs & 3) != 3) return 0;
    int sig = fault_signal_number(vector);
    process_t *process = process_get_current();
    if (!sig || !process || !process->is_user || process->signal_in_handler)
        return 0;

    signal_action_t *action = &process->signal_actions[sig];
    if (action->handler <= SIGNAL_IGNORE) return 0;

    ucontext_t context;
    siginfo_t info = {0};
    signal_capture_context(&context, regs, iret_frame);
    info.si_signo = sig;
    info.si_code = SI_KERNEL;
    info.si_trapno = (int32_t)vector;
    info.si_error = error_code;
    info.si_rip = iret_frame->rip;
    info.si_rsp = iret_frame->rsp;
    if (vector == 14)
        __asm__ volatile("mov %%cr2, %0" : "=r"(info.si_addr));

    if (process->signal_fault_reject_repeat) {
        int repeated = process->signal_fault_vector == vector &&
                       process->signal_fault_context.rip == iret_frame->rip &&
                       (vector != 14 ||
                        process->signal_fault_address == info.si_addr);
        process->signal_fault_reject_repeat = false;
        if (repeated) {
            process->signal_fault_active = false;
            return 0;
        }
    }

    int delivered = signal_enter_user(process, sig, action, &info, &context,
                                      regs, iret_frame);
    if (delivered) {
        process->signal_fault_active = true;
        process->signal_fault_vector = vector;
        process->signal_fault_address = info.si_addr;
        process->signal_fault_error = error_code;
        process->signal_fault_context = context;
    }
    return delivered;
}

int send_signal(uint32_t pid, int sig) {
    if (pid == 0 || sig < 0 || sig >= 64) return -EINVAL;

    process_t *p = process_find(pid);
    if (!p || p->state == PROCESS_STATE_UNUSED ||
        p->state == PROCESS_STATE_EMBRYO ||
        p->state == PROCESS_STATE_ZOMBIE)
        return -ESRCH;

    if (sig == 0) return 0;

    p->pending_signals |= (1ULL << sig);
    if (p->state == PROCESS_STATE_SLEEPING) {
        p->state = PROCESS_STATE_RUNNABLE;
        scheduler_add(p);
    }
    return 0;
}

uint64_t do_signal(int sig, uint64_t handler) {
    process_t *p = (process_t *)current;
    if (!p || sig < 1 || sig >= 64 || sig == SIGKILL || sig == SIGSTOP)
        return (uint64_t)-1;

    uint64_t old_handler = p->signal_actions[sig].handler;
    p->signal_actions[sig].handler = handler;
    p->signal_actions[sig].mask = 0;
    p->signal_actions[sig].flags = 0;
    p->signal_actions[sig].restorer = 0;
    return old_handler;
}

int sys_sigaction(int sig, const signal_action_t *user_action,
                  signal_action_t *user_old_action) {
    process_t *p = process_get_current();
    if (!p || sig < 1 || sig >= 64 || sig == SIGKILL || sig == SIGSTOP ||
        (!user_action && !user_old_action))
        return -EINVAL;

    signal_action_t next;
    if (user_action) {
        if (usercopy_from_user(&next, user_action, sizeof(next)) != 0)
            return -EFAULT;
        if (next.flags & ~SA_SIGINFO) return -EINVAL;
        if (next.handler > SIGNAL_IGNORE &&
            (!signal_user_mapping(next.handler, 0) ||
             !signal_user_mapping(next.restorer, 0)))
            return -EINVAL;
    }

    signal_action_t previous = p->signal_actions[sig];
    if (user_old_action &&
        usercopy_to_user(user_old_action, &previous, sizeof(previous)) != 0)
        return -EFAULT;
    if (user_action) p->signal_actions[sig] = next;
    return 0;
}

uint64_t sys_rt_sigreturn(void) {
    process_t *p = process_get_current();
    if (!p || !p->signal_in_handler) return (uint64_t)-EINVAL;

    uint64_t *saved = (uint64_t *)(uintptr_t)g_syscall_entry_rbp;
    if (!saved || saved[18] < sizeof(uint64_t))
        return (uint64_t)-EINVAL;
    uint64_t user_frame_address = saved[18] - sizeof(uint64_t);

    ucontext_t context;
    uint64_t context_address = user_frame_address +
                               offsetof(user_signal_frame_t, context);
    if (context_address < user_frame_address ||
        usercopy_from_user(&context, (const void *)(uintptr_t)context_address,
                           sizeof(context)) != 0)
        return (uint64_t)-EFAULT;

    if (!context.rsp || !signal_user_mapping(context.rip, 0) ||
        !signal_user_mapping(context.rsp - 1, 0))
        return (uint64_t)-EINVAL;

    p->signal_fault_reject_repeat = false;
    if (p->signal_fault_active &&
        p->signal_fault_context.rip == context.rip) {
        if (p->signal_fault_vector == 14) {
            uint64_t pte = paging_get_pte(p->signal_fault_address);
            int write_fault = (p->signal_fault_error & (1ULL << 1)) != 0;
            int reserved_fault =
                (p->signal_fault_error & (1ULL << 3)) != 0;
            int resolved = !reserved_fault &&
                           (pte & PFLAG_PRESENT) && (pte & PFLAG_USER) &&
                           (!write_fault || (pte & PFLAG_WRITABLE));
            p->signal_fault_reject_repeat = !resolved;
        } else {
            p->signal_fault_reject_repeat =
                memcmp(&context, &p->signal_fault_context,
                       sizeof(context)) == 0;
        }
    }
    p->signal_fault_active = false;

    saved[1] = context.rbx;
    saved[2] = context.rcx;
    saved[3] = context.rdx;
    saved[4] = context.rsi;
    saved[5] = context.rdi;
    saved[6] = context.rbp;
    saved[7] = context.r8;
    saved[8] = context.r9;
    saved[9] = context.r10;
    saved[10] = context.r11;
    saved[11] = context.r12;
    saved[12] = context.r13;
    saved[13] = context.r14;
    saved[14] = context.r15;
    saved[15] = context.rip;
    saved[16] = 0x2B;
    saved[17] = (context.rflags & 0x0000000000240FD5ULL) | 0x202ULL;
    saved[18] = context.rsp;
    saved[19] = 0x23;
    p->signal_in_handler = false;
    return context.rax;
}

void check_signals(void) {
    process_t *p = (process_t *)current;
    if (!p || p->signal_in_handler) return;

    uint64_t pending = p->pending_signals & ~p->blocked_signals;
    if (!pending) return;

    for (int sig = 1; sig < 64; sig++) {
        if (!(pending & (1ULL << sig))) continue;
        uint64_t handler = p->signal_actions[sig].handler;
        if (handler == SIGNAL_IGNORE) {
            p->pending_signals &= ~(1ULL << sig);
            continue;
        }

        if (handler == SIGNAL_DEFAULT && sig == SIGCHLD) {
            p->pending_signals &= ~(1ULL << sig);
            continue;
        }
        if (handler == SIGNAL_DEFAULT) {
            p->pending_signals &= ~(1ULL << sig);
            com_write_string(COM1_PORT, "[SIGNAL] Default action - terminating\n");
            do_exit(sig == SIGKILL || sig == SIGTERM ? -sig : 128 + sig);
            return;
        }

        uint64_t *saved = (uint64_t *)(uintptr_t)g_syscall_entry_rbp;
        if (!saved) return;

        fault_saved_regs_t regs = {
            .r15 = saved[14], .r14 = saved[13], .r13 = saved[12],
            .r12 = saved[11], .r11 = saved[10], .r10 = saved[9],
            .r9 = saved[8], .r8 = saved[7], .rbp = saved[6],
            .rdi = saved[5], .rsi = saved[4], .rdx = saved[3],
            .rcx = saved[2], .rbx = saved[1], .rax = saved[0]
        };
        fault_user_iret_frame_t *frame =
            (fault_user_iret_frame_t *)(uintptr_t)(saved + 15);
        ucontext_t context;
        siginfo_t info = {0};
        signal_capture_context(&context, &regs, frame);
        info.si_signo = sig;
        info.si_code = SI_USER;
        info.si_rip = context.rip;
        info.si_rsp = context.rsp;
        if (!signal_enter_user(p, sig, &p->signal_actions[sig], &info,
                               &context, &regs, frame))
            return;

        saved[0] = regs.rax;
        saved[1] = regs.rbx;
        saved[2] = regs.rcx;
        saved[3] = regs.rdx;
        saved[4] = regs.rsi;
        saved[5] = regs.rdi;
        saved[6] = regs.rbp;
        saved[7] = regs.r8;
        saved[8] = regs.r9;
        saved[9] = regs.r10;
        saved[10] = regs.r11;
        saved[11] = regs.r12;
        saved[12] = regs.r13;
        saved[13] = regs.r14;
        saved[14] = regs.r15;
        p->pending_signals &= ~(1ULL << sig);
        return;
    }
}
