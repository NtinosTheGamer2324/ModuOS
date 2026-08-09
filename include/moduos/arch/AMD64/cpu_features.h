#ifndef MODUOS_ARCH_AMD64_CPU_FEATURES_H
#define MODUOS_ARCH_AMD64_CPU_FEATURES_H

#include <stdbool.h>
#include <stdint.h>

/*
 * CPUID-based feature detection and enablement for AMD64.
 *
 * FPU bring-up (CR0.EM/MP/NE, FNINIT) is handled by mdinit and is out
 * of scope here. This unit only gates CR4/XCR0 for SSE-family and
 * AVX/AVX-512 state, and assumes the FPU is already usable.
 *
 * cpu_enable_features() must be called once per logical core (BSP and
 * each AP independently), since CR4 and XCR0 are per-core state.
 */

typedef struct {
    uint32_t ecx_01, edx_01;
    uint32_t ebx_07, ecx_07, edx_07;
    uint32_t edx_81;

    bool fpu;
    bool mmx;
    bool sse;
    bool sse2;
    bool sse3;
    bool ssse3;
    bool sse4_1;
    bool sse4_2;
    bool sse4a;
    bool fxsr;
    bool xsave;
    bool osxsave;
    bool avx;
    bool avx2;
    bool avx512f;
    bool fma;
    bool popcnt;
    bool aes;
    bool rdrand;
    bool nx;

    char vendor[13];
    char brand[49];
} cpu_features_t;

/* Return codes for cpu_enable_features(). Each write is verified via
 * readback, so a non-zero return means the tier genuinely isn't live. */
enum {
    CPU_ENABLE_OK            = 0,
    CPU_ENABLE_ERR_CR4_FXSR  = -1,
    CPU_ENABLE_ERR_CR4_XSAVE = -2,
    CPU_ENABLE_ERR_XCR0      = -3,
};

void cpu_detect_features(cpu_features_t *out);
int cpu_enable_features(const cpu_features_t *caps);

#endif /* MODUOS_ARCH_AMD64_CPU_FEATURES_H */