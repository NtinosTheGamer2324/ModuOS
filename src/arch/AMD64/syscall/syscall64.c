#include "moduos/arch/AMD64/syscall/syscall64.h"
#include "moduos/arch/AMD64/msr.h"
#include "moduos/arch/AMD64/gdt.h"
#include "moduos/kernel/COM/com.h"
#include "moduos/kernel/macros.h"

extern void syscall64_entry(void);

void amd64_syscall_init(void) {
    /* Enable SYSCALL/SYSRET */
    uint64_t efer = rdmsr(MSR_IA32_EFER);
    efer |= EFER_SCE;
    wrmsr(MSR_IA32_EFER, efer);

    /* STAR:
     * bits 47:32 = kernel CS selector
     * bits 63:48 = user CS selector
     * SS selectors are CS+8.
     *
     * For SYSRET:
     *  CS = (STAR>>48)+16 | 3
     *  SS = (STAR>>48)+8  | 3
     * So we set STAR.user_cs = USER_CS - 16.
     */
    uint64_t star = 0;
    star |= ((uint64_t)KERNEL_CS) << 32;
    /* STAR expects selectors with RPL=0; SYSRET will set RPL=3 automatically. */
    star |= ((uint64_t)((USER_CS - 16) & ~3u)) << 48;
    wrmsr(MSR_IA32_STAR, star);

    /* LSTAR: kernel entry RIP */
    wrmsr(MSR_IA32_LSTAR, (uint64_t)(uintptr_t)syscall64_entry);

    /* FMASK: clear IF (and TF) on entry. We'll re-enable as desired in kernel. */
    wrmsr(MSR_IA32_FMASK, (1ULL << 9) | (1ULL << 8));

    COM_LOG_OK(COM1_PORT, "SYSCALL/SYSRET initialized");
}
