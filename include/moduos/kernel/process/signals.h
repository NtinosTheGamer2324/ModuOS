#ifndef SIGNALS_H
#define SIGNALS_H

// Signal numbers (subset of POSIX signals)
#define SIGKILL  9
#define SIGTERM 15
#define SIGCHLD 17

// NTOSIUX Signals (Used by Fault Handler) (NUM 20-35)
#define SIGFAULT_PAGE            20 // Invalid memory access (non-present page or protection violation)
#define SIGFAULT_GENERAL         21 // General Protection Fault (GPF) - privilege or limit violation
#define SIGFAULT_INVALID_OPCODE  22 // CPU encountered an undefined or illegal instruction
#define SIGFAULT_DIV0            23 // Integer division by zero or division overflow
#define SIGFAULT_DEBUG           24 // Hardware breakpoint or single-step trap
#define SIGFAULT_ALIGNMENT       25 // Data alignment check (unaligned memory access on strict archs)
#define SIGFAULT_BOUNDS          26 // Range check failure (BOUND instruction / array index out of bounds)
#define SIGFAULT_STACK_SEGMENT   27 // Stack segment fault (stack limit exceeded or not present)
#define SIGFAULT_FPU_ERROR       28 // x87 Floating-Point Unit math exception (NaN, Underflow, etc.)
#define SIGFAULT_SIMD_ERROR      29 // SSE/AVX vector math exception (Precision/Masked faults)
#define SIGFAULT_MACHINE_CHECK   30 // Critical hardware failure (ECC error, bus timeout, overheating)
#define SIGFAULT_VIRT_ERROR      31 // Virtualization-specific exception (Hypervisor/EPT violation)
#define SIGFAULT_CP_ERROR        32 // Control Protection (Shadow Stack violation / ROP protection)
#define SIGFAULT_NMI             33 // Non-Maskable Interrupt (Hardware-level "Emergency Stop")
#define SIGFAULT_SEG_NOT_PRESENT 34 // Segment not present in memory (GDT/LDT entry issue)
#define SIGFAULT_DOUBLE_FAULT    35 // Nested exception failure (usually fatal for the process)

int send_signal(uint32_t pid, int sig);
uint64_t do_signal(int sig, uint64_t handler);
void check_signals(void);

#endif /*SIGNALS_H*/