#ifndef _SIGNAL_H
#define _SIGNAL_H

#include "../../include/moduos/kernel/process/signals.h"

/* The kernel's syscall_numbers.h also defines the POSIX signal numbers and
 * SIG_DFL/SIG_IGN/SIG_ERR. Every definition below is token-for-token the same
 * as the kernel's, so including both headers in either order is legal. */

typedef int sig_atomic_t;
typedef void (*sighandler_t)(int);
typedef void (*sigaction_handler_t)(int, siginfo_t *, ucontext_t *);

struct sigaction {
    union {
        sighandler_t sa_handler;
        sigaction_handler_t sa_sigaction;
    };
    uint64_t sa_mask;
    uint32_t sa_flags;
    uint32_t sa_reserved;
    void (*sa_restorer)(void);
};

#ifndef SIG_DFL
#define SIG_DFL ((void (*)(int))0)
#define SIG_IGN ((void (*)(int))1)
#define SIG_ERR ((void (*)(int))-1)
#endif

#define SIGHUP   1
#define SIGINT   2
#define SIGQUIT  3
#define SIGILL   4
#define SIGTRAP  5
#define SIGABRT  6
#define SIGBUS   7
#define SIGFPE   8
#define SIGKILL  9
#define SIGUSR1 10
#define SIGSEGV 11
#define SIGUSR2 12
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15
#define SIGCHLD 17
#define SIGCONT 18
#define SIGSTOP 19
/* SIGTSTP (20) comes from the kernel header; see the SIGFAULT_PAGE note below. */

/* NTOSIUX signals (used by the fault handler), numbers 20-35.
 * NOTE: SIGFAULT_PAGE (20) collides with the kernel's SIGTSTP (20). */
#define SIGFAULT_PAGE            20 /* Invalid memory access (non-present page or protection violation) */
#define SIGFAULT_GENERAL         21 /* General Protection Fault (GPF) */
#define SIGFAULT_INVALID_OPCODE  22 /* Undefined or illegal instruction */
#define SIGFAULT_DIV0            23 /* Integer division by zero or division overflow */
#define SIGFAULT_DEBUG           24 /* Hardware breakpoint or single-step trap */
#define SIGFAULT_ALIGNMENT       25 /* Data alignment check */
#define SIGFAULT_BOUNDS          26 /* Range check failure */
#define SIGFAULT_STACK_SEGMENT   27 /* Stack segment fault */
#define SIGFAULT_FPU_ERROR       28 /* x87 FPU math exception */
#define SIGFAULT_SIMD_ERROR      29 /* SSE/AVX vector math exception */
#define SIGFAULT_MACHINE_CHECK   30 /* Critical hardware failure */
#define SIGFAULT_VIRT_ERROR      31 /* Virtualization-specific exception */
#define SIGFAULT_CP_ERROR        32 /* Control Protection (shadow stack / ROP) */
#define SIGFAULT_NMI             33 /* Non-Maskable Interrupt */
#define SIGFAULT_SEG_NOT_PRESENT 34 /* Segment not present */
#define SIGFAULT_DOUBLE_FAULT    35 /* Nested exception failure */

sighandler_t signal(int sig, sighandler_t handler);
int          sigaction(int sig, const struct sigaction *action,
                       struct sigaction *old_action);
int          raise(int sig);
int          kill(int pid, int sig);

#endif /* _SIGNAL_H */
