#include "moduos/kernel/sqrm.h"

#include "moduos/kernel/kernel.h" // kernel_get_boot_mount
#include "moduos/kernel/COM/com.h"
#include "moduos/kernel/memory/memory.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/blockdev.h"
#include "moduos/fs/fs.h"
#include "moduos/kernel/dma.h"
#include "moduos/kernel/io/io.h"
#include "moduos/drivers/PCI/pci.h"
#include "moduos/arch/AMD64/interrupts/irq.h"
#include "moduos/arch/AMD64/interrupts/pic.h"

// Minimal ELF64 loader for kernel modules.
// Assumptions for v1:
//  - ELF64, x86_64, ET_DYN
//  - Relocations are applied (DT_REL/DT_RELA and SHT_REL[A] where present)
//  - Module must export sqrm_module_desc (in .symtab)
//  - e_entry points to sqrm_module_init()

#define EI_NIDENT 16
#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define ET_DYN 3
#define EM_X86_64 62
#define PT_LOAD 1
#define PT_DYNAMIC 2

#define DT_NULL    0
#define DT_STRTAB  5
#define DT_SYMTAB  6
#define DT_RELA    7
#define DT_RELASZ  8
#define DT_RELAENT 9
#define DT_STRSZ   10
#define DT_SYMENT  11
#define DT_REL     17
#define DT_RELSZ   18
#define DT_RELENT  19

typedef struct __attribute__((packed)) {
    unsigned char e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} elf64_ehdr_t;

typedef struct __attribute__((packed)) {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} elf64_phdr_t;

typedef struct __attribute__((packed)) {
    uint32_t sh_name;
    uint32_t sh_type;
    uint64_t sh_flags;
    uint64_t sh_addr;
    uint64_t sh_offset;
    uint64_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint64_t sh_addralign;
    uint64_t sh_entsize;
} elf64_shdr_t;

typedef struct __attribute__((packed)) {
    uint32_t st_name;
    unsigned char st_info;
    unsigned char st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
} elf64_sym_t;

typedef struct __attribute__((packed)) {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t  r_addend;
} elf64_rela_t;

typedef struct __attribute__((packed)) {
    uint64_t r_offset;
    uint64_t r_info;
} elf64_rel_t;

typedef struct __attribute__((packed)) {
    int64_t d_tag;
    union {
        uint64_t d_val;
        uint64_t d_ptr;
    } d_un;
} elf64_dyn_t;

#define ELF64_R_SYM(i) ((uint32_t)((i) >> 32))
#define ELF64_R_TYPE(i) ((uint32_t)((i) & 0xFFFFFFFFu))

#define SHT_SYMTAB 2
#define SHT_STRTAB 3
#define SHT_RELA   4

// x86_64 relocation types we support
#define R_X86_64_64       1
#define R_X86_64_GLOB_DAT 6
#define R_X86_64_JUMP_SLOT 7
#define R_X86_64_RELATIVE 8

typedef struct {
    char name[64];
    void *base;
    uint64_t size;
    sqrm_module_desc_t desc;
    sqrm_kernel_api_t api; // stable API table for this module (modules may keep the pointer)
} sqrm_loaded_t;

static sqrm_loaded_t g_loaded[64];
static size_t g_loaded_count = 0;

static int ends_with(const char *s, const char *suffix) {
    if (!s || !suffix) return 0;
    size_t sl = strlen(s);
    size_t su = strlen(suffix);
    if (su > sl) return 0;
    return strcmp(s + (sl - su), suffix) == 0;
}

static int already_loaded(const char *basename) {
    for (size_t i = 0; i < g_loaded_count; i++) {
        if (strcmp(g_loaded[i].name, basename) == 0) return 1;
    }
    return 0;
}

static int sqrm_map_va_to_off(uint64_t va, uint64_t min_v, uint64_t max_v, uint64_t img_sz, uint64_t *out_off) {
    if (!out_off) return -1;
    // Normal case: VA in [min_v, max_v)
    if (va >= min_v && va < max_v) {
        uint64_t off = va - min_v;
        if (off < img_sz) { *out_off = off; return 0; }
        return -2;
    }
    // Some toolchains emit relocations/symbol values as image-relative offsets.
    if (va < img_sz) {
        *out_off = va;
        return 0;
    }
    return -3;
}

static int sqrm_apply_relocations_dynamic(const elf64_ehdr_t *eh, const elf64_phdr_t *ph, uint8_t *image, uint64_t img_sz, uint64_t min_v, uint64_t max_v) {
    if (!eh || !ph || !image) return -1;

    // Find PT_DYNAMIC
    const elf64_phdr_t *dynph = NULL;
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_DYNAMIC) { dynph = &ph[i]; break; }
    }
    if (!dynph) return -2;
    uint64_t dyn_off = 0;
    if (sqrm_map_va_to_off(dynph->p_vaddr, min_v, max_v, img_sz, &dyn_off) != 0) return -3;
    if (dyn_off + dynph->p_memsz > img_sz) return -4;

    const elf64_dyn_t *dyn = (const elf64_dyn_t*)(image + dyn_off);
    size_t dyn_cnt = (size_t)(dynph->p_memsz / sizeof(elf64_dyn_t));

    // Dynamic symbol/string tables (for R_X86_64_64 / GLOB_DAT / JUMP_SLOT)
    uint64_t symtab_va = 0;
    uint64_t strtab_va = 0;
    uint64_t strsz = 0;
    uint64_t syment = sizeof(elf64_sym_t);

    // DT_RELA table
    uint64_t rela_va = 0;
    uint64_t rela_sz = 0;
    uint64_t rela_ent = sizeof(elf64_rela_t);

    // DT_REL table
    uint64_t rel_va = 0;
    uint64_t rel_sz = 0;
    uint64_t rel_ent = sizeof(elf64_rel_t);

    for (size_t i = 0; i < dyn_cnt; i++) {
        if (dyn[i].d_tag == DT_NULL) break;
        if (dyn[i].d_tag == DT_SYMTAB) symtab_va = dyn[i].d_un.d_ptr;
        else if (dyn[i].d_tag == DT_STRTAB) strtab_va = dyn[i].d_un.d_ptr;
        else if (dyn[i].d_tag == DT_STRSZ) strsz = dyn[i].d_un.d_val;
        else if (dyn[i].d_tag == DT_SYMENT) syment = dyn[i].d_un.d_val;
        else if (dyn[i].d_tag == DT_RELA) rela_va = dyn[i].d_un.d_ptr;
        else if (dyn[i].d_tag == DT_RELASZ) rela_sz = dyn[i].d_un.d_val;
        else if (dyn[i].d_tag == DT_RELAENT) rela_ent = dyn[i].d_un.d_val;
        else if (dyn[i].d_tag == DT_REL) rel_va = dyn[i].d_un.d_ptr;
        else if (dyn[i].d_tag == DT_RELSZ) rel_sz = dyn[i].d_un.d_val;
        else if (dyn[i].d_tag == DT_RELENT) rel_ent = dyn[i].d_un.d_val;
    }

    uint64_t base = (uint64_t)image;

    // Resolve dynamic symtab/strtab addresses (optional)
    const elf64_sym_t *dynsyms = NULL;
    const char *dynstr = NULL;
    size_t dynsym_count = 0;

    if (symtab_va && strtab_va && syment == sizeof(elf64_sym_t)) {
        uint64_t sym_off = 0;
        uint64_t str_off = 0;
        if (sqrm_map_va_to_off(symtab_va, min_v, max_v, img_sz, &sym_off) == 0 &&
            sqrm_map_va_to_off(strtab_va, min_v, max_v, img_sz, &str_off) == 0) {
            dynsyms = (const elf64_sym_t*)(image + sym_off);
            dynstr = (const char*)(image + str_off);
            // We don't have DT_HASH/DT_GNU_HASH parsing, so just cap by remaining image.
            dynsym_count = (size_t)((img_sz - sym_off) / sizeof(elf64_sym_t));
            (void)dynstr;
            (void)strsz;
        }
    }

    // Apply RELA (if present)
    if (rela_va && rela_sz) {
        if (rela_ent == sizeof(elf64_rela_t)) {
            uint64_t rela_off = 0;
            if (sqrm_map_va_to_off(rela_va, min_v, max_v, img_sz, &rela_off) == 0 && rela_off + rela_sz <= img_sz) {
                const elf64_rela_t *rela = (const elf64_rela_t*)(image + rela_off);
                size_t n = (size_t)(rela_sz / sizeof(elf64_rela_t));

                for (size_t i = 0; i < n; i++) {
                    uint64_t r_off = rela[i].r_offset;
                    uint32_t r_type = ELF64_R_TYPE(rela[i].r_info);
                    uint32_t r_sym  = ELF64_R_SYM(rela[i].r_info);
                    int64_t addend = rela[i].r_addend;

                    uint64_t where_off = 0;
                    if (sqrm_map_va_to_off(r_off, min_v, max_v, img_sz, &where_off) != 0) continue;
                    if (where_off + sizeof(uint64_t) > img_sz) continue;

                    uint64_t *where = (uint64_t*)(image + where_off);
                    if (r_type == R_X86_64_RELATIVE) {
                        *where = base + (uint64_t)addend;
                    } else if (r_type == R_X86_64_64 || r_type == R_X86_64_GLOB_DAT || r_type == R_X86_64_JUMP_SLOT) {
                        // S + A
                        uint64_t S = 0;
                        if (dynsyms && r_sym < dynsym_count) {
                            uint64_t sym_va = dynsyms[r_sym].st_value;
                            uint64_t so = 0;
                            if (sqrm_map_va_to_off(sym_va, min_v, max_v, img_sz, &so) == 0) {
                                S = (uint64_t)(image + so);
                            }
                        }
                        *where = S + (uint64_t)addend;
                    }
                }
            }
        }
    }

    // Apply REL (if present) - addend is taken from memory at *where
    if (rel_va && rel_sz) {
        if (rel_ent == sizeof(elf64_rel_t)) {
            uint64_t rel_off = 0;
            if (sqrm_map_va_to_off(rel_va, min_v, max_v, img_sz, &rel_off) == 0 && rel_off + rel_sz <= img_sz) {
                const elf64_rel_t *rel = (const elf64_rel_t*)(image + rel_off);
                size_t n = (size_t)(rel_sz / sizeof(elf64_rel_t));

                for (size_t i = 0; i < n; i++) {
                    uint64_t r_off = rel[i].r_offset;
                    uint32_t r_type = ELF64_R_TYPE(rel[i].r_info);
                    uint32_t r_sym  = ELF64_R_SYM(rel[i].r_info);

                    uint64_t where_off = 0;
                    if (sqrm_map_va_to_off(r_off, min_v, max_v, img_sz, &where_off) != 0) continue;
                    if (where_off + sizeof(uint64_t) > img_sz) continue;

                    uint64_t *where = (uint64_t*)(image + where_off);
                    if (r_type == R_X86_64_RELATIVE) {
                        uint64_t addend = *where;
                        *where = base + addend;
                    } else if (r_type == R_X86_64_64 || r_type == R_X86_64_GLOB_DAT || r_type == R_X86_64_JUMP_SLOT) {
                        uint64_t addend = *where;
                        uint64_t S = 0;
                        if (dynsyms && r_sym < dynsym_count) {
                            uint64_t sym_va = dynsyms[r_sym].st_value;
                            uint64_t so = 0;
                            if (sqrm_map_va_to_off(sym_va, min_v, max_v, img_sz, &so) == 0) {
                                S = (uint64_t)(image + so);
                            }
                        }
                        *where = S + addend;
                    }
                }
            }
        }
    }

    // Not an error if none found; some modules may rely on section-header relocations.
    return 0;
}

static int sqrm_apply_relocations(const uint8_t *buf, size_t rd, const elf64_ehdr_t *eh, uint64_t min_v, uint64_t max_v, uint8_t *image, uint64_t img_sz) {
    if (!buf || !eh || !image) return -1;
    if (eh->e_shoff == 0 || eh->e_shnum == 0 || eh->e_shentsize != sizeof(elf64_shdr_t)) return -2;
    if (eh->e_shoff + (uint64_t)eh->e_shnum * sizeof(elf64_shdr_t) > rd) return -3;

    const elf64_shdr_t *sh = (const elf64_shdr_t*)(buf + eh->e_shoff);

    // Find a SHT_SYMTAB (optional, but required for R_X86_64_64)
    const elf64_shdr_t *symtab = NULL;
    const elf64_sym_t *syms = NULL;
    size_t n_syms = 0;

    for (uint16_t i = 0; i < eh->e_shnum; i++) {
        if (sh[i].sh_type == SHT_SYMTAB) {
            symtab = &sh[i];
            break;
        }
    }

    if (symtab) {
        if (symtab->sh_offset + symtab->sh_size > rd) return -4;
        syms = (const elf64_sym_t*)(buf + symtab->sh_offset);
        n_syms = (size_t)(symtab->sh_size / sizeof(elf64_sym_t));
    }

    uint64_t base = (uint64_t)image; // where the image is actually loaded

    // Apply all SHT_RELA sections
    for (uint16_t si = 0; si < eh->e_shnum; si++) {
        if (sh[si].sh_type != SHT_RELA) continue;
        if (sh[si].sh_entsize != sizeof(elf64_rela_t) || sh[si].sh_entsize == 0) continue;
        if (sh[si].sh_offset + sh[si].sh_size > rd) continue;

        const elf64_rela_t *rela = (const elf64_rela_t*)(buf + sh[si].sh_offset);
        size_t n = (size_t)(sh[si].sh_size / sizeof(elf64_rela_t));

        for (size_t i = 0; i < n; i++) {
            uint64_t r_off = rela[i].r_offset;
            uint32_t r_type = ELF64_R_TYPE(rela[i].r_info);
            uint32_t r_sym  = ELF64_R_SYM(rela[i].r_info);
            int64_t addend  = rela[i].r_addend;

            uint64_t img_off = 0;
            if (sqrm_map_va_to_off(r_off, min_v, max_v, img_sz, &img_off) != 0) continue;
            if (img_off + sizeof(uint64_t) > img_sz) continue;

            uint64_t *where = (uint64_t*)(image + img_off);

            if (r_type == R_X86_64_RELATIVE) {
                // B + A
                *where = base + (uint64_t)addend;
            } else if (r_type == R_X86_64_64) {
                // S + A
                if (!syms || r_sym >= n_syms) continue;
                uint64_t sym_va = syms[r_sym].st_value;

                // Default S=0 for undefined/absolute symbols (v1 modules should not rely on externs)
                uint64_t S = 0;

                // If the symbol is defined in the module, it's a VA relative to min_v.
                if (sym_va != 0) {
                    uint64_t sym_off = 0;
                    if (sqrm_map_va_to_off(sym_va, min_v, max_v, img_sz, &sym_off) == 0) {
                        S = (uint64_t)image + sym_off;
                    }
                }

                *where = S + (uint64_t)addend;
            }
        }
    }

    return 0;
}

static int sqrm_find_desc(const uint8_t *buf, size_t rd, const elf64_ehdr_t *eh, uint64_t min_v, const uint8_t *image, sqrm_module_desc_t *out_desc) {
    if (!buf || !eh || !out_desc) return -1;
    if (eh->e_shoff == 0 || eh->e_shnum == 0 || eh->e_shentsize != sizeof(elf64_shdr_t)) return -2;
    if (eh->e_shoff + (uint64_t)eh->e_shnum * sizeof(elf64_shdr_t) > rd) return -3;

    const elf64_shdr_t *sh = (const elf64_shdr_t*)(buf + eh->e_shoff);

    const elf64_shdr_t *symtab = NULL;
    const elf64_shdr_t *strtab = NULL;

    // pick the first SHT_SYMTAB and its linked string table
    for (uint16_t i = 0; i < eh->e_shnum; i++) {
        if (sh[i].sh_type == SHT_SYMTAB) {
            symtab = &sh[i];
            if (symtab->sh_link < eh->e_shnum) {
                strtab = &sh[symtab->sh_link];
            }
            break;
        }
    }

    if (!symtab || !strtab) return -4;
    if (strtab->sh_type != SHT_STRTAB) return -5;
    if (symtab->sh_offset + symtab->sh_size > rd) return -6;
    if (strtab->sh_offset + strtab->sh_size > rd) return -7;

    const char *strings = (const char*)(buf + strtab->sh_offset);
    const elf64_sym_t *syms = (const elf64_sym_t*)(buf + symtab->sh_offset);
    size_t n_syms = (size_t)(symtab->sh_size / sizeof(elf64_sym_t));

    for (size_t i = 0; i < n_syms; i++) {
        uint32_t noff = syms[i].st_name;
        if (noff >= strtab->sh_size) continue;
        const char *nm = strings + noff;
        if (strcmp(nm, SQRM_DESC_SYMBOL) != 0) continue;

        // st_value is virtual address in the loaded image
        uint64_t va = syms[i].st_value;
        if (va < min_v) return -8;
        uint64_t off = va - min_v;
        if (off + sizeof(sqrm_module_desc_t) > 0xFFFFFFFFULL) return -9;

        const sqrm_module_desc_t *d = (const sqrm_module_desc_t*)(image + off);
        *out_desc = *d;
        return 0;
    }

    return -10;
}

static int sqrm_block_get_handle_for_vdrive(int vdrive_id, blockdev_handle_t *out_handle) {
    if (!out_handle) return -1;
    *out_handle = blockdev_get_vdrive_handle(vdrive_id);
    return (*out_handle != BLOCKDEV_INVALID_HANDLE) ? 0 : -2;
}

static void sqrm_build_api(const sqrm_module_desc_t *desc, sqrm_kernel_api_t *out_api) {
    memset(out_api, 0, sizeof(*out_api));
    out_api->abi_version = 1;
    out_api->module_type = desc->type;
    out_api->module_name = desc->name;

    // base always available
    out_api->com_write_string = com_write_string;
    out_api->kmalloc = kmalloc;
    out_api->kfree = kfree;

    // Low-level helpers (needed for hardware drivers)
    out_api->dma_alloc = dma_alloc;
    out_api->dma_free = dma_free;

    out_api->inb = inb;
    out_api->inw = inw;
    out_api->inl = inl;
    out_api->outb = outb;
    out_api->outw = outw;
    out_api->outl = outl;

    out_api->irq_install_handler = irq_install_handler;
    out_api->irq_uninstall_handler = irq_uninstall_handler;
    out_api->pic_send_eoi = pic_send_eoi;

    // blockdev APIs: FS modules only
    if (desc->type == SQRM_TYPE_FS) {
        out_api->block_get_info = blockdev_get_info;
        out_api->block_read = blockdev_read;
        out_api->block_write = blockdev_write;
        out_api->block_get_handle_for_vdrive = sqrm_block_get_handle_for_vdrive;
    }

    // VFS FS-driver registration: FS modules only
    if (desc->type == SQRM_TYPE_FS) {
        out_api->fs_register_driver = fs_register_driver;
    }

    // devfs registration will be wired later (after reserved-path enforcement is finished)

    // audio registration: audio modules only
    if (desc->type == SQRM_TYPE_AUDIO) {
        out_api->audio_register_pcm = audio_register_pcm;
    }
}

static int sqrm_load_one(const char *path, const char *basename, const sqrm_kernel_api_t *unused_api) {
    if (already_loaded(basename)) return 0;

    fs_mount_t *mnt = kernel_get_boot_mount();
    if (!mnt || !mnt->valid) return -1;

    fs_file_info_t st;
    if (fs_stat(mnt, path, &st) != 0 || st.is_directory || st.size < sizeof(elf64_ehdr_t)) {
        return -2;
    }

    uint8_t *buf = (uint8_t*)kmalloc(st.size);
    if (!buf) return -3;
    size_t rd = 0;
    if (fs_read_file(mnt, path, buf, st.size, &rd) != 0 || rd < sizeof(elf64_ehdr_t)) {
        kfree(buf);
        return -4;
    }

    const elf64_ehdr_t *eh = (const elf64_ehdr_t*)buf;
    if (!(eh->e_ident[0] == 0x7F && eh->e_ident[1] == 'E' && eh->e_ident[2] == 'L' && eh->e_ident[3] == 'F')) {
        kfree(buf);
        return -5;
    }
    if (eh->e_ident[4] != ELFCLASS64 || eh->e_ident[5] != ELFDATA2LSB) {
        kfree(buf);
        return -6;
    }
    if (eh->e_type != ET_DYN || eh->e_machine != EM_X86_64) {
        kfree(buf);
        return -7;
    }
    if (eh->e_phoff == 0 || eh->e_phnum == 0 || eh->e_phentsize != sizeof(elf64_phdr_t)) {
        kfree(buf);
        return -8;
    }

    uint64_t min_v = UINT64_MAX;
    uint64_t max_v = 0;

    if (eh->e_phoff + (uint64_t)eh->e_phnum * sizeof(elf64_phdr_t) > rd) {
        kfree(buf);
        return -9;
    }

    const elf64_phdr_t *ph = (const elf64_phdr_t*)(buf + eh->e_phoff);
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        if (ph[i].p_memsz == 0) continue;
        if (ph[i].p_vaddr < min_v) min_v = ph[i].p_vaddr;
        if (ph[i].p_vaddr + ph[i].p_memsz > max_v) max_v = ph[i].p_vaddr + ph[i].p_memsz;
    }

    if (min_v == UINT64_MAX || max_v <= min_v) {
        kfree(buf);
        return -10;
    }

    uint64_t img_sz = max_v - min_v;
    // page-align
    uint64_t img_sz_aligned = (img_sz + 0xFFFULL) & ~0xFFFULL;

    uint8_t *image = (uint8_t*)kmalloc((size_t)img_sz_aligned);
    if (!image) {
        kfree(buf);
        return -11;
    }
    memset(image, 0, (size_t)img_sz_aligned);

    // Copy PT_LOAD segments
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        if (ph[i].p_filesz == 0) continue;
        if (ph[i].p_offset + ph[i].p_filesz > rd) continue;

        uint64_t dst_off = ph[i].p_vaddr - min_v;
        if (dst_off + ph[i].p_filesz > img_sz_aligned) continue;

        memcpy(image + dst_off, buf + ph[i].p_offset, (size_t)ph[i].p_filesz);
    }

    // Apply relocations (needed for pointers in .rodata like desc.name and ops function tables)
    // Prefer PT_DYNAMIC-based relocations (works even if section headers are stripped).
    int rel_dyn_rc = sqrm_apply_relocations_dynamic(eh, ph, image, img_sz_aligned, min_v, max_v);
    if (rel_dyn_rc != 0) {
        com_write_string(COM1_PORT, "[SQRM] note: dynamic relocations rc=");
        char rbuf[16];
        itoa(rel_dyn_rc, rbuf, 10);
        com_write_string(COM1_PORT, rbuf);
        com_write_string(COM1_PORT, " for ");
        com_write_string(COM1_PORT, basename);
        com_write_string(COM1_PORT, "\n");
    }

    // Also attempt section-header based relocations as a fallback/extra coverage.
    int rel_sh_rc = sqrm_apply_relocations(buf, rd, eh, min_v, max_v, image, img_sz_aligned);
    if (rel_sh_rc != 0) {
        com_write_string(COM1_PORT, "[SQRM] note: sh relocations rc=");
        char rbuf2[16];
        itoa(rel_sh_rc, rbuf2, 10);
        com_write_string(COM1_PORT, rbuf2);
        com_write_string(COM1_PORT, " for ");
        com_write_string(COM1_PORT, basename);
        com_write_string(COM1_PORT, "\n");
    }

    // Find and validate module descriptor
    sqrm_module_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    int dr = sqrm_find_desc(buf, rd, eh, min_v, image, &desc);
    if (dr != 0 || desc.abi_version == 0 || !desc.name) {
        com_write_string(COM1_PORT, "[SQRM] Missing/invalid sqrm_module_desc in ");
        com_write_string(COM1_PORT, basename);
        com_write_string(COM1_PORT, "\n");
        kfree(image);
        kfree(buf);
        return -12;
    }

    // Strict: reject unknown module types
    if (desc.type != SQRM_TYPE_FS && desc.type != SQRM_TYPE_DRIVE && desc.type != SQRM_TYPE_USB && desc.type != SQRM_TYPE_AUDIO) {
        com_write_string(COM1_PORT, "[SQRM] Unknown module type in ");
        com_write_string(COM1_PORT, basename);
        com_write_string(COM1_PORT, "\n");
        kfree(image);
        kfree(buf);
        return -12;
    }

    // Call entrypoint as init()
    if (eh->e_entry < min_v || eh->e_entry >= max_v) {
        kfree(image);
        kfree(buf);
        return -13;
    }

    sqrm_module_init_fn init = (sqrm_module_init_fn)(void*)(image + (eh->e_entry - min_v));

    com_write_string(COM1_PORT, "[SQRM] Loading module: ");
    com_write_string(COM1_PORT, basename);
    com_write_string(COM1_PORT, "\n");

    // Allocate a stable per-module capability-gated API table.
    // Modules are allowed to keep the pointer they receive in sqrm_module_init().
    if (g_loaded_count >= (sizeof(g_loaded)/sizeof(g_loaded[0]))) {
        kfree(image);
        kfree(buf);
        return -14;
    }

    size_t slot_idx = g_loaded_count;
    memset(&g_loaded[slot_idx], 0, sizeof(g_loaded[slot_idx]));
    strncpy(g_loaded[slot_idx].name, basename, sizeof(g_loaded[slot_idx].name) - 1);
    g_loaded[slot_idx].name[sizeof(g_loaded[slot_idx].name) - 1] = 0;
    g_loaded[slot_idx].base = image;
    g_loaded[slot_idx].size = img_sz_aligned;
    g_loaded[slot_idx].desc = desc;

    sqrm_build_api(&desc, &g_loaded[slot_idx].api);
    sqrm_kernel_api_t *mod_api = &g_loaded[slot_idx].api;

    // Log module type
    com_write_string(COM1_PORT, "[SQRM] type=");
    char tbuf[16];
    itoa((int)desc.type, tbuf, 10);
    com_write_string(COM1_PORT, tbuf);
    com_write_string(COM1_PORT, " name=");
    com_write_string(COM1_PORT, desc.name);
    com_write_string(COM1_PORT, "\n");

    int rc = init(mod_api);

    com_write_string(COM1_PORT, "[SQRM] init returned: ");
    char tmp[32];
    itoa(rc, tmp, 10);
    com_write_string(COM1_PORT, tmp);
    com_write_string(COM1_PORT, "\n");

    // If init failed, roll back and free image.
    if (rc != 0) {
        // don't increment g_loaded_count; free resources
        kfree(image);
        kfree(buf);
        memset(&g_loaded[slot_idx], 0, sizeof(g_loaded[slot_idx]));
        return rc;
    }

    // Commit loaded entry.
    g_loaded_count++;

    // Keep image resident; free file buffer.
    kfree(buf);
    return 0;
}

int sqrm_load_all(void) {
    fs_mount_t *mnt = kernel_get_boot_mount();
    if (!mnt || !mnt->valid) return -1;

    static sqrm_kernel_api_t api;
    memset(&api, 0, sizeof(api));
    api.abi_version = 1;
    api.com_write_string = com_write_string;
    api.kmalloc = kmalloc;
    api.kfree = kfree;

    fs_dir_t *d = fs_opendir(mnt, SQRM_MODULE_DIR);
    if (!d) {
        com_write_string(COM1_PORT, "[SQRM] No module directory: " SQRM_MODULE_DIR "\n");
        return -2;
    }

    fs_dirent_t ent;
    int loaded_any = 0;
    while (1) {
        int r = fs_readdir(d, &ent);
        if (r <= 0) break;
        if (ent.is_directory) continue;
        if (!ends_with(ent.name, ".sqrm")) continue;

        char full[256];
        full[0] = 0;
        strcat(full, SQRM_MODULE_DIR);
        strcat(full, "/");
        strcat(full, ent.name);

        int lr = sqrm_load_one(full, ent.name, &api);
        if (lr == 0) loaded_any = 1;
    }
    fs_closedir(d);

    if (!loaded_any) {
        com_write_string(COM1_PORT, "[SQRM] No modules loaded\n");
    }

    return 0;
}
