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
    if (!user_range_is_mapped((uint64_t)(uintptr_t)user_dst, n)) {
        extern void com_write_string(uint16_t, const char*);
        extern int com_write_hex64(uint16_t, uint64_t);
        com_write_string(0x3F8, "[USERCOPY] ERROR: user range not mapped\n");
        com_write_string(0x3F8, "[USERCOPY]   Address: 0x");
        com_write_hex64(0x3F8, (uint64_t)(uintptr_t)user_dst);
        com_write_string(0x3F8, "\n[USERCOPY]   Size: ");
        char buf[32];
        extern char *itoa(int, char*, int);
        itoa((int)n, buf, 10);
        com_write_string(0x3F8, buf);
        com_write_string(0x3F8, "\n");
        
        // Check current CR3
        uint64_t cr3;
        __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
        com_write_string(0x3F8, "[USERCOPY]   Current CR3: 0x");
        com_write_hex64(0x3F8, cr3);
        com_write_string(0x3F8, "\n");
        
        // Check which page specifically is not mapped
        uint64_t addr = (uint64_t)(uintptr_t)user_dst;
        uint64_t page = addr & ~0xFFFULL;
        extern uint64_t paging_get_pte(uint64_t);
        uint64_t pte = paging_get_pte(page);
        com_write_string(0x3F8, "[USERCOPY]   Page: 0x");
        com_write_hex64(0x3F8, page);
        com_write_string(0x3F8, " PTE: 0x");
        com_write_hex64(0x3F8, pte);
        com_write_string(0x3F8, "\n");
        
        return -2;
    }
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
