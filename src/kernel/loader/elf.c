// elf.c - With argument passing support

#include "moduos/kernel/loader/elf.h"
#include "moduos/kernel/process/process_new.h"
#include "moduos/kernel/memory/memory.h"
#include "moduos/kernel/memory/paging.h"
#include "moduos/kernel/memory/phys.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/macros.h"

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
    return elf_load_with_args(elf_data, size, entry_point, 0, NULL, NULL, NULL);
}

int elf_load_with_args(const void *elf_data, size_t size, uint64_t *entry_point,
                       int argc, char **argv,
                       uint64_t *out_image_base, uint64_t *out_image_end) {
    if (out_image_base) *out_image_base = 0;
    if (out_image_end)  *out_image_end  = 0;

    if (elf_validate(elf_data) != 0) return -1;

    elf64_ehdr_t *ehdr = (elf64_ehdr_t *)elf_data;

    if (size < sizeof(*ehdr)) {
        COM_LOG_ERROR(COM1_PORT, "ELF too small");
        return -1;
    }
    if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0) {
        COM_LOG_ERROR(COM1_PORT, "ELF has no program headers");
        return -1;
    }

    uint64_t phentsz = ehdr->e_phentsize ? ehdr->e_phentsize
                                         : (uint64_t)sizeof(elf64_phdr_t);
    uint64_t ph_end  = ehdr->e_phoff + (uint64_t)ehdr->e_phnum * phentsz;
    if (ph_end > size || ehdr->e_phoff > size) {
        COM_LOG_ERROR(COM1_PORT, "ELF program header table out of range");
        return -1;
    }

    elf64_phdr_t *phdr = (elf64_phdr_t *)((uint8_t *)elf_data + ehdr->e_phoff);

    com_write_string(COM1_PORT, "[ELF] Loading ");
    char buf[12];
    itoa(ehdr->e_phnum, buf, 10);
    com_write_string(COM1_PORT, buf);
    com_write_string(COM1_PORT, " program headers\n");

    uint64_t img_base = 0;
    uint64_t img_end  = 0;

    /* We must NOT use paging_map_range() (which maps into the current CR3)
     * because during execve the current CR3 is the child's old address space.
     * Instead, copy all segment data directly through phys_to_virt_kernel(),
     * which works regardless of which CR3 is loaded.
     * The only PML4 we touch is build_pml4 (the new process address space). */

    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type != PT_LOAD) continue;

        /* Validate file-backed range. */
        if (phdr[i].p_offset + phdr[i].p_filesz > size) {
            COM_LOG_ERROR(COM1_PORT, "ELF segment file range out of bounds");
            return -1;
        }

        uint64_t vaddr         = phdr[i].p_vaddr;
        uint64_t page_offset   = vaddr & 0xFFFULL;
        uint64_t vaddr_aligned = vaddr & ~0xFFFULL;
        size_t   total_size    = (size_t)(phdr[i].p_memsz + page_offset);
        size_t   aligned_size  = (total_size + 0xFFFULL) & ~0xFFFULL;
        size_t   num_pages     = aligned_size / PAGE_SIZE;

        uint64_t seg_end = (vaddr + phdr[i].p_memsz + 0xFFFULL) & ~0xFFFULL;
        if (img_base == 0 || vaddr_aligned < img_base) img_base = vaddr_aligned;
        if (seg_end > img_end) img_end = seg_end;

        com_write_string(COM1_PORT, "[ELF] Loading segment ");
        itoa(i, buf, 10);
        com_write_string(COM1_PORT, buf);
        com_write_string(COM1_PORT, " at vaddr 0x");
        com_write_hex64(COM1_PORT, vaddr);
        com_write_string(COM1_PORT, "\n");

        com_write_string(COM1_PORT, "[ELF] Need ");
        itoa((int)num_pages, buf, 10);
        com_write_string(COM1_PORT, buf);
        com_write_string(COM1_PORT, " pages, page_offset=0x");
        com_write_hex64(COM1_PORT, page_offset);
        com_write_string(COM1_PORT, "\n");

        /* Allocate physical pages. */
        uint64_t phys_base = phys_alloc_contiguous(num_pages);
        if (!phys_base) {
            COM_LOG_ERROR(COM1_PORT, "Failed to allocate physical pages for segment");
            return -1;
        }

        com_write_string(COM1_PORT, "[ELF] Allocated phys pages at 0x");
        com_write_hex64(COM1_PORT, phys_base);
        com_write_string(COM1_PORT, "\n");

        /* Zero the entire physical allocation through the kernel direct map.
         * This is CR3-independent — phys_to_virt_kernel always works. */
        uint8_t *kva = (uint8_t *)phys_to_virt_kernel(phys_base);
        memset(kva, 0, aligned_size);

        /* Copy file-backed bytes at the correct intra-page offset. */
        memcpy(kva + page_offset,
               (const uint8_t *)elf_data + phdr[i].p_offset,
               phdr[i].p_filesz);

        com_write_string(COM1_PORT, "[ELF] Copied ");
        itoa((int)phdr[i].p_filesz, buf, 10);
        com_write_string(COM1_PORT, buf);
        com_write_string(COM1_PORT, " bytes\n");

        /* Determine final page flags. */
        uint64_t flags = PFLAG_PRESENT | PFLAG_USER;
        if (phdr[i].p_flags & PF_W) flags |= PFLAG_WRITABLE;

        /* Map segment into the process PML4 (build_pml4 = new address space).
         * We map with PFLAG_WRITABLE during execve so the stack-build and
         * dynamic fixups can write; the kernel does not enforce NX on segments
         * yet but that can be tightened later via paging_set_pte. */
        uint64_t *build_pml4 = process_get_build_pml4();
        if (build_pml4) {
            com_write_string(COM1_PORT, "[ELF] Mapping 0x");
            com_write_hex64(COM1_PORT, vaddr_aligned);
            com_write_string(COM1_PORT, " -> 0x");
            com_write_hex64(COM1_PORT, phys_base);
            com_write_string(COM1_PORT, " into build_pml4\n");

            if (paging_map_range_to_pml4(build_pml4, vaddr_aligned,
                                         phys_base, aligned_size,
                                         flags | PFLAG_WRITABLE) != 0) {
                COM_LOG_ERROR(COM1_PORT, "Failed to map segment into process PML4");
                for (size_t p = 0; p < num_pages; p++)
                    phys_ref_dec(phys_base + (uint64_t)p * PAGE_SIZE);
                return -1;
            }
        } else {
            /* No build_pml4 set — fallback for early-boot / kernel ELF loads.
             * Map into the current CR3 as before. */
            com_write_string(COM1_PORT, "[ELF] Mapping 0x");
            com_write_hex64(COM1_PORT, vaddr_aligned);
            com_write_string(COM1_PORT, " -> 0x");
            com_write_hex64(COM1_PORT, phys_base);
            com_write_string(COM1_PORT, " into current CR3 (fallback)\n");

            if (paging_map_range(vaddr_aligned, phys_base,
                                 aligned_size, flags | PFLAG_WRITABLE) != 0) {
                COM_LOG_ERROR(COM1_PORT, "Failed to map segment (fallback)");
                for (size_t p = 0; p < num_pages; p++)
                    phys_ref_dec(phys_base + (uint64_t)p * PAGE_SIZE);
                return -1;
            }

            /* TLB shootdown for fallback path only. */
            __asm__ volatile(
                "mov %%cr3, %%rax\n"
                "mov %%rax, %%cr3\n"
                ::: "rax", "memory"
            );
        }
    }

    *entry_point = ehdr->e_entry;
    if (out_image_base) *out_image_base = img_base;
    if (out_image_end)  *out_image_end  = img_end;

    (void)argc;
    (void)argv;

    COM_LOG_OK(COM1_PORT, "ELF loaded successfully");
    return 0;
}

int elf_get_interp_path(const void *elf_data, size_t size, char *out, size_t out_size) {
    if (!elf_data || size < sizeof(elf64_ehdr_t) || !out || out_size == 0) return -1;
    if (elf_validate(elf_data) != 0) return -1;

    elf64_ehdr_t *ehdr = (elf64_ehdr_t *)elf_data;
    if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0) return 0;

    uint64_t ph_end = ehdr->e_phoff + (uint64_t)ehdr->e_phnum
                                    * (uint64_t)sizeof(elf64_phdr_t);
    if (ph_end > size) return -1;

    elf64_phdr_t *phdr = (elf64_phdr_t *)((uint8_t *)elf_data + ehdr->e_phoff);
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type != PT_INTERP) continue;
        if (phdr[i].p_offset + phdr[i].p_filesz > size) return -1;

        size_t n = (size_t)phdr[i].p_filesz;
        if (n == 0) return -1;
        if (n > out_size) n = out_size;

        memcpy(out, (const uint8_t *)elf_data + phdr[i].p_offset, n);
        out[out_size - 1] = 0;

        for (size_t j = 0; j < out_size; j++) {
            if (out[j] == 0) return 1;
        }
        out[out_size - 1] = 0;
        return 1;
    }
    return 0;
}

int elf_load_process(const char *path, char *const argv[]) {
    COM_LOG_INFO(COM1_PORT, "Loading ELF from file");
    COM_LOG_ERROR(COM1_PORT, "File loading not yet implemented");
    return -1;
}