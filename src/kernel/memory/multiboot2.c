//multiboot2.c
#include "moduos/kernel/memory/memory.h"
#include "moduos/kernel/memory/phys.h"
#include "moduos/kernel/memory/paging.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/COM/com.h"
#include <stdint.h>
#include <stddef.h>

/* VGA text mode buffer */
#define VGA_WIDTH 80
#define VGA_HEIGHT 25
static uint16_t* vga_buffer = (uint16_t*)0xB8000;
static int vga_col = 0;
static int vga_row = 0;
static uint8_t vga_color = 0x0F; /* White on black */

/* VGA helper functions */
static void vga_putchar(char c) {
    if (c == '\n') {
        vga_col = 0;
        vga_row++;
        if (vga_row >= VGA_HEIGHT) {
            /* Scroll */
            for (int y = 0; y < VGA_HEIGHT - 1; y++) {
                for (int x = 0; x < VGA_WIDTH; x++) {
                    vga_buffer[y * VGA_WIDTH + x] = vga_buffer[(y + 1) * VGA_WIDTH + x];
                }
            }
            /* Clear last line */
            for (int x = 0; x < VGA_WIDTH; x++) {
                vga_buffer[(VGA_HEIGHT - 1) * VGA_WIDTH + x] = (vga_color << 8) | ' ';
            }
            vga_row = VGA_HEIGHT - 1;
        }
        return;
    }
    
    if (vga_col >= VGA_WIDTH) {
        vga_col = 0;
        vga_row++;
        if (vga_row >= VGA_HEIGHT) {
            vga_row = VGA_HEIGHT - 1;
        }
    }
    
    vga_buffer[vga_row * VGA_WIDTH + vga_col] = (vga_color << 8) | c;
    vga_col++;
}

static void vga_print(const char* str) {
    while (*str) {
        vga_putchar(*str);
        str++;
    }
}

static void vga_clear_screen(void) {
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        vga_buffer[i] = (vga_color << 8) | ' ';
    }
    vga_col = 0;
    vga_row = 0;
}

static void vga_set_color(uint8_t fg, uint8_t bg) {
    vga_color = (bg << 4) | (fg & 0x0F);
}

/* Dual output - both VGA and COM */
static void log_msg(const char* msg) {
    vga_print(msg);
    com_write_string(COM1_PORT, msg);
}

/* Helper function declarations */
static void print_hex64(uint64_t v);
static void print_dec64(uint64_t v);

/* CRITICAL FIX: Validate multiboot pointer before dereferencing */
static int validate_mb2_pointer(void *mb2_ptr) {
    if (!mb2_ptr) {
        log_msg("[MEM] ERROR: NULL multiboot pointer\n");
        return 0;
    }
    
    /* Check if pointer is in reasonable range (below 4GB) */
    uint64_t ptr_val = (uint64_t)(uintptr_t)mb2_ptr;
    if (ptr_val >= 0x100000000ULL) {
        log_msg("[MEM] ERROR: Multiboot pointer above 4GB: ");
        print_hex64(ptr_val);
        log_msg("\n");
        return 0;
    }
    
    /* Check alignment (should be 8-byte aligned) */
    if ((ptr_val & 0x7) != 0) {
        log_msg("[MEM] WARNING: Multiboot pointer not 8-byte aligned\n");
    }
    
    return 1;
}

/* CRITICAL FIX: Add memory barriers and cache flushes for real hardware */
static inline void memory_barrier(void) {
    __asm__ volatile("mfence" ::: "memory");
}

/* Early identity map - bootloader already has identity mapping! */
void early_identity_map()
{
    log_msg("[MEM] Using bootloader's identity mapping\n");
    memory_barrier(); /* CRITICAL: Ensure all memory ops complete */
}

void early_identity_map_all() {
    uint64_t total_frames = phys_total_frames();
    uint64_t free_frames = phys_count_free_frames();
    
    log_msg("[MEM] Identity mapping RAM...\n");
    log_msg("[MEM]   Total frames: ");
    print_dec64(total_frames);
    log_msg("\n[MEM]   Free frames: ");
    print_dec64(free_frames);
    log_msg("\n");
    
    /* CRITICAL FIX: More conservative reserve */
    uint64_t reserve_for_tables = 200;
    if (free_frames < reserve_for_tables) {
        vga_set_color(0x0C, 0x00); /* Red text */
        log_msg("[MEM] ERROR: Not enough free frames!\n");
        vga_set_color(0x0F, 0x00);
        return;
    }
    
    /* Limit mapping to 512MB initially on real hardware */
    uint64_t max_addr = total_frames * PAGE_SIZE;
    uint64_t safe_limit = 512 * 1024 * 1024; /* 512 MB */
    
    if (max_addr > safe_limit) {
        log_msg("[MEM] Limiting to 512MB for stability\n");
        max_addr = safe_limit;
    }
    
    log_msg("[MEM] Mapping up to ");
    print_dec64(max_addr / (1024 * 1024));
    log_msg(" MB\n");
    
    uint64_t mapped_count = 0;
    uint64_t progress_interval = 32 * 1024 * 1024; /* 32MB progress updates */
    
    /* CRITICAL FIX: Start from 64KB instead of 0 to avoid NULL page and BIOS area */
    uint64_t start_addr = 0x10000; /* 64 KB - skip BIOS data area and NULL page */
    
    log_msg("[MEM] Starting identity mapping from ");
    print_hex64(start_addr);
    log_msg(" (skipping low memory)\n");
    
    for (uint64_t addr = start_addr; addr < max_addr; addr += PAGE_SIZE) {
        /* Check reserves every 32MB to avoid too frequent checks */
        if ((addr % progress_interval) == 0) {
            if (phys_count_free_frames() < reserve_for_tables) {
                vga_set_color(0x0E, 0x00); /* Yellow warning */
                log_msg("[MEM] Stopped at ");
                print_dec64(addr / (1024 * 1024));
                log_msg(" MB (reserve limit)\n");
                vga_set_color(0x0F, 0x00);
                break;
            }
            
            /* Progress update */
            vga_set_color(0x0A, 0x00); /* Green for progress */
            log_msg("[MEM] ");
            print_dec64(addr / (1024 * 1024));
            log_msg(" MB, ");
            print_dec64(phys_count_free_frames());
            log_msg(" frames free\n");
            vga_set_color(0x0F, 0x00);
            memory_barrier();
        }
        
        /* Try to map - paging_map_page will detect if already present */
        int result = paging_map_page(addr, addr, PFLAG_PRESENT | PFLAG_WRITABLE);
        
        if (result == 0) {
            mapped_count++;
        }
        /* Note: result != 0 could mean already mapped OR allocation failure
         * We don't distinguish here - just continue trying next pages */
    }
    
    vga_set_color(0x0B, 0x00); /* Cyan for success */
    log_msg("[MEM] Identity mapping complete!\n");
    log_msg("[MEM]   Successfully mapped: ");
    print_dec64((mapped_count * PAGE_SIZE) / (1024 * 1024));
    log_msg(" MB\n");
    log_msg("[MEM]   Free frames remaining: ");
    print_dec64(phys_count_free_frames());
    log_msg("\n");
    vga_set_color(0x0F, 0x00);
    
    /* CRITICAL: Final memory barrier and TLB flush */
    log_msg("[MEM] Performing memory barrier...\n");
    memory_barrier();
    
    log_msg("[MEM] Flushing TLB...\n");
    __asm__ volatile(
        "mov %%cr3, %%rax\n"
        "mov %%rax, %%cr3\n"
        ::: "rax", "memory"
    );
    
    log_msg("[MEM] TLB flush complete!\n");
    log_msg("[MEM] early_identity_map_all() returning...\n");
}

/* Helper implementations */
static void print_hex64(uint64_t v) {
    char hex[] = "0123456789ABCDEF";
    char buf[19];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 16; i++)
        buf[2 + i] = hex[(v >> ((15 - i) * 4)) & 0xF];
    buf[18] = 0;
    log_msg(buf);
}

static void print_dec64(uint64_t v) {
    char tmp[32];
    int pos = 0;
    if (v == 0) { log_msg("0"); return; }
    while (v > 0 && pos < 31) {
        tmp[pos++] = '0' + (v % 10);
        v /= 10;
    }
    for (int i = pos - 1; i >= 0; i--) {
        char c[2] = { tmp[i], 0 };
        log_msg(c);
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
    /* Initialize VGA for visual feedback */
    vga_clear_screen();
    vga_set_color(0x0F, 0x01); /* White on blue header */
    log_msg("=============== ModuOS Memory Init ===============\n");
    vga_set_color(0x0F, 0x00); /* Back to white on black */
    
    /* CRITICAL FIX: Validate pointer first */
    if (!validate_mb2_pointer(mb2_ptr)) {
        vga_set_color(0x0C, 0x00); /* Red error */
        log_msg("[MEM] FATAL: Invalid multiboot pointer!\n");
        log_msg("System halted. Check bootloader.\n");
        __asm__ volatile("cli; hlt");
        return;
    }

    log_msg("[MEM] Multiboot2 pointer: ");
    print_hex64((uint64_t)(uintptr_t)mb2_ptr);
    log_msg("\n");

    /* CRITICAL FIX: Add memory barrier before reading multiboot data */
    memory_barrier();

    uint8_t *base = (uint8_t *)mb2_ptr;
    uint32_t total_size;
    
    /* CRITICAL FIX: Safe read with bounds check */
    __asm__ volatile("" ::: "memory"); /* Compiler barrier */
    total_size = *(volatile uint32_t *)(base + 0);
    
    /* Validate total_size is reasonable */
    if (total_size < 8 || total_size > 0x10000) { /* Max 64KB */
        vga_set_color(0x0C, 0x00); /* Red */
        log_msg("[MEM] ERROR: Invalid MB2 size: ");
        print_hex64(total_size);
        log_msg("\n");
        __asm__ volatile("cli; hlt");
        return;
    }
    
    log_msg("[MEM] MB2 structure size: ");
    print_dec64(total_size);
    log_msg(" bytes\n");

    uint8_t *end  = base + total_size;
    uint8_t *tagp = base + 8;

    uint64_t regions[64 * 2];
    size_t   rcount = 0;
    uint64_t total_usable = 0;

    uint64_t kernel_start = (uint64_t)(uintptr_t)&_kernel_start;
    uint64_t kernel_end   = (uint64_t)(uintptr_t)&_kernel_end;
    
    log_msg("[MEM] Kernel: ");
    print_hex64(kernel_start);
    log_msg(" - ");
    print_hex64(kernel_end);
    log_msg("\n");
    
    vga_set_color(0x0E, 0x00); /* Yellow for parsing */
    log_msg("[MEM] Parsing memory map...\n");
    vga_set_color(0x0F, 0x00);
    
    int tags_found = 0;

    while (tagp + sizeof(struct mb2_tag) <= end) {
        /* CRITICAL FIX: Safe tag read */
        memory_barrier();
        struct mb2_tag *tag = (struct mb2_tag *)tagp;

        if (tag->type == 0) {
            log_msg("[MEM] End tag found\n");
            break;
        }
        
        if (tag->size < 8) {
            vga_set_color(0x0E, 0x00);
            log_msg("[MEM] WARNING: Invalid tag size\n");
            vga_set_color(0x0F, 0x00);
            break;
        }

        tags_found++;

        if (tag->type == 6) { /* Memory map tag */
            vga_set_color(0x0A, 0x00); /* Green */
            log_msg("[MEM] Found memory map tag!\n");
            vga_set_color(0x0F, 0x00);
            
            uint32_t entry_size    = *(uint32_t *)(tagp + 8);
            uint32_t entry_version = *(uint32_t *)(tagp + 12);
            
            log_msg("[MEM]   Entry size: ");
            print_dec64(entry_size);
            log_msg(", Ver: ");
            print_dec64(entry_version);
            log_msg("\n");

            uint8_t *mmap = tagp + 16;
            uint8_t *mend = tagp + tag->size;

            int entry_count = 0;
            while (mmap + entry_size <= mend && rcount < 64) {
                memory_barrier(); /* CRITICAL: Barrier before reading each entry */
                
                struct mb2_mmap_entry *e = (struct mb2_mmap_entry *)mmap;
                entry_count++;

                if (e->type == 1) { /* Available RAM */
                    uint64_t region_start = e->addr;
                    uint64_t region_end = e->addr + e->len;
                    
                    /* Skip if region overlaps kernel */
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
            
            log_msg("[MEM]   Processed ");
            print_dec64(entry_count);
            log_msg(" entries\n");
        }

        tagp += (tag->size + 7) & ~7;
    }

    log_msg("[MEM] Total tags: ");
    print_dec64(tags_found);
    log_msg("\n");

    log_msg("[MEM] Usable regions: ");
    print_dec64(rcount);
    log_msg("\n");

    if (rcount == 0) {
        vga_set_color(0x0C, 0x00); /* Red */
        log_msg("[MEM] FATAL: No usable memory!\n");
        __asm__ volatile("cli; hlt");
        return;
    }

    for (size_t i = 0; i < rcount && i < 5; ++i) { /* Show first 5 regions */
        log_msg("[MEM]   R");
        print_dec64(i);
        log_msg(": ");
        print_hex64(regions[i * 2 + 0]);
        log_msg(" len=");
        print_dec64(regions[i * 2 + 1] / (1024 * 1024));
        log_msg(" MB\n");
    }
    
    if (rcount > 5) {
        log_msg("[MEM]   ... (");
        print_dec64(rcount - 5);
        log_msg(" more)\n");
    }

    vga_set_color(0x0B, 0x00); /* Cyan */
    log_msg("[MEM] Total usable: ");
    print_dec64(total_usable / (1024 * 1024));
    log_msg(" MB\n");
    vga_set_color(0x0F, 0x00);

    /* CRITICAL FIX: Memory barrier before initializing physical allocator */
    memory_barrier();
    
    vga_set_color(0x0E, 0x00); /* Yellow */
    log_msg("[MEM] Initializing physical allocator...\n");
    vga_set_color(0x0F, 0x00);
    
    phys_init(total_usable, regions, rcount);
    
    vga_set_color(0x0A, 0x00); /* Green success */
    log_msg("[MEM] Physical allocator ready!\n");
    vga_set_color(0x0F, 0x00);
}