#include "moduos/kernel/memory/usercopy.h"
#include "moduos/kernel/memory/paging.h"
#include "moduos/kernel/memory/string.h"

/*
 * Minimal user pointer validation.
 * We validate that each page in [addr, addr+n) is mapped and has PFLAG_USER set.
 * This kernel uses a single address space; USER bit is still meaningful.
 */
static int user_range_is_mapped(uint64_t addr, size_t n) {
    if (n == 0) return 1;
    if (addr >= 0x0000800000000000ULL) return 0;
    uint64_t end = addr + (uint64_t)n - 1;
    if (end < addr) return 0;
    if (end >= 0x0000800000000000ULL) return 0;

    uint64_t start_page = addr & ~0xFFFULL;
    uint64_t end_page = end & ~0xFFFULL;

    for (uint64_t v = start_page; v <= end_page; v += 0x1000ULL) {
        uint64_t pte = paging_get_pte(v);
        if (!(pte & PFLAG_PRESENT)) return 0;
        if (!(pte & PFLAG_USER)) return 0;
    }

    return 1;
}

int usercopy_to_user(void *user_dst, const void *kernel_src, size_t n) {
    if (!user_dst || (!kernel_src && n)) return -1;
    if (!user_range_is_mapped((uint64_t)(uintptr_t)user_dst, n)) return -2;
    memcpy(user_dst, kernel_src, n);
    return 0;
}

int usercopy_from_user(void *kernel_dst, const void *user_src, size_t n) {
    if (!kernel_dst || (!user_src && n)) return -1;
    if (!user_range_is_mapped((uint64_t)(uintptr_t)user_src, n)) return -2;
    memcpy(kernel_dst, user_src, n);
    return 0;
}

int usercopy_string_from_user(char *kernel_dst, const char *user_src, size_t max_len) {
    if (!kernel_dst || max_len == 0) return -1;
    kernel_dst[0] = 0;
    if (!user_src) return -1;

    /* Copy byte-by-byte with mapping checks per page boundary. */
    for (size_t i = 0; i + 1 < max_len; i++) {
        if (!user_range_is_mapped((uint64_t)(uintptr_t)(user_src + i), 1)) {
            kernel_dst[i] = 0;
            return -2;
        }
        char c = user_src[i];
        kernel_dst[i] = c;
        if (c == 0) return 0;
    }
    kernel_dst[max_len - 1] = 0;
    return 0;
}
