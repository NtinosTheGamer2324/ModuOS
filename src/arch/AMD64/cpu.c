#include "moduos/arch/AMD64/cpu.h"
#include "moduos/arch/AMD64/msr.h"

uint64_t amd64_cpu_get_gs_base(void) {
    return rdmsr(MSR_IA32_GS_BASE);
}

void amd64_cpu_set_gs_base(uint64_t base) {
    wrmsr(MSR_IA32_GS_BASE, base);
}

void amd64_cpu_set_kernel_gs_base(uint64_t base) {
    wrmsr(MSR_IA32_KERNEL_GS_BASE, base);
}

cpu_local_t *cpu_local_get(void) {
    cpu_local_t *p;
    __asm__ volatile("movq %%gs:0, %0" : "=r"(p));
    return p;
}
uint64_t get_cpu_id(void) {
    cpu_local_t *local = cpu_local_get();
    return local->cpu_num;
}