// elf.c - With argument passing support

#include "moduos/kernel/loader/elf.h"
#include "moduos/kernel/process/process.h"
#include "moduos/kernel/memory/memory.h"
#include "moduos/kernel/memory/paging.h"
#include "moduos/kernel/memory/phys.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/macros.h"

// Storage for arguments (Windows-style: argc and argv[] on stack)
#define ARG_STACK_BASE 0x7FFFFFFFE000ULL  // Just below user stack
#define ARG_STACK_SIZE 0x2000              // 8KB for arguments

static uint64_t g_saved_entry = 0;
static int g_saved_argc = 0;
static char **g_saved_argv = NULL;

int elf_validate(const void *elf_data) {
    if (!elf_data) return -1;
    
    elf64_ehdr_t *ehdr = (elf64_ehdr_t *)elf_data;
    
    if (ehdr->e_ident[EI_MAG0] != ELFMAG0 ||
        ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
        ehdr->e_ident[EI_MAG2] != ELFMAG2 ||
        ehdr->e_ident[EI_MAG3] != ELFMAG3) {
        COM_LOG_ERROR(COM1_PORT, "Invalid ELF magic number");
        return -1;
    }
    
    if (ehdr->e_ident[EI_CLASS] != ELFCLASS64) {
        COM_LOG_ERROR(COM1_PORT, "Not a 64-bit ELF");
        return -1;
    }
    
    if (ehdr->e_ident[EI_DATA] != ELFDATA2LSB) {
        COM_LOG_ERROR(COM1_PORT, "Not little-endian ELF");
        return -1;
    }
    
    if (ehdr->e_machine != EM_X86_64) {
        COM_LOG_ERROR(COM1_PORT, "Not an x86-64 ELF");
        return -1;
    }
    
    if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN) {
        COM_LOG_ERROR(COM1_PORT, "Not an executable ELF");
        return -1;
    }
    
    return 0;
}

int elf_load(const void *elf_data, size_t size, uint64_t *entry_point) {
    return elf_load_with_args(elf_data, size, entry_point, 0, NULL);
}

int elf_load_with_args(const void *elf_data, size_t size, uint64_t *entry_point, int argc, char **argv) {
    if (elf_validate(elf_data) != 0) {
        return -1;
    }
    
    elf64_ehdr_t *ehdr = (elf64_ehdr_t *)elf_data;
    elf64_phdr_t *phdr = (elf64_phdr_t *)((uint8_t *)elf_data + ehdr->e_phoff);
    
    com_write_string(COM1_PORT, "[ELF] Loading ");
    char buf[12];
    itoa(ehdr->e_phnum, buf, 10);
    com_write_string(COM1_PORT, buf);
    com_write_string(COM1_PORT, " program headers\n");
    
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == PT_LOAD) {
            com_write_string(COM1_PORT, "[ELF] Loading segment ");
            itoa(i, buf, 10);
            com_write_string(COM1_PORT, buf);
            com_write_string(COM1_PORT, " at vaddr 0x");
            
            uint64_t vaddr = phdr[i].p_vaddr;
            for (int j = 15; j >= 0; j--) {
                uint8_t nibble = (vaddr >> (j * 4)) & 0xF;
                char hex = nibble < 10 ? '0' + nibble : 'a' + (nibble - 10);
                com_write_byte(COM1_PORT, hex);
            }
            com_write_string(COM1_PORT, "\n");
            
            // Calculate page-aligned addresses
            uint64_t page_offset = vaddr & 0xFFF;
            uint64_t vaddr_aligned = vaddr & ~0xFFFULL;
            size_t total_size = phdr[i].p_memsz + page_offset;
            size_t aligned_size = (total_size + 0xFFF) & ~0xFFFULL;
            size_t num_pages = aligned_size / PAGE_SIZE;
            
            com_write_string(COM1_PORT, "[ELF] Need ");
            itoa(num_pages, buf, 10);
            com_write_string(COM1_PORT, buf);
            com_write_string(COM1_PORT, " pages, page_offset=0x");
            for (int j = 2; j >= 0; j--) {
                uint8_t nibble = (page_offset >> (j * 4)) & 0xF;
                char hex = nibble < 10 ? '0' + nibble : 'a' + (nibble - 10);
                com_write_byte(COM1_PORT, hex);
            }
            com_write_string(COM1_PORT, "\n");
            
            // Allocate physical pages
            uint64_t phys_base = phys_alloc_contiguous(num_pages);
            if (!phys_base) {
                COM_LOG_ERROR(COM1_PORT, "Failed to allocate physical pages");
                return -1;
            }
            
            com_write_string(COM1_PORT, "[ELF] Allocated phys pages at 0x");
            for (int j = 15; j >= 0; j--) {
                uint8_t nibble = (phys_base >> (j * 4)) & 0xF;
                char hex = nibble < 10 ? '0' + nibble : 'a' + (nibble - 10);
                com_write_byte(COM1_PORT, hex);
            }
            com_write_string(COM1_PORT, "\n");
            
            // Set up page flags
            uint64_t flags = PFLAG_PRESENT | PFLAG_WRITABLE;
            
            /*
             * Safety: if the target virtual range is already mapped (e.g. repeated exec),
             * unmap it first. This prevents collisions with existing mappings, especially
             * when reusing the same process address space template.
             */
            {
                size_t pages = aligned_size / PAGE_SIZE;
                int unmapped_any = 0;
                for (size_t p = 0; p < pages; p++) {
                    uint64_t vcheck = vaddr_aligned + (uint64_t)p * PAGE_SIZE;
                    if (paging_virt_to_phys(vcheck) != 0) {
                        if (!unmapped_any) {
                            com_write_string(COM1_PORT, "[ELF] Warning: target vaddr range already mapped; unmapping first...\n");
                            unmapped_any = 1;
                        }
                        paging_unmap_page(vcheck);
                    }
                }
                if (unmapped_any) {
                    __asm__ volatile(
                        "mov %%cr3, %%rax\n"
                        "mov %%rax, %%cr3\n"
                        ::: "rax", "memory"
                    );
                }
            }

            // Map the pages
            com_write_string(COM1_PORT, "[ELF] Mapping 0x");
            for (int j = 15; j >= 0; j--) {
                uint8_t nibble = (vaddr_aligned >> (j * 4)) & 0xF;
                char hex = nibble < 10 ? '0' + nibble : 'a' + (nibble - 10);
                com_write_byte(COM1_PORT, hex);
            }
            com_write_string(COM1_PORT, " -> 0x");
            for (int j = 15; j >= 0; j--) {
                uint8_t nibble = (phys_base >> (j * 4)) & 0xF;
                char hex = nibble < 10 ? '0' + nibble : 'a' + (nibble - 10);
                com_write_byte(COM1_PORT, hex);
            }
            com_write_string(COM1_PORT, "\n");
            
            if (paging_map_range(vaddr_aligned, phys_base, aligned_size, flags) != 0) {
                COM_LOG_ERROR(COM1_PORT, "Failed to map segment");
                for (size_t p = 0; p < num_pages; p++) {
                    phys_free_frame(phys_base + p * PAGE_SIZE);
                }
                return -1;
            }
            
            // Flush TLB
            __asm__ volatile(
                "mov %%cr3, %%rax\n"
                "mov %%rax, %%cr3\n"
                ::: "rax", "memory"
            );
            
            // Zero and copy data
            uint8_t *dest = (uint8_t *)vaddr_aligned;
            memset(dest, 0, aligned_size);
            
            uint8_t *data_dest = (uint8_t *)vaddr;
            memcpy(data_dest, (uint8_t *)elf_data + phdr[i].p_offset, phdr[i].p_filesz);
            
            com_write_string(COM1_PORT, "[ELF] Copied ");
            itoa(phdr[i].p_filesz, buf, 10);
            com_write_string(COM1_PORT, buf);
            com_write_string(COM1_PORT, " bytes\n");
        }
    }
    
    *entry_point = ehdr->e_entry;
    
    // If we have arguments, set them up
    if (argc > 0 && argv) {
        com_write_string(COM1_PORT, "[ELF] Setting up arguments (argc=");
        itoa(argc, buf, 10);
        com_write_string(COM1_PORT, buf);
        com_write_string(COM1_PORT, ")\n");
        
        // Save for process initialization
        g_saved_entry = ehdr->e_entry;
        g_saved_argc = argc;
        g_saved_argv = argv;
    } else {
        g_saved_argc = 0;
        g_saved_argv = NULL;
    }
    
    COM_LOG_OK(COM1_PORT, "ELF loaded successfully");
    
    return 0;
}

// Call this after process creation to get the saved args
void elf_get_saved_args(int *argc, char ***argv) {
    *argc = g_saved_argc;
    *argv = g_saved_argv;
}

int elf_load_process(const char *path, char *const argv[]) {
    COM_LOG_INFO(COM1_PORT, "Loading ELF from file");
    COM_LOG_ERROR(COM1_PORT, "File loading not yet implemented");
    return -1;
}