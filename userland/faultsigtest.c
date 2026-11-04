#include "libc/libc.h"

static volatile sig_atomic_t handler_called;
static volatile sig_atomic_t handler_received_expected_fault;
static volatile sig_atomic_t handler_adjusted_context;

static void fault_handler(int signal_number, siginfo_t *info,
                          ucontext_t *context) {
    handler_called = 1;
    if (signal_number != SIGFAULT_INVALID_OPCODE || !info || !context)
        return;

    if (info->si_signo == SIGFAULT_INVALID_OPCODE &&
        info->si_trapno == 6 &&
        info->si_rip == context->rip)
        handler_received_expected_fault = 1;

    context->rip += 2;
    handler_adjusted_context = 1;
}

int md_main(long argc, char **argv) {
    (void)argc;
    (void)argv;

    struct sigaction action = {0};
    action.sa_sigaction = fault_handler;
    action.sa_flags = SA_SIGINFO;
    if (sigaction(SIGFAULT_INVALID_OPCODE, &action, NULL) != 0) {
        printf("FAIL: sigaction registration failed (errno=%d)\n", errno);
        return 1;
    }

    printf("Triggering an invalid-opcode fault...\n");
    __asm__ volatile("ud2" ::: "memory");

    if (!handler_called) {
        puts("FAIL: the fault handler was not called");
        return 1;
    }
    if (!handler_received_expected_fault) {
        puts("FAIL: handler received incorrect signal or fault information");
        return 1;
    }
    if (!handler_adjusted_context) {
        puts("FAIL: handler did not edit the saved context");
        return 1;
    }

    puts("PASS: SIGFAULT_INVALID_OPCODE delivered with fault information");
    puts("PASS: edited ucontext resumed execution after the fault");
    return 0;
}
