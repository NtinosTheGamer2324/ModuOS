#include "moduos/arch/AMD64/cpu_features.h"
#include "moduos/kernel/memory/string.h"

static inline void cpuid(uint32_t leaf, uint32_t subleaf, uint32_t *eax,
                          uint32_t *ebx, uint32_t *ecx, uint32_t *edx) {
    __asm__ volatile("cpuid"
                      : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                      : "a"(leaf), "c"(subleaf));
}

static inline uint32_t cpuid_max_leaf(void) {
    uint32_t eax, ebx, ecx, edx;
    cpuid(0, 0, &eax, &ebx, &ecx, &edx);
    return eax;
}

static inline uint32_t cpuid_max_ext_leaf(void) {
    uint32_t eax, ebx, ecx, edx;
    cpuid(0x80000000, 0, &eax, &ebx, &ecx, &edx);
    return eax;
}

static inline uint64_t read_cr4(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr4, %0" : "=r"(v));
    return v;
}

static inline void write_cr4(uint64_t v) {
    __asm__ volatile("mov %0, %%cr4" ::"r"(v) : "memory");
}

/* Valid only once CR4.OSXSAVE is set. */
static inline uint64_t xgetbv(uint32_t index) {
    uint32_t lo, hi;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(index));
    return ((uint64_t)hi << 32) | lo;
}

static inline void xsetbv(uint32_t index, uint64_t value) {
    uint32_t lo = (uint32_t)value;
    uint32_t hi = (uint32_t)(value >> 32);
    __asm__ volatile("xsetbv" ::"a"(lo), "d"(hi), "c"(index));
}

#define CR4_OSFXSR     (1u << 9)
#define CR4_OSXMMEXCPT (1u << 10)
#define CR4_OSXSAVE    (1u << 18)

#define XCR0_X87       (1u << 0)
#define XCR0_SSE       (1u << 1)
#define XCR0_AVX       (1u << 2)
#define XCR0_OPMASK    (1u << 5)
#define XCR0_ZMM_HI256 (1u << 6)
#define XCR0_HI16_ZMM  (1u << 7)

void cpu_detect_features(cpu_features_t *out) {
    memset(out, 0, sizeof(*out));

    uint32_t eax, ebx, ecx, edx;
    uint32_t max_leaf = cpuid_max_leaf();

    cpuid(0, 0, &eax, &ebx, &ecx, &edx);
    memcpy(out->vendor + 0, &ebx, 4);
    memcpy(out->vendor + 4, &edx, 4);
    memcpy(out->vendor + 8, &ecx, 4);
    out->vendor[12] = '\0';

    if (max_leaf >= 1) {
        cpuid(1, 0, &eax, &ebx, &ecx, &edx);
        out->ecx_01 = ecx;
        out->edx_01 = edx;

        out->fpu     = edx & (1u << 0);
        out->mmx     = edx & (1u << 23);
        out->fxsr    = edx & (1u << 24);
        out->sse     = edx & (1u << 25);
        out->sse2    = edx & (1u << 26);

        out->sse3    = ecx & (1u << 0);
        out->ssse3   = ecx & (1u << 9);
        out->fma     = ecx & (1u << 12);
        out->sse4_1  = ecx & (1u << 19);
        out->sse4_2  = ecx & (1u << 20);
        out->popcnt  = ecx & (1u << 23);
        out->aes     = ecx & (1u << 25);
        out->xsave   = ecx & (1u << 26);
        out->osxsave = ecx & (1u << 27);
        out->avx     = ecx & (1u << 28);
        out->rdrand  = ecx & (1u << 30);
    }

    if (max_leaf >= 7) {
        cpuid(7, 0, &eax, &ebx, &ecx, &edx);
        out->ebx_07 = ebx;
        out->ecx_07 = ecx;
        out->edx_07 = edx;

        out->avx2    = ebx & (1u << 5);
        out->avx512f = ebx & (1u << 16);
    }

    uint32_t max_ext = cpuid_max_ext_leaf();
    if (max_ext >= 0x80000001) {
        cpuid(0x80000001, 0, &eax, &ebx, &ecx, &edx);
        out->edx_81 = edx;
        out->nx     = edx & (1u << 20);
        out->sse4a  = ecx & (1u << 6);
    }

    if (max_ext >= 0x80000004) {
        uint32_t *b = (uint32_t *)out->brand;
        cpuid(0x80000002, 0, &b[0], &b[1], &b[2], &b[3]);
        cpuid(0x80000003, 0, &b[4], &b[5], &b[6], &b[7]);
        cpuid(0x80000004, 0, &b[8], &b[9], &b[10], &b[11]);
        out->brand[48] = '\0';
    }
}

int cpu_enable_features(const cpu_features_t *caps) {
    uint64_t cr4;

    /* All SSE generations share a single OS-enable gate; there is no
     * per-generation bit. */
    if (caps->fxsr || caps->sse || caps->sse2 || caps->sse3 ||
        caps->ssse3 || caps->sse4_1 || caps->sse4_2 || caps->sse4a) {
        cr4 = read_cr4();
        cr4 |= CR4_OSFXSR | CR4_OSXMMEXCPT;
        write_cr4(cr4);

        if ((read_cr4() & (CR4_OSFXSR | CR4_OSXMMEXCPT)) !=
            (CR4_OSFXSR | CR4_OSXMMEXCPT)) {
            return CPU_ENABLE_ERR_CR4_FXSR;
        }
    }

    /* OSXSAVE gates XGETBV/XSETBV existing at all; must precede any
     * XCR0 access. */
    if (caps->xsave) {
        cr4 = read_cr4();
        cr4 |= CR4_OSXSAVE;
        write_cr4(cr4);

        if (!(read_cr4() & CR4_OSXSAVE)) {
            return CPU_ENABLE_ERR_CR4_XSAVE;
        }

        uint64_t xcr0 = xgetbv(0);
        xcr0 |= XCR0_X87 | XCR0_SSE;

        if (caps->avx) {
            xcr0 |= XCR0_AVX;
        }
        if (caps->avx512f) {
            xcr0 |= XCR0_OPMASK | XCR0_ZMM_HI256 | XCR0_HI16_ZMM;
        }

        xsetbv(0, xcr0);

        if (xgetbv(0) != xcr0) {
            return CPU_ENABLE_ERR_XCR0;
        }
    }

    /* MMX, POPCNT, AES, RDRAND, FMA carry no separate OS-enable gate.
     * NX is a paging-level control, out of scope here. */

    return CPU_ENABLE_OK;
}