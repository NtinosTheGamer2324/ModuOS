// multiboot2.c - Fixed version with proper identity mapping

#include "moduos/kernel/memory/memory.h"
#include "moduos/kernel/memory/phys.h"
#include "moduos/kernel/memory/paging.h"  // Make sure this is included
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/COM/com.h"
#include <stdint.h>
#include <stddef.h>

/* Helper function declarations */
static void print_hex64(uint64_t v);
static void print_dec64(uint64_t v);
static int is_page_mapped(uint64_t virt);

/* Early identity map - bootloader already has identity mapping! */
void early_identity_map()
{
    com_write_string(COM1_PORT, "[MEM] Using bootloader's identity mapping for early boot\n");
}

/* Check if a virtual address is already mapped */
static int is_page_mapped(uint64_t virt) {
    uint64_t *pml4 = paging_get_pml4();
    if (!pml4) return 0;
    
    unsigned i4 = (virt >> 39) & 0x1FF;
    unsigned i3 = (virt >> 30) & 0x1FF;
    unsigned i2 = (virt >> 21) & 0x1FF;
    unsigned i1 = (virt >> 12) & 0x1FF;
    
    uint64_t ent4 = pml4[i4];
    if (!(ent4 & PFLAG_PRESENT)) return 0;
    
    uint64_t *pdpt = (uint64_t *)(uintptr_t)(ent4 & ~0xFFFULL);
    uint64_t ent3 = pdpt[i3];
    if (!(ent3 & PFLAG_PRESENT)) return 0;
    
    uint64_t *pd = (uint64_t *)(uintptr_t)(ent3 & ~0xFFFULL);
    uint64_t ent2 = pd[i2];
    if (!(ent2 & PFLAG_PRESENT)) return 0;
    
    uint64_t *pt = (uint64_t *)(uintptr_t)(ent2 & ~0xFFFULL);
    uint64_t ent1 = pt[i1];
    
    return (ent1 & PFLAG_PRESENT) ? 1 : 0;
}

void early_identity_map_all() {
    uint64_t total_frames = phys_total_frames();
    uint64_t free_frames = phys_count_free_frames();
    
    com_write_string(COM1_PORT, "[MEM] Identity mapping RAM...\n");
    com_write_string(COM1_PORT, "[MEM]   Total frames: ");
    print_dec64(total_frames);
    com_write_string(COM1_PORT, "\n[MEM]   Free frames: ");
    print_dec64(free_frames);
    com_write_string(COM1_PORT, "\n");
    
    /* Calculate how much RAM we can actually map
     * We need to leave some frames free for page tables themselves!
     * Reserve at least 100 frames for page table structures
     */
    uint64_t reserve_for_tables = 100;
    if (free_frames < reserve_for_tables) {
        com_write_string(COM1_PORT, "[MEM] ERROR: Not enough free frames for page tables!\n");
        return;
    }
    
    /* Calculate maximum address we should try to map
     * Use total_frames (not free_frames) because already-used frames
     * should already be mapped by bootloader
     */
    uint64_t max_addr = total_frames * PAGE_SIZE;
    
    com_write_string(COM1_PORT, "[MEM] Mapping up to ");
    print_dec64(max_addr / (1024 * 1024));
    com_write_string(COM1_PORT, " MB\n");
    
    uint64_t mapped_count = 0;
    uint64_t skipped_count = 0;
    
    for (uint64_t addr = 0; addr < max_addr; addr += PAGE_SIZE) {
        /* Check if already mapped - if so, skip it */
        if (is_page_mapped(addr)) {
            skipped_count++;
            continue;
        }
        
        /* Check if we have enough free frames left for page tables */
        if (phys_count_free_frames() < reserve_for_tables) {
            com_write_string(COM1_PORT, "[MEM] Stopping early - need to reserve frames for page tables\n");
            com_write_string(COM1_PORT, "[MEM] Mapped up to ");
            print_dec64(addr / (1024 * 1024));
            com_write_string(COM1_PORT, " MB\n");
            break;
        }
        
        /* Try to map - if it fails, that's OK, it means this physical address
         * is already in use or invalid */
        if (paging_map_page(addr, addr, PFLAG_PRESENT | PFLAG_WRITABLE) == 0) {
            mapped_count++;
            
            /* Print progress every 64MB */
            if ((addr % (64 * 1024 * 1024)) == 0 && addr > 0) {
                com_write_string(COM1_PORT, "[MEM] Mapped up to ");
                print_dec64(addr / (1024 * 1024));
                com_write_string(COM1_PORT, " MB\n");
            }
        } else {
            /* Mapping failed - stop here to avoid exhausting memory */
            com_write_string(COM1_PORT, "[MEM] Mapping failed at ");
            print_dec64(addr / (1024 * 1024));
            com_write_string(COM1_PORT, " MB - stopping\n");
            break;
        }
    }
    
    com_write_string(COM1_PORT, "[MEM] Identity mapping complete:\n");
    com_write_string(COM1_PORT, "[MEM]   New mappings: ");
    print_dec64(mapped_count);
    com_write_string(COM1_PORT, " pages (");
    print_dec64((mapped_count * PAGE_SIZE) / (1024 * 1024));
    com_write_string(COM1_PORT, " MB)\n");
    com_write_string(COM1_PORT, "[MEM]   Already mapped: ");
    print_dec64(skipped_count);
    com_write_string(COM1_PORT, " pages (");
    print_dec64((skipped_count * PAGE_SIZE) / (1024 * 1024));
    com_write_string(COM1_PORT, " MB)\n");
    com_write_string(COM1_PORT, "[MEM]   Free frames remaining: ");
    print_dec64(phys_count_free_frames());
    com_write_string(COM1_PORT, "\n");
}

/* Helper implementations */
static void print_hex64(uint64_t v) {
    char hex[] = "0123456789ABCDEF";
    char buf[19];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 16; i++)
        buf[2 + i] = hex[(v >> ((15 - i) * 4)) & 0xF];
    buf[18] = 0;
    com_write_string(COM1_PORT, buf);
}

static void print_dec64(uint64_t v) {
    char tmp[32];
    int pos = 0;
    if (v == 0) { com_write_string(COM1_PORT, "0"); return; }
    while (v > 0 && pos < 31) {
        tmp[pos++] = '0' + (v % 10);
        v /= 10;
    }
    for (int i = pos - 1; i >= 0; i--) {
        char c[2] = { tmp[i], 0 };
        com_write_string(COM1_PORT, c);
    }
}

struct mb2_tag {
    uint32_t type;
    uint32_t size;
};

struct mb2_mmap_entry {
    uint64_t addr;
    uint64_t len;
    uint32_t type;
    uint32_t zero;
};

extern uint8_t _kernel_start;
extern uint8_t _kernel_end;

void memory_init(void *mb2_ptr) {
    if (!mb2_ptr) {
        com_write_string(COM1_PORT, "[MEM] No multiboot2 pointer.\n");
        return;
    }

    uint8_t *base = (uint8_t *)mb2_ptr;
    uint32_t total_size = *(uint32_t *)(base + 0);

    uint8_t *end  = base + total_size;
    uint8_t *tagp = base + 8;

    uint64_t regions[64 * 2];
    size_t   rcount = 0;
    uint64_t total_usable = 0;

    uint64_t kernel_start = (uint64_t)(uintptr_t)&_kernel_start;
    uint64_t kernel_end   = (uint64_t)(uintptr_t)&_kernel_end;
    
    com_write_string(COM1_PORT, "[MEM] Kernel loaded at: ");
    print_hex64(kernel_start);
    com_write_string(COM1_PORT, " - ");
    print_hex64(kernel_end);
    com_write_string(COM1_PORT, "\n");

    while (tagp + sizeof(struct mb2_tag) <= end) {
        struct mb2_tag *tag = (struct mb2_tag *)tagp;

        if (tag->type == 0) break;
        if (tag->size < 8) break;

        if (tag->type == 6) {
            uint32_t entry_size    = *(uint32_t *)(tagp + 8);
            uint32_t entry_version = *(uint32_t *)(tagp + 12);
            (void)entry_version;

            uint8_t *mmap = tagp + 16;
            uint8_t *mend = tagp + tag->size;

            while (mmap + entry_size <= mend && rcount < 64) {
                struct mb2_mmap_entry *e = (struct mb2_mmap_entry *)mmap;

                if (e->type == 1) {
                    uint64_t region_start = e->addr;
                    uint64_t region_end = e->addr + e->len;
                    
                    if (region_end > kernel_start && region_start < kernel_end) {
                        if (region_start < kernel_start) {
                            uint64_t len_before = kernel_start - region_start;
                            if (len_before >= PAGE_SIZE) {
                                regions[rcount * 2 + 0] = region_start;
                                regions[rcount * 2 + 1] = len_before;
                                total_usable += len_before;
                                rcount++;
                            }
                        }
                        
                        if (region_end > kernel_end) {
                            uint64_t start_after = kernel_end;
                            start_after = (start_after + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
                            
                            if (start_after < region_end) {
                                uint64_t len_after = region_end - start_after;
                                if (len_after >= PAGE_SIZE) {
                                    regions[rcount * 2 + 0] = start_after;
                                    regions[rcount * 2 + 1] = len_after;
                                    total_usable += len_after;
                                    rcount++;
                                }
                            }
                        }
                    } else {
                        regions[rcount * 2 + 0] = region_start;
                        regions[rcount * 2 + 1] = e->len;
                        total_usable += e->len;
                        rcount++;
                    }
                }

                mmap += entry_size;
            }
        }

        tagp += (tag->size + 7) & ~7;
    }

    com_write_string(COM1_PORT, "[MEM] Usable region count: ");
    print_dec64(rcount);
    com_write_string(COM1_PORT, "\n");

    for (size_t i = 0; i < rcount; ++i) {
        com_write_string(COM1_PORT, "[MEM]   region ");
        print_dec64(i);
        com_write_string(COM1_PORT, ": addr=");
        print_hex64(regions[i * 2 + 0]);
        com_write_string(COM1_PORT, "  len=");
        print_dec64(regions[i * 2 + 1]);
        com_write_string(COM1_PORT, "\n");
    }

    com_write_string(COM1_PORT, "[MEM] Total usable bytes: ");
    print_dec64(total_usable);
    com_write_string(COM1_PORT, "\n");

    phys_init(total_usable, regions, rcount);
}