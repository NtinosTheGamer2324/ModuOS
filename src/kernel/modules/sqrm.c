#include "moduos/kernel/sqrm.h"
#include "moduos/kernel/sqrm_internal.h"

#include "moduos/kernel/kernel.h" // kernel_get_boot_mount
#include "moduos/kernel/COM/com.h"
#include "moduos/kernel/memory/memory.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/blockdev.h"
#include "moduos/drivers/Drive/vDrive.h"
#include "moduos/kernel/errno.h"
#include "moduos/fs/fs.h"
#include "moduos/fs/devfs.h"
#include "moduos/kernel/events/events.h"
#include "moduos/kernel/dma.h"
#include "moduos/kernel/io/io.h"
#include "moduos/drivers/PCI/pci.h"
#include "moduos/arch/AMD64/interrupts/irq.h"
#include "moduos/arch/AMD64/interrupts/pic.h"
#include "moduos/arch/AMD64/interrupts/timer.h"
#include "moduos/kernel/memory/paging.h"
#include "moduos/kernel/memory/usercopy.h"
#include "moduos/kernel/multiboot2.h"
#include "moduos/kernel/io/io.h"
#include "moduos/kernel/spinlock.h"
#include "moduos/drivers/input/input.h"
#include "moduos/kernel/interrupts/hlt_wait.h"
#include "moduos/fs/fd.h"

#include "moduos/kernel/panic.h"

extern sqrm_gpu_device_t *g_active_gpu;
static char g_sqrm_current_module_name[64];
const char *sqrm_get_current_module_name(void) { return g_sqrm_current_module_name; }

int gfx_register_framebuffer_from_sqrm(const sqrm_gpu_device_t *dev);
int gfx_update_framebuffer_from_sqrm(const framebuffer_t *fb);

extern const char *gfx_get_active_driver_name(void);
extern const char *md64api_sqrm_get_smbios_field(int field);
extern uint64_t phys_total_frames(void);
extern uint64_t phys_count_free_frames(void);

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
#define SHT_REL    9
#define SHT_DYNSYM 11

#define SHN_UNDEF 0

// x86_64 relocation types we support
#define R_X86_64_64        1
#define R_X86_64_PC32      2
#define R_X86_64_PLT32     4
#define R_X86_64_GLOB_DAT  6
#define R_X86_64_JUMP_SLOT 7
#define R_X86_64_RELATIVE  8
#define R_X86_64_GOTPCREL  9
#define R_X86_64_32        10
#define R_X86_64_32S       11
#define R_X86_64_GOTPCRELX 41
#define R_X86_64_REX_GOTPCRELX 42

typedef struct {
    char name[64];
    void *base;
    uint64_t size;
    sqrm_module_desc_t desc; // v1 prefix of descriptor (always populated)
    const sqrm_module_desc_v2_t *desc_v2; // non-NULL if abi_version==2
    /* Separately heap-allocated so its address is stable across g_loaded reallocations.
     * Modules store this pointer in their g_api and call through it after init returns,
     * so it must never move. */
    sqrm_kernel_api_t *api;
} sqrm_loaded_t;

// --- Dynamic module registry ---
// Old code used a fixed [64] array which is too small once you have lots of driver modules.
// We keep this dynamically-sized so "load all" can scale with the contents of /System64/md.
static sqrm_loaded_t *g_loaded = NULL;
static size_t g_loaded_count = 0;
static size_t g_loaded_cap = 0;

static int sqrm_loaded_ensure_capacity(size_t need) {
    if (need <= g_loaded_cap) return 0;

    size_t new_cap = (g_loaded_cap == 0) ? 16 : g_loaded_cap;
    while (new_cap < need) {
        // grow exponentially
        if (new_cap > (((size_t)-1) / 2)) return -ENOMEM;
        new_cap *= 2;
    }

    // hard safety cap to avoid pathological OOM (still far > previous 64)
    if (new_cap > 4096) new_cap = 4096;
    if (need > new_cap) return -ENOMEM;

    sqrm_loaded_t *n = (sqrm_loaded_t*)kmalloc(new_cap * sizeof(*n));
    if (!n) return -ENOMEM;
    memset(n, 0, new_cap * sizeof(*n));

    if (g_loaded && g_loaded_count) {
        memcpy(n, g_loaded, g_loaded_count * sizeof(*n));
    }

    if (g_loaded) kfree(g_loaded);
    g_loaded = n;
    g_loaded_cap = new_cap;
    return 0;
}

// Forward declarations (avoid implicit declarations; required for freestanding build)
typedef struct {
    uint32_t is_dyn;
    uint32_t sym;
    uint64_t slot_addr;
} sqrm_got_slot_t;

typedef struct {
    uint64_t target;
    uint64_t tramp;
} sqrm_tramp_entry_t;

typedef struct {
    uint8_t *cursor;      // allocate from high to low
    uint8_t *low_water;   // do not cross into used GOT slots
    uint8_t *limit_high;  // one past end
    sqrm_tramp_entry_t *ents;
    size_t ent_cnt;
    size_t ent_cap;
} sqrm_tramp_ctx_t;

/*
 * Maximum GOT slots per module. 256 is more than sufficient for any real
 * SQRM module (most have <32 external symbols). Reducing from 4096 saves
 * ~28 KB of permanent kernel heap per loaded module.
 */
#define SQRM_GOT_MAX_SLOTS 256u

static int sqrm_got_add_slot(sqrm_got_slot_t *got, size_t *got_cnt, size_t got_max,
                            uint32_t is_dyn, uint32_t sym, uint64_t *got_base) {
    if (!got || !got_cnt || !got_base) return -EINVAL;
    for (size_t i = 0; i < *got_cnt; i++) {
        if (got[i].is_dyn == is_dyn && got[i].sym == sym) return 0;
    }
    if (*got_cnt >= got_max) return -ENOMEM;
    size_t idx = (*got_cnt)++;
    got[idx].is_dyn = is_dyn;
    got[idx].sym = sym;
    got[idx].slot_addr = (uint64_t)(&got_base[idx]);
    got_base[idx] = 0;
    return 0;
}

static int sqrm_apply_relocations_dynamic(const elf64_ehdr_t *eh, const elf64_phdr_t *ph,
                                         uint8_t *image, uint64_t img_sz,
                                         uint64_t min_v, uint64_t max_v,
                                         const sqrm_got_slot_t *got, size_t got_cnt,
                                         sqrm_tramp_ctx_t *tramp);
static int sqrm_apply_relocations(const uint8_t *buf, size_t rd, const elf64_ehdr_t *eh,
                                  uint64_t min_v, uint64_t max_v,
                                  uint8_t *image, uint64_t img_sz,
                                  const sqrm_got_slot_t *got, size_t got_cnt,
                                  sqrm_tramp_ctx_t *tramp);
static int sqrm_find_desc(const uint8_t *buf, size_t rd, const elf64_ehdr_t *eh,
                          uint64_t min_v, const uint8_t *image,
                          sqrm_module_desc_t *out_desc);
static int sqrm_load_one(const char *path, const char *basename, const sqrm_kernel_api_t *unused_api,
                         const char **dep_stack, size_t dep_depth);

// --- SQRM service registry (named API exports) ---
// Used for module->kernel and module->module discovery of subsystem APIs.

typedef struct {
    char name[32];
    char owner[64];
    const void *api_ptr;
    size_t api_size;
} sqrm_service_entry_t;

// --- Dynamic service registry ---
static sqrm_service_entry_t *g_services = NULL;
static size_t g_service_count = 0;
static size_t g_service_cap = 0;

static int sqrm_services_ensure_capacity(size_t need) {
    if (need <= g_service_cap) return 0;

    size_t new_cap = (g_service_cap == 0) ? 16 : g_service_cap;
    while (new_cap < need) {
        if (new_cap > (((size_t)-1) / 2)) return -ENOMEM;
        new_cap *= 2;
    }

    if (new_cap > 4096) new_cap = 4096;
    if (need > new_cap) return -ENOMEM;

    sqrm_service_entry_t *n = (sqrm_service_entry_t*)kmalloc(new_cap * sizeof(*n));
    if (!n) return -ENOMEM;
    memset(n, 0, new_cap * sizeof(*n));

    if (g_services && g_service_count) {
        memcpy(n, g_services, g_service_count * sizeof(*n));
    }

    if (g_services) kfree(g_services);
    g_services = n;
    g_service_cap = new_cap;
    return 0;
}

static int sqrm_service_register_impl(const char *service_name, const void *api_ptr, size_t api_size) {
    if (!service_name || !service_name[0] || !api_ptr || api_size == 0) return -EINVAL;

    // Replace existing service with same name.
    for (size_t i = 0; i < g_service_count; i++) {
        if (strcmp(g_services[i].name, service_name) == 0) {
            strncpy(g_services[i].owner, sqrm_get_current_module_name(), sizeof(g_services[i].owner) - 1);
            g_services[i].owner[sizeof(g_services[i].owner) - 1] = 0;
            g_services[i].api_ptr = api_ptr;
            g_services[i].api_size = api_size;
            return 0;
        }
    }

    int cap_rc = sqrm_services_ensure_capacity(g_service_count + 1);
    if (cap_rc != 0) return cap_rc;

    size_t idx = g_service_count++;
    memset(&g_services[idx], 0, sizeof(g_services[idx]));
    strncpy(g_services[idx].name, service_name, sizeof(g_services[idx].name) - 1);
    g_services[idx].name[sizeof(g_services[idx].name) - 1] = 0;
    strncpy(g_services[idx].owner, sqrm_get_current_module_name(), sizeof(g_services[idx].owner) - 1);
    g_services[idx].owner[sizeof(g_services[idx].owner) - 1] = 0;
    g_services[idx].api_ptr = api_ptr;
    g_services[idx].api_size = api_size;
    return 0;
}

static const void* sqrm_service_get_impl(const char *service_name, size_t *out_size) {
    if (out_size) *out_size = 0;
    if (!service_name || !service_name[0]) return NULL;

    for (size_t i = 0; i < g_service_count; i++) {
        if (strcmp(g_services[i].name, service_name) == 0) {
            if (out_size) *out_size = g_services[i].api_size;
            return g_services[i].api_ptr;
        }
    }

    return NULL;
}

const void* sqrm_service_get_kernel(const char *service_name, size_t *out_size) {
    return sqrm_service_get_impl(service_name, out_size);
}

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

static int already_loaded_by_modname(const char *modname) {
    if (!modname) return 0;
    for (size_t i = 0; i < g_loaded_count; i++) {
        const char *n = g_loaded[i].desc.name;
        if (n && strcmp(n, modname) == 0) return 1;
    }
    return 0;
}

// Load and relocate a module just enough to read its descriptor name.
// Returns 0 and writes out_desc (v1 prefix) on success.
// Load and relocate a module just enough to read its descriptor name.
// Returns 0 and writes a COPY of desc.name into out_name_buf.
static int sqrm_read_desc_name_from_file(const char *path, const char *basename,
                                        sqrm_module_desc_t *out_desc,
                                        char *out_name_buf, size_t out_name_buf_sz) {
    if (!path || !basename || !out_desc || !out_name_buf || out_name_buf_sz == 0) return -1;
    out_name_buf[0] = 0;
    memset(out_desc, 0, sizeof(*out_desc));

    fs_mount_t *mnt = kernel_get_boot_mount();
    if (!mnt || !mnt->valid) return -2;

    fs_file_info_t st;
    if (fs_stat(mnt, path, &st) != 0 || st.is_directory || st.size < sizeof(elf64_ehdr_t)) return -3;

    uint8_t *buf = (uint8_t*)kmalloc(st.size);
    if (!buf) return -4;

    size_t rd = 0;
    if (fs_read_file(mnt, path, buf, st.size, &rd) != 0 || rd < sizeof(elf64_ehdr_t)) {
        kfree(buf);
        return -5;
    }

    const elf64_ehdr_t *eh = (const elf64_ehdr_t*)buf;
    if (!(eh->e_ident[0] == 0x7F && eh->e_ident[1] == 'E' && eh->e_ident[2] == 'L' && eh->e_ident[3] == 'F')) {
        kfree(buf);
        return -6;
    }
    if (eh->e_ident[4] != ELFCLASS64 || eh->e_ident[5] != ELFDATA2LSB) {
        kfree(buf);
        return -7;
    }
    if (eh->e_type != ET_DYN || eh->e_machine != EM_X86_64) {
        kfree(buf);
        return -8;
    }
    if (eh->e_phoff == 0 || eh->e_phnum == 0 || eh->e_phentsize != sizeof(elf64_phdr_t) ||
        eh->e_phoff + (uint64_t)eh->e_phnum * sizeof(elf64_phdr_t) > rd) {
        kfree(buf);
        return -9;
    }

    const elf64_phdr_t *ph = (const elf64_phdr_t*)(buf + eh->e_phoff);

    uint64_t min_v = UINT64_MAX;
    uint64_t max_v = 0;
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
    uint64_t img_sz_aligned = (img_sz + 0xFFFULL) & ~0xFFFULL;

    /* Probe pass: no GOT needed — allocate only the image, not the GOT region. */
    uint8_t *image = (uint8_t*)kmalloc((size_t)img_sz_aligned);
    if (!image) {
        kfree(buf);
        return -11;
    }
    memset(image, 0, (size_t)img_sz_aligned);

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        if (ph[i].p_filesz == 0) continue;
        if (ph[i].p_offset + ph[i].p_filesz > rd) continue;
        uint64_t dst_off = ph[i].p_vaddr - min_v;
        if (dst_off + ph[i].p_filesz > img_sz_aligned) continue;
        memcpy(image + dst_off, buf + ph[i].p_offset, (size_t)ph[i].p_filesz);
    }

    /* Descriptor-name probing does not need GOT support; keep it minimal. */
    (void)sqrm_apply_relocations_dynamic(eh, ph, image, img_sz_aligned, min_v, max_v, NULL, 0, NULL);
    (void)sqrm_apply_relocations(buf, rd, eh, min_v, max_v, image, img_sz_aligned, NULL, 0, NULL);

    sqrm_module_desc_t d;
    memset(&d, 0, sizeof(d));
    int dr = sqrm_find_desc(buf, rd, eh, min_v, image, &d);
    if (dr != 0 || d.abi_version == 0 || !d.name) {
        kfree(image);
        kfree(buf);
        return -12;
    }

    *out_desc = d;
    // Copy name out (descriptor strings live inside `image`).
    {
        const char *n = d.name;
        size_t i = 0;
        for (; i + 1 < out_name_buf_sz && n[i]; i++) out_name_buf[i] = n[i];
        out_name_buf[i] = 0;
    }

    // Replace pointer with the stable copied buffer for the caller.
    out_desc->name = out_name_buf;

    kfree(image);
    kfree(buf);
    return 0;
}

// Find the filename (*.sqrm basename) for a module with given descriptor name.
// Returns 0 and writes basename into out_basename on success.
static int sqrm_find_module_file_by_desc_name(const char *want_name, char *out_basename, size_t out_sz) {
    if (!want_name || !out_basename || out_sz == 0) return -1;

    fs_mount_t *mnt = kernel_get_boot_mount();
    if (!mnt || !mnt->valid) return -2;

    fs_dir_t *d = fs_opendir(mnt, SQRM_MODULE_DIR);
    if (!d) return -3;

    fs_dirent_t ent;
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

        sqrm_module_desc_t dsc;
        char namebuf[64];
        if (sqrm_read_desc_name_from_file(full, ent.name, &dsc, namebuf, sizeof(namebuf)) == 0) {
            if (dsc.name && strcmp(dsc.name, want_name) == 0) {
                strncpy(out_basename, ent.name, out_sz - 1);
                out_basename[out_sz - 1] = 0;
                fs_closedir(d);
                return 0;
            }
        }
    }

    fs_closedir(d);
    return -4;
}


static uint64_t sqrm_got_lookup_slot(const sqrm_got_slot_t *got, size_t got_cnt, uint32_t is_dyn, uint32_t sym) {
    if (!got) return 0;
    for (size_t i = 0; i < got_cnt; i++) {
        if (got[i].is_dyn == is_dyn && got[i].sym == sym) return got[i].slot_addr;
    }
    return 0;
}

// Create/lookup a per-module trampoline for calling an arbitrary 64-bit target.
// We need this because R_X86_64_PLT32 / R_X86_64_PC32 can only encode a 32-bit
// PC-relative displacement (±2GB), while kernel symbols typically live far away
// in the higher half.
//
// Trampoline encoding (14 bytes):
//   FF 25 00 00 00 00      jmp qword ptr [rip+0]
//   <8-byte absolute target address>
// We allocate these trampolines in the module's GOT-adjacent region so they are
// within range of module code.
static uint64_t sqrm_tramp_get(sqrm_tramp_ctx_t *ctx, uint64_t target) {
    if (!ctx || !target) return 0;

    for (size_t i = 0; i < ctx->ent_cnt; i++) {
        if (ctx->ents[i].target == target) return ctx->ents[i].tramp;
    }

    // Ensure cache capacity
    if (ctx->ent_cnt >= ctx->ent_cap) {
        size_t new_cap = (ctx->ent_cap == 0) ? 32 : (ctx->ent_cap * 2);
        sqrm_tramp_entry_t *n = (sqrm_tramp_entry_t*)kmalloc(new_cap * sizeof(*n));
        if (!n) return 0;
        if (ctx->ents && ctx->ent_cnt) memcpy(n, ctx->ents, ctx->ent_cnt * sizeof(*n));
        if (ctx->ents) kfree(ctx->ents);
        ctx->ents = n;
        ctx->ent_cap = new_cap;
    }

    // Allocate 16 bytes for alignment/padding.
    if (ctx->cursor < ctx->low_water + 16) return 0;
    ctx->cursor -= 16;
    uint8_t *t = ctx->cursor;

    // jmp [rip+0]
    t[0] = 0xFF;
    t[1] = 0x25;
    t[2] = 0x00;
    t[3] = 0x00;
    t[4] = 0x00;
    t[5] = 0x00;

    // absolute address
    *(uint64_t*)(t + 6) = target;

    // pad
    t[14] = 0x90;
    t[15] = 0x90;

    uint64_t tramp_va = (uint64_t)(uintptr_t)t;
    ctx->ents[ctx->ent_cnt].target = target;
    ctx->ents[ctx->ent_cnt].tramp = tramp_va;
    ctx->ent_cnt++;

    // Debug: log trampoline creation.
    com_write_string(COM1_PORT, "[SQRM] tramp ");
    com_write_string(COM1_PORT, sqrm_get_current_module_name());
    com_write_string(COM1_PORT, " target=");
    com_write_hex64(COM1_PORT, target);
    com_write_string(COM1_PORT, " tramp=");
    com_write_hex64(COM1_PORT, tramp_va);
    com_write_string(COM1_PORT, "\n");

    return tramp_va;
}

// Resolve well-known kernel symbols for SQRM modules.
// This is needed when modules reference standard C helpers via undefined symbols (SHN_UNDEF)
// and the loader currently does not implement full dynamic linking against a kernel symbol table.
static int sqrm_sym_eq(const char *name, const char *base) {
    if (!name || !base) return 0;
    // Skip leading underscores (toolchain variants)
    while (*name == '_') name++;

    // Compare until end of base, but stop name at '@' (version/plt suffix)
    for (size_t i = 0; base[i]; i++) {
        if (!name[i] || name[i] == '@') return 0;
        if (name[i] != base[i]) return 0;
    }
    // base ended; name must end or hit '@'
    return (name[strlen(base)] == 0 || name[strlen(base)] == '@');
}

static uint64_t sqrm_resolve_kernel_symbol(const char *name) {
    if (!name || !name[0]) return 0;

    // memory
    if (sqrm_sym_eq(name, "memset") || sqrm_sym_eq(name, "__memset_chk")) return (uint64_t)(uintptr_t)&memset;
    if (sqrm_sym_eq(name, "memcpy") || sqrm_sym_eq(name, "__memcpy_chk")) return (uint64_t)(uintptr_t)&memcpy;
    if (sqrm_sym_eq(name, "memmove")) return (uint64_t)(uintptr_t)&memmove;
    if (sqrm_sym_eq(name, "memcmp")) return (uint64_t)(uintptr_t)&memcmp;

    // strings
    if (sqrm_sym_eq(name, "strlen")) return (uint64_t)(uintptr_t)&strlen;
    if (sqrm_sym_eq(name, "strcmp")) return (uint64_t)(uintptr_t)&strcmp;
    if (sqrm_sym_eq(name, "strncmp")) return (uint64_t)(uintptr_t)&strncmp;

    if (strcmp(name, "strcpy") == 0) return (uint64_t)(uintptr_t)&strcpy;
    if (strcmp(name, "strncpy") == 0) return (uint64_t)(uintptr_t)&strncpy;
    if (strcmp(name, "strlcpy") == 0) return (uint64_t)(uintptr_t)&strlcpy;

    if (strcmp(name, "strcat") == 0) return (uint64_t)(uintptr_t)&strcat;
    if (strcmp(name, "strncat") == 0) return (uint64_t)(uintptr_t)&strncat;

    if (strcmp(name, "strchr") == 0) return (uint64_t)(uintptr_t)&strchr;
    if (strcmp(name, "strrchr") == 0) return (uint64_t)(uintptr_t)&strrchr;
    if (strcmp(name, "strstr") == 0) return (uint64_t)(uintptr_t)&strstr;

    // conversion / formatting
    if (strcmp(name, "itoa") == 0) return (uint64_t)(uintptr_t)&itoa;
    if (strcmp(name, "utoa") == 0) return (uint64_t)(uintptr_t)&utoa;
    if (strcmp(name, "atoi") == 0) return (uint64_t)(uintptr_t)&atoi;
    if (strcmp(name, "snprintf") == 0) return (uint64_t)(uintptr_t)&snprintf;
    if (strcmp(name, "str_append") == 0) return (uint64_t)(uintptr_t)&str_append;

    // Debug: log unresolved *mem* symbols only, and only for the HID module.
    // (The loader sees many unresolved module-internal globals like g_api; those are noise and
    // would exhaust any global budget before we reach the interesting failure.)
    {
        const char *m = sqrm_get_current_module_name();
        if (m && strcmp(m, "hid") == 0) {
            if (strstr(name, "mem") != NULL) {
                com_write_string(COM1_PORT, "[SQRM] unresolved ");
                com_write_string(COM1_PORT, m);
                com_write_string(COM1_PORT, " sym=");
                com_write_string(COM1_PORT, name);
                com_write_string(COM1_PORT, "\n");
            }
        }
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

static int sqrm_apply_relocations_dynamic(const elf64_ehdr_t *eh, const elf64_phdr_t *ph,
                                         uint8_t *image, uint64_t img_sz,
                                         uint64_t min_v, uint64_t max_v,
                                         const sqrm_got_slot_t *got, size_t got_cnt,
                                         sqrm_tramp_ctx_t *tramp) {
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
                            // If symbol is undefined (extern), resolve from kernel table.
                            if (dynsyms[r_sym].st_shndx == SHN_UNDEF) {
                                const char *nm = (dynstr && dynsyms[r_sym].st_name < strsz) ? (dynstr + dynsyms[r_sym].st_name) : NULL;
                                S = sqrm_resolve_kernel_symbol(nm);
                                // Debug: did we resolve memset for HID?
                                {
                                    const char *m = sqrm_get_current_module_name();
                                    if (m && strcmp(m, "hid") == 0 && nm && sqrm_sym_eq(nm, "memset")) {
                                        com_write_string(COM1_PORT, "[SQRM] resolve hid ");
                                        com_write_string(COM1_PORT, nm);
                                        com_write_string(COM1_PORT, " -> ");
                                        com_write_hex64(COM1_PORT, S);
                                        com_write_string(COM1_PORT, "\n");
                                    }
                                }
                            } else {
                                uint64_t sym_va = dynsyms[r_sym].st_value;
                                uint64_t so = 0;
                                if (sqrm_map_va_to_off(sym_va, min_v, max_v, img_sz, &so) == 0) {
                                    S = (uint64_t)(image + so);
                                }
                            }
                        }
                        *where = S + (uint64_t)addend;
                    } else if (r_type == R_X86_64_PC32 || r_type == R_X86_64_PLT32) {
                        // S + A - P (32-bit signed)
                        uint64_t S = 0;
                        if (dynsyms && r_sym < dynsym_count) {
                            if (dynsyms[r_sym].st_shndx == SHN_UNDEF) {
                                const char *nm = (dynstr && dynsyms[r_sym].st_name < strsz) ? (dynstr + dynsyms[r_sym].st_name) : NULL;
                                S = sqrm_resolve_kernel_symbol(nm);
                                if (tramp && S) S = sqrm_tramp_get(tramp, S);
                            } else {
                                uint64_t sym_va = dynsyms[r_sym].st_value;
                                uint64_t so = 0;
                                if (sqrm_map_va_to_off(sym_va, min_v, max_v, img_sz, &so) == 0) {
                                    S = (uint64_t)(image + so);
                                }
                            }
                        }
                        uint64_t P = (uint64_t)(image + where_off);
                        int64_t val = (int64_t)S + addend - (int64_t)P;
                        *(int32_t*)where = (int32_t)val;
                    } else if (r_type == R_X86_64_GOTPCREL || r_type == R_X86_64_GOTPCRELX || r_type == R_X86_64_REX_GOTPCRELX) {
                        // G + A - P (32-bit signed)
                        uint64_t G = sqrm_got_lookup_slot(got, got_cnt, 1, r_sym);
                        uint64_t P = (uint64_t)(image + where_off);
                        int64_t val = (int64_t)G + addend - (int64_t)P;
                        *(int32_t*)where = (int32_t)val;
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
                            if (dynsyms[r_sym].st_shndx == SHN_UNDEF) {
                                const char *nm = (dynstr && dynsyms[r_sym].st_name < strsz) ? (dynstr + dynsyms[r_sym].st_name) : NULL;
                                S = sqrm_resolve_kernel_symbol(nm);
                            } else {
                                uint64_t sym_va = dynsyms[r_sym].st_value;
                                uint64_t so = 0;
                                if (sqrm_map_va_to_off(sym_va, min_v, max_v, img_sz, &so) == 0) {
                                    S = (uint64_t)(image + so);
                                }
                            }
                        }
                        *where = S + addend;
                    } else if (r_type == R_X86_64_PC32 || r_type == R_X86_64_PLT32) {
                        int32_t addend = *(int32_t*)where;
                        uint64_t S = 0;
                        if (dynsyms && r_sym < dynsym_count) {
                            if (dynsyms[r_sym].st_shndx == SHN_UNDEF) {
                                const char *nm = (dynstr && dynsyms[r_sym].st_name < strsz) ? (dynstr + dynsyms[r_sym].st_name) : NULL;
                                S = sqrm_resolve_kernel_symbol(nm);
                                if (tramp && S) S = sqrm_tramp_get(tramp, S);
                            } else {
                                uint64_t sym_va = dynsyms[r_sym].st_value;
                                uint64_t so = 0;
                                if (sqrm_map_va_to_off(sym_va, min_v, max_v, img_sz, &so) == 0) {
                                    S = (uint64_t)(image + so);
                                }
                            }
                        }
                        uint64_t P = (uint64_t)(image + where_off);
                        int64_t val = (int64_t)S + (int64_t)addend - (int64_t)P;
                        *(int32_t*)where = (int32_t)val;
                    } else if (r_type == R_X86_64_GOTPCREL || r_type == R_X86_64_GOTPCRELX || r_type == R_X86_64_REX_GOTPCRELX) {
                        int32_t addend = *(int32_t*)where;
                        uint64_t G = sqrm_got_lookup_slot(got, got_cnt, 1, r_sym);
                        uint64_t P = (uint64_t)(image + where_off);
                        int64_t val = (int64_t)G + (int64_t)addend - (int64_t)P;
                        *(int32_t*)where = (int32_t)val;
                    }
                }
            }
        }
    }

    // Not an error if none found; some modules may rely on section-header relocations.
    return 0;
}

static int sqrm_apply_relocations(const uint8_t *buf, size_t rd, const elf64_ehdr_t *eh,
                                  uint64_t min_v, uint64_t max_v,
                                  uint8_t *image, uint64_t img_sz,
                                  const sqrm_got_slot_t *got, size_t got_cnt,
                                  sqrm_tramp_ctx_t *tramp) {
    // got/got_cnt used for GOTPCREL relocations
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

        // Select the correct symbol table for this relocation section (ELF sh_link)
        symtab = NULL;
        syms = NULL;
        n_syms = 0;
        if (sh[si].sh_link < eh->e_shnum) {
            const elf64_shdr_t *symsec = &sh[sh[si].sh_link];
            if ((symsec->sh_type == SHT_SYMTAB || symsec->sh_type == SHT_DYNSYM) &&
                symsec->sh_entsize == sizeof(elf64_sym_t) &&
                symsec->sh_offset + symsec->sh_size <= rd) {
                symtab = symsec;
                syms = (const elf64_sym_t*)(buf + symsec->sh_offset);
                n_syms = (size_t)(symsec->sh_size / sizeof(elf64_sym_t));
            }
        }

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

                uint64_t S = 0;
                if (syms[r_sym].st_shndx == SHN_UNDEF) {
                    // extern symbol (e.g. memset/memcpy)
                    const elf64_shdr_t *strtab = NULL;
                    if (symtab && symtab->sh_link < eh->e_shnum) strtab = &sh[symtab->sh_link];
                    const char *strings = (strtab && strtab->sh_type == SHT_STRTAB && strtab->sh_offset + strtab->sh_size <= rd)
                                              ? (const char*)(buf + strtab->sh_offset)
                                              : NULL;
                    const char *nm = (strings && syms[r_sym].st_name < (strtab ? strtab->sh_size : 0)) ? (strings + syms[r_sym].st_name) : NULL;
                    S = sqrm_resolve_kernel_symbol(nm);
                } else {
                    uint64_t sym_va = syms[r_sym].st_value;
                    if (sym_va != 0) {
                        uint64_t sym_off = 0;
                        if (sqrm_map_va_to_off(sym_va, min_v, max_v, img_sz, &sym_off) == 0) {
                            S = (uint64_t)image + sym_off;
                        }
                    }
                }

                *where = S + (uint64_t)addend;
            } else if (r_type == R_X86_64_PC32 || r_type == R_X86_64_PLT32) {
                // S + A - P (32-bit signed)
                if (!syms || r_sym >= n_syms) continue;

                // Debug for HID: show the symbol referenced by PC32/PLT32 relocs.
                {
                    static int g_hid_pc32_dbg_budget = 16;
                    const char *m = sqrm_get_current_module_name();
                    if (g_hid_pc32_dbg_budget > 0 && m && strcmp(m, "hid") == 0) {
                        g_hid_pc32_dbg_budget--;
                        const elf64_shdr_t *strtab_dbg = NULL;
                        if (symtab && symtab->sh_link < eh->e_shnum) strtab_dbg = &sh[symtab->sh_link];
                        const char *strings_dbg = (strtab_dbg && strtab_dbg->sh_type == SHT_STRTAB && strtab_dbg->sh_offset + strtab_dbg->sh_size <= rd)
                                                      ? (const char*)(buf + strtab_dbg->sh_offset)
                                                      : NULL;
                        const char *nm_dbg = (strings_dbg && syms[r_sym].st_name < (strtab_dbg ? strtab_dbg->sh_size : 0)) ? (strings_dbg + syms[r_sym].st_name) : NULL;
                        com_write_string(COM1_PORT, "[SQRM] hid pc32 sym=");
                        com_write_string(COM1_PORT, nm_dbg ? nm_dbg : "(null)");
                        com_write_string(COM1_PORT, " shndx=");
                        com_write_hex64(COM1_PORT, (uint64_t)syms[r_sym].st_shndx);
                        com_write_string(COM1_PORT, "\n");
                    }
                }

                uint64_t S = 0;
                if (syms[r_sym].st_shndx == SHN_UNDEF) {
                    const elf64_shdr_t *strtab = NULL;
                    if (symtab && symtab->sh_link < eh->e_shnum) strtab = &sh[symtab->sh_link];
                    const char *strings = (strtab && strtab->sh_type == SHT_STRTAB && strtab->sh_offset + strtab->sh_size <= rd)
                                              ? (const char*)(buf + strtab->sh_offset)
                                              : NULL;
                    const char *nm = (strings && syms[r_sym].st_name < (strtab ? strtab->sh_size : 0)) ? (strings + syms[r_sym].st_name) : NULL;
                    S = sqrm_resolve_kernel_symbol(nm);
                    if (!S) {
                        const char *m = sqrm_get_current_module_name();
                        if (m && strcmp(m, "hid") == 0) {
                            com_write_string(COM1_PORT, "[SQRM] undef pc32 ");
                            com_write_string(COM1_PORT, m);
                            com_write_string(COM1_PORT, " sym=");
                            com_write_string(COM1_PORT, nm ? nm : "(null)");
                            com_write_string(COM1_PORT, "\n");
                        }
                    }
                    if (tramp && S) S = sqrm_tramp_get(tramp, S);
                } else {
                    uint64_t sym_va = syms[r_sym].st_value;
                    if (sym_va != 0) {
                        uint64_t sym_off = 0;
                        if (sqrm_map_va_to_off(sym_va, min_v, max_v, img_sz, &sym_off) == 0) {
                            S = (uint64_t)image + sym_off;
                        }
                    }
                }

                uint64_t P = (uint64_t)(image + img_off);
                int64_t val = (int64_t)S + addend - (int64_t)P;
                *(int32_t*)where = (int32_t)val;
            } else if (r_type == R_X86_64_GOTPCREL || r_type == R_X86_64_GOTPCRELX || r_type == R_X86_64_REX_GOTPCRELX) {
                // G + A - P (32-bit signed)
                uint64_t G = sqrm_got_lookup_slot(got, got_cnt, 0, r_sym);
                uint64_t P = (uint64_t)(image + img_off);
                int64_t v = (int64_t)G + addend - (int64_t)P;
                *(int32_t*)where = (int32_t)v;
            } else if (r_type == R_X86_64_32 || r_type == R_X86_64_32S) {
                // S + A (truncated)
                if (!syms || r_sym >= n_syms) continue;
                uint64_t sym_va = syms[r_sym].st_value;
                uint64_t S = 0;
                if (sym_va != 0) {
                    uint64_t sym_off = 0;
                    if (sqrm_map_va_to_off(sym_va, min_v, max_v, img_sz, &sym_off) == 0) {
                        S = (uint64_t)image + sym_off;
                    }
                }
                *(uint32_t*)where = (uint32_t)(S + (uint64_t)addend);
            }
        }
    }

    // Apply all SHT_REL sections (addend stored at relocation target)
    for (uint16_t si = 0; si < eh->e_shnum; si++) {
        if (sh[si].sh_type != SHT_REL) continue;
        if (sh[si].sh_entsize != sizeof(elf64_rel_t) || sh[si].sh_entsize == 0) continue;
        if (sh[si].sh_offset + sh[si].sh_size > rd) continue;

        // Select the correct symbol table for this relocation section (ELF sh_link)
        symtab = NULL;
        syms = NULL;
        n_syms = 0;
        if (sh[si].sh_link < eh->e_shnum) {
            const elf64_shdr_t *symsec = &sh[sh[si].sh_link];
            if ((symsec->sh_type == SHT_SYMTAB || symsec->sh_type == SHT_DYNSYM) &&
                symsec->sh_entsize == sizeof(elf64_sym_t) &&
                symsec->sh_offset + symsec->sh_size <= rd) {
                symtab = symsec;
                syms = (const elf64_sym_t*)(buf + symsec->sh_offset);
                n_syms = (size_t)(symsec->sh_size / sizeof(elf64_sym_t));
            }
        }

        const elf64_rel_t *rel = (const elf64_rel_t*)(buf + sh[si].sh_offset);
        size_t n = (size_t)(sh[si].sh_size / sizeof(elf64_rel_t));

        for (size_t i = 0; i < n; i++) {
            uint64_t r_off = rel[i].r_offset;
            uint32_t r_type = ELF64_R_TYPE(rel[i].r_info);
            uint32_t r_sym  = ELF64_R_SYM(rel[i].r_info);

            uint64_t img_off = 0;
            if (sqrm_map_va_to_off(r_off, min_v, max_v, img_sz, &img_off) != 0) continue;
            if (img_off + sizeof(uint64_t) > img_sz) continue;

            uint64_t *where64 = (uint64_t*)(image + img_off);

            if (r_type == R_X86_64_RELATIVE) {
                uint64_t addend = *where64;
                *where64 = base + addend;
            } else if (r_type == R_X86_64_64) {
                uint64_t addend = *where64;
                if (!syms || r_sym >= n_syms) continue;

                uint64_t S = 0;
                if (syms[r_sym].st_shndx == SHN_UNDEF) {
                    const elf64_shdr_t *strtab = NULL;
                    if (symtab && symtab->sh_link < eh->e_shnum) strtab = &sh[symtab->sh_link];
                    const char *strings = (strtab && strtab->sh_type == SHT_STRTAB && strtab->sh_offset + strtab->sh_size <= rd)
                                              ? (const char*)(buf + strtab->sh_offset)
                                              : NULL;
                    const char *nm = (strings && syms[r_sym].st_name < (strtab ? strtab->sh_size : 0)) ? (strings + syms[r_sym].st_name) : NULL;
                    S = sqrm_resolve_kernel_symbol(nm);
                } else {
                    uint64_t sym_va = syms[r_sym].st_value;
                    if (sym_va != 0) {
                        uint64_t sym_off = 0;
                        if (sqrm_map_va_to_off(sym_va, min_v, max_v, img_sz, &sym_off) == 0) {
                            S = (uint64_t)image + sym_off;
                        }
                    }
                }

                *where64 = S + addend;
            } else if (r_type == R_X86_64_PC32 || r_type == R_X86_64_PLT32) {
                int32_t addend = *(int32_t*)where64;
                if (!syms || r_sym >= n_syms) continue;

                uint64_t S = 0;
                if (syms[r_sym].st_shndx == SHN_UNDEF) {
                    const elf64_shdr_t *strtab = NULL;
                    if (symtab && symtab->sh_link < eh->e_shnum) strtab = &sh[symtab->sh_link];
                    const char *strings = (strtab && strtab->sh_type == SHT_STRTAB && strtab->sh_offset + strtab->sh_size <= rd)
                                              ? (const char*)(buf + strtab->sh_offset)
                                              : NULL;
                    const char *nm = (strings && syms[r_sym].st_name < (strtab ? strtab->sh_size : 0)) ? (strings + syms[r_sym].st_name) : NULL;
                    S = sqrm_resolve_kernel_symbol(nm);
                    if (tramp && S) S = sqrm_tramp_get(tramp, S);
                } else {
                    uint64_t sym_va = syms[r_sym].st_value;
                    if (sym_va != 0) {
                        uint64_t sym_off = 0;
                        if (sqrm_map_va_to_off(sym_va, min_v, max_v, img_sz, &sym_off) == 0) {
                            S = (uint64_t)image + sym_off;
                        }
                    }
                }

                uint64_t P = (uint64_t)(image + img_off);
                int64_t val = (int64_t)S + (int64_t)addend - (int64_t)P;
                *(int32_t*)where64 = (int32_t)val;
            } else if (r_type == R_X86_64_GOTPCREL || r_type == R_X86_64_GOTPCRELX || r_type == R_X86_64_REX_GOTPCRELX) {
                int32_t addend = *(int32_t*)where64;
                uint64_t G = sqrm_got_lookup_slot(got, got_cnt, 0, r_sym);
                uint64_t P = (uint64_t)(image + img_off);
                int64_t v = (int64_t)G + (int64_t)addend - (int64_t)P;
                *(int32_t*)where64 = (int32_t)v;
            } else if (r_type == R_X86_64_32 || r_type == R_X86_64_32S) {
                uint32_t addend = *(uint32_t*)where64;
                if (!syms || r_sym >= n_syms) continue;
                uint64_t sym_va = syms[r_sym].st_value;
                uint64_t S = 0;
                if (sym_va != 0) {
                    uint64_t sym_off = 0;
                    if (sqrm_map_va_to_off(sym_va, min_v, max_v, img_sz, &sym_off) == 0) {
                        S = (uint64_t)image + sym_off;
                    }
                }
                *(uint32_t*)where64 = (uint32_t)(S + addend);
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

// block_register for SQRM_TYPE_DRIVE modules: registers the module's
// blockdev_ops_t in the blockdev registry, then immediately attaches it as
// a vdrive so the rest of the storage stack (devfs nodes, FS mounts, cache)
// sees it exactly like a native ATA/SATA drive. Modules only need to make
// this one call per drive they discover.
static int sqrm_drive_block_register(const void *ops, void *ctx, blockdev_handle_t *out_handle) {
    if (!ops) return -1;

    blockdev_handle_t h = blockdev_register((const blockdev_ops_t *)ops, ctx);
    if (h == BLOCKDEV_INVALID_HANDLE) return -2;

    blockdev_info_t info;
    if (blockdev_get_info(h, &info) != 0) return -3;

    int vdrive_id = vdrive_register_sqrm_drive(h, &info);
    if (vdrive_id < 0) return -4;

    if (out_handle) *out_handle = h;
    return 0;
}

// Resolve the address of sqrm_module_desc inside the relocated image and optionally return v2 pointer.
static const void *sqrm_find_desc_ptr_in_image(const uint8_t *buf, size_t rd, const elf64_ehdr_t *eh,
                                              uint64_t min_v, const uint8_t *image,
                                              const sqrm_module_desc_v2_t **out_v2) {
    if (out_v2) *out_v2 = NULL;
    if (!buf || !eh || !image) return NULL;

    if (eh->e_shoff == 0 || eh->e_shnum == 0 || eh->e_shentsize != sizeof(elf64_shdr_t)) return NULL;
    if (eh->e_shoff + (uint64_t)eh->e_shnum * sizeof(elf64_shdr_t) > rd) return NULL;

    const elf64_shdr_t *sh = (const elf64_shdr_t*)(buf + eh->e_shoff);
    const elf64_shdr_t *symtab = NULL;
    const elf64_shdr_t *strtab = NULL;
    for (uint16_t i = 0; i < eh->e_shnum; i++) {
        if (sh[i].sh_type == SHT_SYMTAB) {
            symtab = &sh[i];
            if (symtab->sh_link < eh->e_shnum) strtab = &sh[symtab->sh_link];
            break;
        }
    }
    if (!symtab || !strtab || strtab->sh_type != SHT_STRTAB) return NULL;
    if (symtab->sh_offset + symtab->sh_size > rd || strtab->sh_offset + strtab->sh_size > rd) return NULL;

    const char *strings = (const char*)(buf + strtab->sh_offset);
    const elf64_sym_t *syms = (const elf64_sym_t*)(buf + symtab->sh_offset);
    size_t n_syms = (size_t)(symtab->sh_size / sizeof(elf64_sym_t));

    for (size_t i = 0; i < n_syms; i++) {
        uint32_t noff = syms[i].st_name;
        if (noff >= strtab->sh_size) continue;
        const char *nm = strings + noff;
        if (strcmp(nm, SQRM_DESC_SYMBOL) != 0) continue;
        uint64_t va = syms[i].st_value;
        if (va < min_v) return NULL;
        uint64_t off = va - min_v;
        const sqrm_module_desc_t *d = (const sqrm_module_desc_t*)(image + off);
        if (out_v2 && d && d->abi_version == SQRM_ABI_V2) {
            *out_v2 = (const sqrm_module_desc_v2_t*)(image + off);
        }
        return (const void*)(image + off);
    }

    return NULL;
}

static int sqrm_dep_stack_contains(const char **stack, size_t depth, const char *name) {
    if (!stack || !name) return 0;
    for (size_t i = 0; i < depth; i++) {
        if (stack[i] && strcmp(stack[i], name) == 0) return 1;
    }
    return 0;
}

static int sqrm_load_module_by_desc_name_recursive(const char *modname, const char **stack, size_t depth) {
    if (!modname) return -1;
    if (already_loaded_by_modname(modname)) return 0;
    if (sqrm_dep_stack_contains(stack, depth, modname)) {
        com_write_string(COM1_PORT, "[SQRM] dependency cycle detected at ");
        com_write_string(COM1_PORT, modname);
        com_write_string(COM1_PORT, "\n");
        return -2;
    }

    // Extend stack for deeper loads.
    // Old code used a fixed depth of 16 which breaks once you have large dependency graphs.
    // Keep a reasonable safety limit to avoid infinite recursion on buggy module metadata.
    if (depth >= 256) return -4;

    const char **next_stack = (const char**)kmalloc((depth + 1) * sizeof(*next_stack));
    if (!next_stack) return -ENOMEM;
    for (size_t i = 0; i < depth; i++) next_stack[i] = stack[i];
    next_stack[depth] = modname;

    char basename[128];
    if (sqrm_find_module_file_by_desc_name(modname, basename, sizeof(basename)) != 0) {
        com_write_string(COM1_PORT, "[SQRM] missing dependency module: ");
        com_write_string(COM1_PORT, modname);
        com_write_string(COM1_PORT, "\n");
        kfree((void*)next_stack);
        return -3;
    }

    char full[256];
    full[0] = 0;
    strcat(full, SQRM_MODULE_DIR);
    strcat(full, "/");
    strcat(full, basename);

    // Load module normally.
    // NOTE: sqrm_load_one currently does not accept a dependency stack; so cycle detection is limited to
    // the immediate chain. This is still valuable (A->B->A), and deeper cycles will be caught as modules
    // are requested again by name.
    int rc = sqrm_load_one(full, basename, NULL, next_stack, depth + 1);
    kfree((void*)next_stack);
    return rc;
}

static void sqrm_input_push_event_impl(const Event *e) {
    if (!e) return;
    devfs_input_push_event(e);
    // event_push takes non-const pointer; it copies the event into a ring internally.
    Event tmp = *e;
    (void)event_push(&tmp);
}

/* DevFS registration wrapper for SQRM modules */
static int sqrm_devfs_register_path(const char *path, const void *ops, void *ctx) {
    devfs_owner_t owner = { .kind = DEVFS_OWNER_SQRM, .id = "sqrm" };
    return devfs_register_path(path, (const devfs_device_ops_t *)ops, ctx, owner);
}

static void sqrm_sleep_ms_impl(uint64_t ms) {
    const uint64_t start = get_system_ticks();
    const uint64_t wait = ms_to_ticks(ms);
    while ((get_system_ticks() - start) < wait) {
        // busy wait; modules should avoid long sleeps
    }
}

static const framebuffer_t *gfx_wrap_get_framebuffer(void) {
    if (!g_active_gpu) return NULL;
    return &g_active_gpu->fb;
}
static uint32_t gfx_wrap_get_caps(void) {
    if (!g_active_gpu) return 0;
    return g_active_gpu->caps;
}
static int gfx_wrap_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t pixel) {
    if (!g_active_gpu || !g_active_gpu->fill_rect32_native) return -1;
    return g_active_gpu->fill_rect32_native(&g_active_gpu->fb, x, y, w, h, pixel);
}
static int gfx_wrap_blit_rect(uint32_t sx, uint32_t sy, uint32_t dx, uint32_t dy, uint32_t w, uint32_t h) {
    if (!g_active_gpu || !g_active_gpu->blit_rect32) return -1;
    return g_active_gpu->blit_rect32(&g_active_gpu->fb, sx, sy, dx, dy, w, h);
}
static int gfx_wrap_cursor_move(int32_t x, int32_t y) {
    if (!g_active_gpu || !g_active_gpu->cursor_move) return -1;
    return g_active_gpu->cursor_move(x, y);
}
static int gfx_wrap_cursor_show(int visible) {
    if (!g_active_gpu || !g_active_gpu->cursor_show) return -1;
    return g_active_gpu->cursor_show(visible);
}
static int gfx_wrap_flush(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (!g_active_gpu || !g_active_gpu->flush) return -1;
    g_active_gpu->flush(&g_active_gpu->fb, x, y, w, h);
    return 0;
}
static int gfx_wrap_set_mode(uint32_t width, uint32_t height, uint32_t bpp) {
    if (!g_active_gpu || !g_active_gpu->set_mode) return -1;
    return g_active_gpu->set_mode(width, height, bpp);
}

static int gfx_wrap_blit_buffer(uint32_t dx, uint32_t dy, uint32_t w, uint32_t h,
                                const void *src_buf, uint32_t src_stride) {
    return g_active_gpu->blit_buffer(&g_active_gpu->fb, dx, dy, src_buf, src_stride, w, h);
}

/* NOTE — trust boundary, currently incomplete:
 * umcs_binary is expected to have ALREADY been validated (module header +
 * every instruction, the same checks wumcs-validate.py performs) before it
 * reaches here. This wrapper is the natural point to enforce that — every
 * caller funnels through it — but that check is NOT YET IMPLEMENTED. Do
 * not register a real (non-stub) GPU shader_run backend until a C port of
 * the validator is added here; until then this only forwards to whatever
 * the active GPU module does with unvalidated bytecode. */
static int gfx_wrap_shader_dispatch(const uint8_t *umcs_binary, size_t umcs_size,
                                    sqrm_shader_stage_t stage,
                                    void *buffers[], uint32_t buffer_count,
                                    uint32_t invocation_count) {
    if (!g_active_gpu || !g_active_gpu->shader_run) return -1;
    if (!(g_active_gpu->caps & SQRM_GPU_CAP_PROGRAMMABLE)) return -1;
    return g_active_gpu->shader_run(umcs_binary, umcs_size, stage,
                                    buffers, buffer_count, invocation_count);
}

static void sqrm_build_api(const sqrm_module_desc_t *desc, sqrm_kernel_api_t *out_api) {
    memset(out_api, 0, sizeof(*out_api));
    out_api->abi_version = 1;
    out_api->module_type = desc->type;
    out_api->module_name = desc->name;

    // Make module name available to kernel-side callbacks invoked by this module.
    {
        const char *n = desc->name ? desc->name : "";
        size_t i = 0;
        for (; i + 1 < sizeof(g_sqrm_current_module_name) && n[i]; i++) g_sqrm_current_module_name[i] = n[i];
        g_sqrm_current_module_name[i] = 0;
    }

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

    out_api->get_system_ticks = get_system_ticks;
    out_api->ticks_to_ms = ticks_to_ms;
    out_api->ms_to_ticks = ms_to_ticks;
    out_api->sleep_ms = sqrm_sleep_ms_impl;

    /* Panic */
    out_api->panic_system = panic;

    out_api->usercopy_from_user = usercopy_from_user;
    out_api->usercopy_to_user = usercopy_to_user;
    out_api->usercopy_string_from_user = usercopy_string_from_user;

    // blockdev APIs: FS and GENERIC modules (GENERIC needs read-only info for sysinfo)
    if (desc->type == SQRM_TYPE_FS || desc->type == SQRM_TYPE_GENERIC) {
        out_api->block_get_info = blockdev_get_info;
        out_api->block_read = blockdev_read;
        out_api->block_write = blockdev_write;
        out_api->block_get_handle_for_vdrive = sqrm_block_get_handle_for_vdrive;
    }

    // VFS FS-driver registration: FS modules only
    if (desc->type == SQRM_TYPE_FS) {
        out_api->fs_register_driver = fs_register_driver;
    }

    // DRIVE modules: storage controller drivers (AHCI, NVMe, etc.) living
    // entirely outside the kernel tree. They need PCI enumeration/config,
    // MMIO for controller BARs, an IRQ line, DMA for command structures,
    // and the ability to register the drives they find as vdrives.
    if (desc->type == SQRM_TYPE_DRIVE) {
        out_api->pci_get_device_count     = pci_get_device_count;
        out_api->pci_get_device           = pci_get_device;
        out_api->pci_find_device          = pci_find_device;
        out_api->pci_enable_memory_space  = pci_enable_memory_space;
        out_api->pci_enable_io_space      = pci_enable_io_space;
        out_api->pci_enable_bus_mastering = pci_enable_bus_mastering;
        out_api->pci_cfg_read32           = pci_config_read_dword;
        out_api->pci_cfg_write32          = pci_config_write_dword;

        out_api->ioremap         = ioremap;
        out_api->ioremap_guarded = ioremap_guarded;
        out_api->virt_to_phys    = paging_virt_to_phys;

        out_api->irq_install_handler   = irq_install_handler;
        out_api->irq_uninstall_handler = irq_uninstall_handler;
        out_api->pic_send_eoi          = pic_send_eoi;

        out_api->dma_alloc = dma_alloc;
        out_api->dma_free  = dma_free;

        out_api->block_get_info = blockdev_get_info;
        out_api->block_read     = blockdev_read;
        out_api->block_write    = blockdev_write;
        out_api->block_register = sqrm_drive_block_register;
    }

    // DEVFS/input injection: available to HID modules.
    if (desc->type == SQRM_TYPE_HID) {
        out_api->input_push_event = sqrm_input_push_event_impl;
    }

    // devfs registration will be wired later (after reserved-path enforcement is finished)

    // Graphics registration: GPU modules only
    if (desc->type == SQRM_TYPE_GPU) {
        out_api->gfx_register_framebuffer = gfx_register_framebuffer_from_sqrm;
        out_api->gfx_update_framebuffer = gfx_update_framebuffer_from_sqrm;

        // PCI + MMIO helpers for GPU modules
        out_api->pci_get_device_count = pci_get_device_count;
        out_api->pci_get_device = pci_get_device;
        out_api->pci_find_device = pci_find_device;
        out_api->pci_enable_memory_space = pci_enable_memory_space;
        out_api->pci_enable_io_space = pci_enable_io_space;
        out_api->pci_enable_bus_mastering = pci_enable_bus_mastering;
        out_api->ioremap = ioremap;
        out_api->ioremap_guarded = ioremap_guarded;

        out_api->pci_cfg_read32           = pci_config_read_dword;
        out_api->pci_cfg_write32          = pci_config_write_dword;
        out_api->virt_to_phys             = paging_virt_to_phys;
    }

    // NET modules also need PCI + MMIO
    if (desc->type == SQRM_TYPE_NET) {
        out_api->pci_get_device_count = pci_get_device_count;
        out_api->pci_get_device = pci_get_device;
        out_api->pci_find_device = pci_find_device;
        out_api->pci_enable_memory_space = pci_enable_memory_space;
        out_api->pci_enable_io_space = pci_enable_io_space;
        out_api->pci_enable_bus_mastering = pci_enable_bus_mastering;
        out_api->ioremap = ioremap;
        out_api->ioremap_guarded = ioremap_guarded;

        // restricted PCI config access
        out_api->pci_cfg_read32 = pci_config_read_dword;
        out_api->pci_cfg_write32 = pci_config_write_dword;
        out_api->virt_to_phys = paging_virt_to_phys;
    }

    // USB/HID modules need PCI for controller enumeration and MMIO mapping (OHCI/EHCI).
    if (desc->type == SQRM_TYPE_USB || desc->type == SQRM_TYPE_HID) {
        out_api->pci_get_device_count = pci_get_device_count;
        out_api->pci_get_device = pci_get_device;
        out_api->pci_find_device = pci_find_device;
        out_api->pci_enable_memory_space = pci_enable_memory_space;
        out_api->pci_enable_io_space = pci_enable_io_space;
        out_api->pci_enable_bus_mastering = pci_enable_bus_mastering;
        out_api->ioremap = ioremap;
        out_api->ioremap_guarded = ioremap_guarded;

        // restricted PCI config access
        out_api->pci_cfg_read32 = pci_config_read_dword;
        out_api->pci_cfg_write32 = pci_config_write_dword;
        out_api->virt_to_phys = paging_virt_to_phys;
    }

    // audio registration: audio modules only
    if (desc->type == SQRM_TYPE_AUDIO) {
        out_api->audio_register_pcm = audio_register_pcm;

        /* DMA — HDA/AC97 need physically contiguous ring buffers (CORB/RIRB/BDL) */
        out_api->dma_alloc = dma_alloc;
        out_api->dma_free  = dma_free;

        /* Port I/O — raw CF8/CFC PCI scan used by HDA/AC97 */
        out_api->inb  = inb;
        out_api->inw  = inw;
        out_api->inl  = inl;
        out_api->outb = outb;
        out_api->outw = outw;
        out_api->outl = outl;

        /* IRQ — HDA interrupt-driven BDL completion */
        out_api->irq_install_handler   = irq_install_handler;
        out_api->irq_uninstall_handler = irq_uninstall_handler;
        out_api->pic_send_eoi          = pic_send_eoi;

        /* MMIO — HDA BAR0 mapping */
        out_api->ioremap         = ioremap;
        out_api->ioremap_guarded = ioremap_guarded;

        /* virt_to_phys — fallback when dma_alloc returns NULL */
        out_api->virt_to_phys = paging_virt_to_phys;

        /* PCI — controller discovery */
        out_api->pci_get_device_count    = pci_get_device_count;
        out_api->pci_get_device          = pci_get_device;
        out_api->pci_find_device         = pci_find_device;
        out_api->pci_enable_memory_space  = pci_enable_memory_space;
        out_api->pci_enable_io_space      = pci_enable_io_space;
        out_api->pci_enable_bus_mastering = pci_enable_bus_mastering;
        out_api->pci_cfg_read32           = pci_config_read_dword;
        out_api->pci_cfg_write32          = pci_config_write_dword;
    }

    if (desc->type == SQRM_TYPE_GENERIC) {
        out_api->gfx_get_framebuffer = gfx_wrap_get_framebuffer;
        out_api->gfx_get_caps        = gfx_wrap_get_caps;
        out_api->gfx_fill_rect       = gfx_wrap_fill_rect;
        out_api->gfx_blit_rect       = gfx_wrap_blit_rect;
        out_api->gfx_blit_buffer     = gfx_wrap_blit_buffer;
        out_api->gfx_shader_dispatch = gfx_wrap_shader_dispatch;
        out_api->gfx_cursor_move     = gfx_wrap_cursor_move;
        out_api->gfx_cursor_show     = gfx_wrap_cursor_show;
        out_api->gfx_flush           = gfx_wrap_flush;
        out_api->gfx_set_mode        = gfx_wrap_set_mode;
        com_write_hex64(COM1_PORT, (uint64_t)(uintptr_t)gfx_wrap_blit_buffer);
    }

    // SQRM services (exports): available to all modules
    out_api->sqrm_service_register = sqrm_service_register_impl;
    out_api->sqrm_service_get      = sqrm_service_get_impl;

    /* Multiboot2 header — virtual address set in kernel_main(). */
    extern uint64_t kernel_get_mb2_ptr(void);
    out_api->multiboot2_header = (const void *)(uintptr_t)kernel_get_mb2_ptr();

    /* DevFS registration — available to all module types. */
    out_api->devfs_register_path = sqrm_devfs_register_path;

    out_api->get_gpu_driver_name    = gfx_get_active_driver_name;
    out_api->get_smbios_field       = md64api_sqrm_get_smbios_field;
    out_api->phys_total_frames      = phys_total_frames;
    out_api->phys_count_free_frames = phys_count_free_frames;

    out_api->devfs_mmap_region = devfs_mmap_region;
}

/*
 * ── NET glue: DevFS node backing a registered "net" SQRM service ───────
 *
 * Previous design (removed): control AND data (TX/RX) were multiplexed
 * through devfs write()+read() as a two-syscall command/reply RPC, with
 * the reply staged in a per-open buffer between the two calls. That's
 * fragile — a second write() before the matching read() clobbers the
 * previous reply, two threads sharing a dup'd fd can interleave a write
 * from one with a read from the other, and RX was strictly one frame
 * per poll with no queue, so any frame that arrived between poll calls
 * and got overwritten in the NIC's own ring was silently lost.
 *
 * New design:
 *   - read()/write() are real byte-oriented frame I/O, matching what
 *     DevFS's read/write contract already means everywhere else:
 *       read()  -> pop one queued RX frame (or drain-and-block on the
 *                  ~1kHz timer tick if the fd is blocking and the ring
 *                  is empty; return 0 immediately if O_NONBLOCK).
 *       write() -> transmit exactly one frame (buf/count is the raw
 *                  Ethernet frame, no header/wrapper).
 *   - invoke() is metadata/control only (GET_MODE / GET_LINK_UP /
 *     GET_MTU / GET_MAC) — a single syscall, no shared staging buffer,
 *     nothing to interleave.
 *   - A real RX ring sits between the driver and devfs: sqrm_net_rx_drain()
 *     pulls every frame currently sitting in the driver's descriptors in
 *     one pass, so a burst of packets between two userland reads is
 *     queued instead of dropped down to the last one. The ring is
 *     IRQ-safe (spinlock + irq_save) so it's ready for the driver to
 *     push into it directly from an interrupt handler later — today the
 *     e1000 driver is poll-only (it masks all NIC interrupts at init),
 *     so the drain is still pull-based, just pulled in bulk instead of
 *     one frame at a time.
 */

#define NET_CMD_GET_MODE     1u
#define NET_CMD_GET_LINK_UP  2u
#define NET_CMD_GET_MTU      3u
#define NET_CMD_GET_MAC      4u

#define NET_MODE_ETH         1u
#define NET_MODE_WIFI        2u

#define SQRM_NET_RX_RING_SLOTS 32
#define SQRM_NET_RX_FRAME_MAX  1600  /* MTU 1500 + Ethernet header/VLAN slack */

typedef struct {
    uint8_t  data[SQRM_NET_RX_FRAME_MAX];
    uint32_t len;
} sqrm_net_rx_slot_t;

/* Shared per-device state — one instance per net0/net1/... node. */
typedef struct {
    sqrm_net_api_v1_t *net;

    spinlock_t          rx_lock;
    sqrm_net_rx_slot_t  rx_ring[SQRM_NET_RX_RING_SLOTS];
    uint32_t             rx_r, rx_w, rx_count;
    uint32_t             rx_dropped; /* diagnostic: frames dropped, ring was full */
} sqrm_net_node_ctx_t;

/* Per-open state. */
typedef struct {
    sqrm_net_node_ctx_t *nc;
    int                   open_flags;
} sqrm_net_open_ctx_t;

static int g_net_index = 0;

/* ── RX ring helpers (IRQ-safe; ready for future interrupt-time push) ── */

static void sqrm_net_rx_push(sqrm_net_node_ctx_t *nc, const void *frame, uint32_t len)
{
    if (!nc || !frame || len == 0 || len > SQRM_NET_RX_FRAME_MAX) return;

    uint64_t f = irq_save();
    spinlock_lock(&nc->rx_lock);

    if (nc->rx_count >= SQRM_NET_RX_RING_SLOTS) {
        nc->rx_dropped++;
    } else {
        sqrm_net_rx_slot_t *slot = &nc->rx_ring[nc->rx_w];
        memcpy(slot->data, frame, len);
        slot->len = len;
        nc->rx_w = (nc->rx_w + 1) % SQRM_NET_RX_RING_SLOTS;
        nc->rx_count++;
    }

    spinlock_unlock(&nc->rx_lock);
    irq_restore(f);
}

/* Pop the oldest queued frame. Returns 1 and fills out_buf/out_len, or 0 if empty. */
static int sqrm_net_rx_pop(sqrm_net_node_ctx_t *nc, void *out_buf, size_t out_cap, uint32_t *out_len)
{
    if (!nc) return 0;

    uint64_t f = irq_save();
    spinlock_lock(&nc->rx_lock);

    if (nc->rx_count == 0) {
        spinlock_unlock(&nc->rx_lock);
        irq_restore(f);
        return 0;
    }

    sqrm_net_rx_slot_t *slot = &nc->rx_ring[nc->rx_r];
    uint32_t len = slot->len;

    if (len > out_cap) {
        /* Caller's buffer is too small — leave the frame queued so a
         * retry with a bigger buffer still works, rather than losing it. */
        spinlock_unlock(&nc->rx_lock);
        irq_restore(f);
        return -1;
    }

    memcpy(out_buf, slot->data, len);
    if (out_len) *out_len = len;

    nc->rx_r = (nc->rx_r + 1) % SQRM_NET_RX_RING_SLOTS;
    nc->rx_count--;

    spinlock_unlock(&nc->rx_lock);
    irq_restore(f);
    return 1;
}

/*
 * Drain every frame currently sitting in the driver's own RX descriptors
 * into our ring. Bounded so a misbehaving driver (rx_poll never returning
 * "empty") can't wedge the caller forever.
 */
static void sqrm_net_rx_drain(sqrm_net_node_ctx_t *nc)
{
    if (!nc || !nc->net || !nc->net->rx_poll) return;

    uint8_t tmp[SQRM_NET_RX_FRAME_MAX];
    for (int i = 0; i < SQRM_NET_RX_RING_SLOTS * 2; i++) {
        size_t got = 0;
        int rc = nc->net->rx_poll(tmp, sizeof(tmp), &got);
        if (rc != 0 || got == 0) break;

        sqrm_net_rx_push(nc, tmp, (uint32_t)got);
        if (nc->net->rx_consume) nc->net->rx_consume();
    }
}

/* ── open / close ─────────────────────────────────────────────────────── */

static void *sqrm_net_devfs_open(void *ctx, int flags)
{
    sqrm_net_node_ctx_t *nc = (sqrm_net_node_ctx_t *)ctx;
    sqrm_net_open_ctx_t *oc = (sqrm_net_open_ctx_t *)kmalloc(sizeof(*oc));
    if (!oc) return NULL;
    oc->nc          = nc;
    oc->open_flags  = flags;
    return oc;
}

static int sqrm_net_devfs_close(void *ctx)
{
    if (ctx) kfree(ctx);
    return 0;
}

/* ── read()  –  pop one queued RX frame, blocking unless O_NONBLOCK ───── */

static ssize_t sqrm_net_devfs_read(void *ctx, void *user_buf, size_t count)
{
    sqrm_net_open_ctx_t *oc = (sqrm_net_open_ctx_t *)ctx;
    if (!oc || !oc->nc || !user_buf) return -1;

    for (;;) {
        sqrm_net_rx_drain(oc->nc);

        uint32_t got = 0;
        int rc = sqrm_net_rx_pop(oc->nc, user_buf, count, &got);
        if (rc > 0) return (ssize_t)got;
        if (rc < 0) return -2; /* ENOSPC-ish: caller's buffer too small for the queued frame */

        /* Nothing queued. */
        if (oc->open_flags & O_NONBLOCK) return 0;

        /* Block until the next interrupt (the ~1kHz timer tick included),
         * then loop around and drain again. Once the driver gains real
         * RX-interrupt support this same wait wakes immediately on a
         * frame arriving instead of on the next tick. */
        hlt_wait_preserve_if();
    }
}

/* ── write()  –  transmit exactly one raw frame ───────────────────────── */
//
// tx_frame() returns -11 (EAGAIN) when the driver's next TX descriptor
// hasn't had its DD (Descriptor Done) bit set by hardware yet -- normally
// a race that resolves itself in well under a millisecond. The old code
// surfaced that instantly as a write() failure, and since the driver
// never advances its tx_tail on that path, every future write() re-checked
// the exact same descriptor: one transient EAGAIN turned into every
// subsequent transmit failing forever (this is what was wedging netd
// after one slow packet). Retry here, under a bounded real-time deadline,
// so a normal short stall is absorbed instead of propagating as a
// permanent failure. O_NONBLOCK callers still get EAGAIN back immediately,
// same as a nonblocking socket would.
#define SQRM_NET_TX_RETRY_MS 50

static ssize_t sqrm_net_devfs_write(void *ctx, const void *user_buf, size_t count)
{
    sqrm_net_open_ctx_t *oc = (sqrm_net_open_ctx_t *)ctx;
    if (!oc || !oc->nc || !oc->nc->net || !user_buf || count == 0) return -1;
    if (!oc->nc->net->tx_frame) return -1;

    const uint64_t start = get_system_ticks();
    const uint64_t deadline_ticks = ms_to_ticks(SQRM_NET_TX_RETRY_MS);

    for (;;) {
        int rc = oc->nc->net->tx_frame(user_buf, count);
        if (rc == 0) return (ssize_t)count;

        if (rc != -11 /* EAGAIN */) return rc; /* real error -- don't retry */

        if (oc->open_flags & O_NONBLOCK) return rc;

        if ((get_system_ticks() - start) >= deadline_ticks) {
            /* Genuinely stuck (not just a transient race) -- give up and
             * report EAGAIN truthfully rather than spinning forever. */
            return rc;
        }
        /* busy wait a beat and let the descriptor's in-flight completion catch up */
    }
}

/* ── invoke()  –  metadata/control only; one syscall, no staging state ── */

static ssize_t sqrm_net_devfs_invoke(void *ctx, const void *in_buf, size_t in_size,
                                     void *out_buf, size_t out_size)
{
    sqrm_net_open_ctx_t *oc = (sqrm_net_open_ctx_t *)ctx;
    if (!oc || !oc->nc || !oc->nc->net) return -1;
    if (!in_buf || in_size < sizeof(uint32_t)) return -1;

    sqrm_net_api_v1_t *net = oc->nc->net;

    uint32_t cmd = 0;
    memcpy(&cmd, in_buf, sizeof(cmd));

    switch (cmd) {

        case NET_CMD_GET_MODE: {
            if (!out_buf || out_size < sizeof(uint32_t)) return -1;
            uint32_t mode = NET_MODE_ETH;
            memcpy(out_buf, &mode, sizeof(mode));
            return (ssize_t)sizeof(mode);
        }

        case NET_CMD_GET_LINK_UP: {
            if (!net->get_link_up) return -1;
            if (!out_buf || out_size < sizeof(uint32_t)) return -1;
            uint32_t val = (uint32_t)(net->get_link_up() ? 1 : 0);
            memcpy(out_buf, &val, sizeof(val));
            return (ssize_t)sizeof(val);
        }

        case NET_CMD_GET_MTU: {
            if (!net->get_mtu) return -1;
            if (!out_buf || out_size < sizeof(uint32_t)) return -1;
            uint32_t mtu = 0;
            if (net->get_mtu(&mtu) != 0) return -1;
            memcpy(out_buf, &mtu, sizeof(mtu));
            return (ssize_t)sizeof(mtu);
        }

        case NET_CMD_GET_MAC: {
            if (!net->get_mac) return -1;
            if (!out_buf || out_size < 6) return -1;
            uint8_t mac[6] = {0};
            if (net->get_mac(mac) != 0) return -1;
            memcpy(out_buf, mac, 6);
            return 6;
        }

        default:
            return -99;
    }
}

/* ── devfs ops table ──────────────────────────────────────────────────── */

static const devfs_device_ops_t g_sqrm_net_ops = {
    .name   = "net",
    .open   = sqrm_net_devfs_open,
    .read   = sqrm_net_devfs_read,
    .write  = sqrm_net_devfs_write,
    .close  = sqrm_net_devfs_close,
    .invoke = sqrm_net_devfs_invoke,
};

/* ── Auto-registration (called after a NIC SQRM module loads) ─────────── */

static void sqrm_net_autoregister_devfs(const sqrm_module_desc_t *desc)
{
    size_t api_sz = 0;
    const sqrm_net_api_v1_t *net =
        (const sqrm_net_api_v1_t *)sqrm_service_get_impl("net", &api_sz);
    if (!net || api_sz < sizeof(sqrm_net_api_v1_t)) {
        com_write_string(COM1_PORT,
            "[SQRM] NET module did not register 'net' service\n");
        return;
    }

    sqrm_net_node_ctx_t *nc =
        (sqrm_net_node_ctx_t *)kmalloc(sizeof(sqrm_net_node_ctx_t));
    if (!nc) {
        com_write_string(COM1_PORT, "[SQRM] NET node ctx alloc failed\n");
        return;
    }
    memset(nc, 0, sizeof(*nc));
    nc->net = (sqrm_net_api_v1_t *)net;
    spinlock_init(&nc->rx_lock);

    /* Build canonical name: net0, net1, net2 … */
    char name[16];
    name[0] = 'n'; name[1] = 'e'; name[2] = 't';
    itoa(g_net_index, name + 3, 10);

    char path[64];
    path[0] = 0;
    strcat(path, "net/");
    strcat(path, name);

    devfs_owner_t owner = { .kind = DEVFS_OWNER_SQRM, .id = "sqrm-net" };
    int rc = devfs_register_path(path, &g_sqrm_net_ops, nc, owner);
    if (rc != 0) {
        kfree(nc);
        com_write_string(COM1_PORT, "[SQRM] NET devfs register failed (rc=");
        char rcbuf[12]; itoa(rc, rcbuf, 10);
        com_write_string(COM1_PORT, rcbuf);
        com_write_string(COM1_PORT, "): ");
        com_write_string(COM1_PORT, path);
        com_write_string(COM1_PORT, "\n");
        return;
    }

    g_net_index++;
    com_write_string(COM1_PORT, "[SQRM] NET devfs node: ");
    com_write_string(COM1_PORT, path);
    com_write_string(COM1_PORT, " (driver: ");
    com_write_string(COM1_PORT, desc->name);
    com_write_string(COM1_PORT, ")\n");
}

// Core ELF loader, shared by disk-backed (SQRM_MODULE_DIR) and in-memory
// (multiboot2 initmod TPK) module sources. Takes ownership of buf on every
// return path -- callers must not touch or free it after calling this.
static int sqrm_load_one_from_memory(uint8_t *buf, size_t rd, const char *basename,
                                     const char **dep_stack, size_t dep_depth) {
    if (already_loaded(basename)) {
        kfree(buf);
        return 0;
    }

    if (rd < sizeof(elf64_ehdr_t)) {
        kfree(buf);
        return -2;
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

    // Reserve a contiguous GOT region right after the image so RIP-relative GOT accesses stay in range.
    uint64_t got_bytes = (uint64_t)SQRM_GOT_MAX_SLOTS * sizeof(uint64_t);
    uint64_t got_pages = (got_bytes + 0xFFFULL) & ~0xFFFULL;
    uint64_t total_sz = img_sz_aligned + got_pages;

    uint8_t *image = (uint8_t*)kmalloc((size_t)total_sz);
    if (!image) {
        kfree(buf);
        return -11;
    }
    memset(image, 0, (size_t)total_sz);

    // Copy PT_LOAD segments
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        if (ph[i].p_filesz == 0) continue;
        if (ph[i].p_offset + ph[i].p_filesz > rd) continue;

        uint64_t dst_off = ph[i].p_vaddr - min_v;
        if (dst_off + ph[i].p_filesz > img_sz_aligned) continue;

        memcpy(image + dst_off, buf + ph[i].p_offset, (size_t)ph[i].p_filesz);
    }

    // Build minimal per-module GOT for GOTPCREL relocations.
    sqrm_got_slot_t *got = (sqrm_got_slot_t*)kmalloc(sizeof(sqrm_got_slot_t) * SQRM_GOT_MAX_SLOTS);
    if (!got) {
        kfree(image);
        kfree(buf);
        return -ENOMEM;
    }
    memset(got, 0, sizeof(sqrm_got_slot_t) * SQRM_GOT_MAX_SLOTS);
    size_t got_cnt = 0;
    uint64_t *got_base = (uint64_t*)(image + img_sz_aligned);

    // Scan section relocation sections for GOTPCREL*.
    if (eh->e_shoff && eh->e_shnum && eh->e_shentsize == sizeof(elf64_shdr_t) &&
        eh->e_shoff + (uint64_t)eh->e_shnum * sizeof(elf64_shdr_t) <= rd) {
        const elf64_shdr_t *sh = (const elf64_shdr_t*)(buf + eh->e_shoff);
        for (uint16_t si = 0; si < eh->e_shnum; si++) {
            if (sh[si].sh_offset + sh[si].sh_size > rd) continue;
            if (sh[si].sh_type == SHT_RELA) {
                const elf64_rela_t *rela = (const elf64_rela_t*)(buf + sh[si].sh_offset);
                size_t n = (size_t)(sh[si].sh_size / sizeof(elf64_rela_t));
                for (size_t i = 0; i < n; i++) {
                    uint32_t t = ELF64_R_TYPE(rela[i].r_info);
                    if (t != R_X86_64_GOTPCREL && t != R_X86_64_GOTPCRELX && t != R_X86_64_REX_GOTPCRELX) continue;
                    (void)sqrm_got_add_slot(got, &got_cnt, SQRM_GOT_MAX_SLOTS, 0, ELF64_R_SYM(rela[i].r_info), got_base);
                }
            } else if (sh[si].sh_type == SHT_REL) {
                const elf64_rel_t *rel = (const elf64_rel_t*)(buf + sh[si].sh_offset);
                size_t n = (size_t)(sh[si].sh_size / sizeof(elf64_rel_t));
                for (size_t i = 0; i < n; i++) {
                    uint32_t t = ELF64_R_TYPE(rel[i].r_info);
                    if (t != R_X86_64_GOTPCREL && t != R_X86_64_GOTPCRELX && t != R_X86_64_REX_GOTPCRELX) continue;
                    (void)sqrm_got_add_slot(got, &got_cnt, SQRM_GOT_MAX_SLOTS, 0, ELF64_R_SYM(rel[i].r_info), got_base);
                }
            }
        }

        // Fill section slots from section SYMTAB.
        const elf64_shdr_t *symtab = NULL;
        for (uint16_t i = 0; i < eh->e_shnum; i++) {
            if (sh[i].sh_type == SHT_SYMTAB) { symtab = &sh[i]; break; }
        }
        if (symtab && symtab->sh_offset + symtab->sh_size <= rd) {
            const elf64_sym_t *syms = (const elf64_sym_t*)(buf + symtab->sh_offset);
            size_t n_syms = (size_t)(symtab->sh_size / sizeof(elf64_sym_t));

            const elf64_shdr_t *strtab = NULL;
            const char *strings = NULL;
            if (symtab->sh_link < eh->e_shnum) {
                strtab = &sh[symtab->sh_link];
                if (strtab->sh_type == SHT_STRTAB && strtab->sh_offset + strtab->sh_size <= rd) {
                    strings = (const char*)(buf + strtab->sh_offset);
                }
            }

            for (size_t gi = 0; gi < got_cnt; gi++) {
                if (got[gi].is_dyn != 0) continue;
                uint32_t sym = got[gi].sym;
                if (sym >= n_syms) continue;

                if (syms[sym].st_shndx == SHN_UNDEF) {
                    const char *nm = (strings && strtab && syms[sym].st_name < strtab->sh_size) ? (strings + syms[sym].st_name) : NULL;
                    uint64_t S = sqrm_resolve_kernel_symbol(nm);
                    if (S) got_base[gi] = S;
                    continue;
                }

                uint64_t sym_va = syms[sym].st_value;
                uint64_t so = 0;
                if (sym_va && sqrm_map_va_to_off(sym_va, min_v, max_v, img_sz_aligned, &so) == 0) {
                    got_base[gi] = (uint64_t)(image + so);
                }
            }
        }
    }

    // Scan dynamic relocation tables for GOTPCREL* (dynsym-indexed)
    const elf64_sym_t *dynsyms = NULL;
    size_t dynsym_count = 0;
    for (uint16_t pi = 0; pi < eh->e_phnum; pi++) {
        if (ph[pi].p_type != PT_DYNAMIC) continue;
        uint64_t dyn_off = 0;
        if (sqrm_map_va_to_off(ph[pi].p_vaddr, min_v, max_v, img_sz_aligned, &dyn_off) != 0) break;
        const elf64_dyn_t *dyn = (const elf64_dyn_t*)(image + dyn_off);
        size_t dyn_cnt = (size_t)(ph[pi].p_memsz / sizeof(elf64_dyn_t));
        uint64_t symtab_va = 0; uint64_t syment = sizeof(elf64_sym_t);
        uint64_t strtab_va = 0; uint64_t strsz = 0;
        const char *dynstr = NULL;
        uint64_t rela_va = 0, rela_sz = 0, rela_ent = sizeof(elf64_rela_t);
        uint64_t rel_va = 0, rel_sz = 0, rel_ent = sizeof(elf64_rel_t);
        for (size_t di = 0; di < dyn_cnt; di++) {
            if (dyn[di].d_tag == DT_NULL) break;
            if (dyn[di].d_tag == DT_SYMTAB) symtab_va = dyn[di].d_un.d_ptr;
            else if (dyn[di].d_tag == DT_SYMENT) syment = dyn[di].d_un.d_val;
            else if (dyn[di].d_tag == DT_STRTAB) strtab_va = dyn[di].d_un.d_ptr;
            else if (dyn[di].d_tag == DT_STRSZ) strsz = dyn[di].d_un.d_val;
            else if (dyn[di].d_tag == DT_RELA) rela_va = dyn[di].d_un.d_ptr;
            else if (dyn[di].d_tag == DT_RELASZ) rela_sz = dyn[di].d_un.d_val;
            else if (dyn[di].d_tag == DT_RELAENT) rela_ent = dyn[di].d_un.d_val;
            else if (dyn[di].d_tag == DT_REL) rel_va = dyn[di].d_un.d_ptr;
            else if (dyn[di].d_tag == DT_RELSZ) rel_sz = dyn[di].d_un.d_val;
            else if (dyn[di].d_tag == DT_RELENT) rel_ent = dyn[di].d_un.d_val;
        }
        if (strtab_va && strsz) {
            uint64_t str_off = 0;
            if (sqrm_map_va_to_off(strtab_va, min_v, max_v, img_sz_aligned, &str_off) == 0 && str_off + strsz <= img_sz_aligned) {
                dynstr = (const char*)(image + str_off);
            }
        }

        if (symtab_va && syment == sizeof(elf64_sym_t)) {
            uint64_t sym_off = 0;
            if (sqrm_map_va_to_off(symtab_va, min_v, max_v, img_sz_aligned, &sym_off) == 0) {
                dynsyms = (const elf64_sym_t*)(image + sym_off);
                dynsym_count = (size_t)((img_sz_aligned - sym_off) / sizeof(elf64_sym_t));
            }
        }
        if (rela_va && rela_sz && rela_ent == sizeof(elf64_rela_t)) {
            uint64_t roff = 0;
            if (sqrm_map_va_to_off(rela_va, min_v, max_v, img_sz_aligned, &roff) == 0 && roff + rela_sz <= img_sz_aligned) {
                const elf64_rela_t *rela = (const elf64_rela_t*)(image + roff);
                size_t n = (size_t)(rela_sz / sizeof(elf64_rela_t));
                for (size_t ri = 0; ri < n; ri++) {
                    uint32_t t = ELF64_R_TYPE(rela[ri].r_info);
                    if (t != R_X86_64_GOTPCREL && t != R_X86_64_GOTPCRELX && t != R_X86_64_REX_GOTPCRELX) continue;
                    (void)sqrm_got_add_slot(got, &got_cnt, SQRM_GOT_MAX_SLOTS, 1, ELF64_R_SYM(rela[ri].r_info), got_base);
                }
            }
        }
        if (rel_va && rel_sz && rel_ent == sizeof(elf64_rel_t)) {
            uint64_t roff = 0;
            if (sqrm_map_va_to_off(rel_va, min_v, max_v, img_sz_aligned, &roff) == 0 && roff + rel_sz <= img_sz_aligned) {
                const elf64_rel_t *rel = (const elf64_rel_t*)(image + roff);
                size_t n = (size_t)(rel_sz / sizeof(elf64_rel_t));
                for (size_t ri = 0; ri < n; ri++) {
                    uint32_t t = ELF64_R_TYPE(rel[ri].r_info);
                    if (t != R_X86_64_GOTPCREL && t != R_X86_64_GOTPCRELX && t != R_X86_64_REX_GOTPCRELX) continue;
                    (void)sqrm_got_add_slot(got, &got_cnt, SQRM_GOT_MAX_SLOTS, 1, ELF64_R_SYM(rel[ri].r_info), got_base);
                }
            }
        }

        // Fill dyn slots from dynsym
        if (dynsyms && dynsym_count) {
            for (size_t gi = 0; gi < got_cnt; gi++) {
                if (got[gi].is_dyn != 1) continue;
                uint32_t sym = got[gi].sym;
                if (sym >= dynsym_count) continue;

                if (dynsyms[sym].st_shndx == SHN_UNDEF) {
                    const char *nm = (dynstr && dynsyms[sym].st_name < strsz) ? (dynstr + dynsyms[sym].st_name) : NULL;
                    uint64_t S = sqrm_resolve_kernel_symbol(nm);
                    if (S) {
                        got_base[gi] = S;
                        const char *m = sqrm_get_current_module_name();
                        if (m && strcmp(m, "hid") == 0 && nm && sqrm_sym_eq(nm, "memset")) {
                            com_write_string(COM1_PORT, "[SQRM] got hid ");
                            com_write_string(COM1_PORT, nm);
                            com_write_string(COM1_PORT, " -> ");
                            com_write_hex64(COM1_PORT, S);
                            com_write_string(COM1_PORT, "\n");
                        }
                    }
                    continue;
                }

                uint64_t sym_va = dynsyms[sym].st_value;
                uint64_t so = 0;
                if (sym_va && sqrm_map_va_to_off(sym_va, min_v, max_v, img_sz_aligned, &so) == 0) {
                    got_base[gi] = (uint64_t)(image + so);
                }
            }
        }

        break;
    }

    // Apply relocations (needed for pointers in .rodata like desc.name and ops function tables)
    // Prefer PT_DYNAMIC-based relocations (works even if section headers are stripped).
    // Trampoline allocator: use the tail end of GOT pages.
    sqrm_tramp_ctx_t tramp;
    memset(&tramp, 0, sizeof(tramp));
    tramp.low_water = (uint8_t*)(got_base + got_cnt); // keep clear of used GOT slots
    tramp.limit_high = ((uint8_t*)got_base) + got_pages;
    tramp.cursor = tramp.limit_high;

    int rel_dyn_rc = sqrm_apply_relocations_dynamic(eh, ph, image, img_sz_aligned, min_v, max_v, got, got_cnt, &tramp);
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
    int rel_sh_rc = sqrm_apply_relocations(buf, rd, eh, min_v, max_v, image, img_sz_aligned, got, got_cnt, &tramp);
    if (rel_sh_rc != 0) {
        com_write_string(COM1_PORT, "[SQRM] note: sh relocations rc=");
        char rbuf2[16];
        itoa(rel_sh_rc, rbuf2, 10);
        com_write_string(COM1_PORT, rbuf2);
        com_write_string(COM1_PORT, " for ");
        com_write_string(COM1_PORT, basename);
        com_write_string(COM1_PORT, "\n");
    }

    // GOT metadata no longer needed after relocations are applied.
    kfree(got);
    got = NULL;

    // Find and validate module descriptor
    sqrm_module_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    int dr = sqrm_find_desc(buf, rd, eh, min_v, image, &desc);

    // If this is an ABI v2 module, locate the full v2 descriptor in-image.
    const sqrm_module_desc_v2_t *desc_v2 = NULL;
    (void)sqrm_find_desc_ptr_in_image(buf, rd, eh, min_v, image, &desc_v2);
    if (dr != 0 || desc.abi_version == 0 || !desc.name) {
        com_write_string(COM1_PORT, "[SQRM] Missing/invalid sqrm_module_desc in ");
        com_write_string(COM1_PORT, basename);
        com_write_string(COM1_PORT, "\n");
        kfree(image);
        kfree(buf);
        return -12;
    }

    // Strict: reject unknown module types
    if (desc.type != SQRM_TYPE_FS && desc.type != SQRM_TYPE_DRIVE && desc.type != SQRM_TYPE_USB && desc.type != SQRM_TYPE_AUDIO &&
        desc.type != SQRM_TYPE_GPU && desc.type != SQRM_TYPE_NET && desc.type != SQRM_TYPE_HID && desc.type != SQRM_TYPE_GENERIC) {
        com_write_string(COM1_PORT, "[SQRM] Unknown module type in ");
        com_write_string(COM1_PORT, basename);
        com_write_string(COM1_PORT, "\n");
        kfree(image);
        kfree(buf);
        return -12;
    }

    // Resolve and load dependencies (ABI v2 only) BEFORE calling module init.
    if (desc.abi_version == SQRM_ABI_V2) {
        if (!desc_v2) {
            com_write_string(COM1_PORT, "[SQRM] v2 descriptor pointer not found for ");
            com_write_string(COM1_PORT, basename);
            com_write_string(COM1_PORT, "\n");
            kfree(image);
            kfree(buf);
            return -12;
        }
        // Sanity checks
        if (desc_v2->dep_count && !desc_v2->deps) {
            com_write_string(COM1_PORT, "[SQRM] v2 module has dep_count but deps=NULL in ");
            com_write_string(COM1_PORT, basename);
            com_write_string(COM1_PORT, "\n");
            kfree(image);
            kfree(buf);
            return -12;
        }

        // Dependency load stack for cycle detection.
        // Keep it dynamically sized so we don't truncate useful cycle detection information.
        size_t depth = 0;
        size_t stack_cap = (dep_stack && dep_depth) ? (dep_depth + 1) : 1;
        if (stack_cap > 256) stack_cap = 256;

        const char **stack = (const char**)kmalloc(stack_cap * sizeof(*stack));
        if (!stack) {
            kfree(image);
            kfree(buf);
            return -ENOMEM;
        }

        // Start with any inherited stack (if we are loading as a dependency ourselves).
        if (dep_stack && dep_depth) {
            if (dep_depth > stack_cap - 1) dep_depth = stack_cap - 1;
            for (size_t i = 0; i < dep_depth; i++) {
                stack[depth++] = dep_stack[i];
            }
        }

        // Add current module.
        if (depth < stack_cap) {
            stack[depth++] = desc.name;
        }

        for (uint16_t i = 0; i < desc_v2->dep_count; i++) {
            const char *raw_dep = desc_v2->deps[i];
            if (!raw_dep || !raw_dep[0]) continue;

            // Optional-dependency syntax: "? depname" (leading '?', optional whitespace).
            int optional = 0;
            const char *dep = raw_dep;
            if (dep[0] == '?') {
                optional = 1;
                dep++;
                while (*dep == ' ' || *dep == '\t') dep++;
            }
            if (!dep[0]) continue;

            com_write_string(COM1_PORT, optional ? "[SQRM] optional dep: " : "[SQRM] dep: ");
            com_write_string(COM1_PORT, desc.name);
            com_write_string(COM1_PORT, " -> ");
            com_write_string(COM1_PORT, dep);
            com_write_string(COM1_PORT, "\n");

            int ldr = sqrm_load_module_by_desc_name_recursive(dep, stack, depth);
            if (ldr != 0) {
                com_write_string(COM1_PORT, optional
                    ? "[SQRM] optional dependency unavailable (continuing): "
                    : "[SQRM] failed to load dependency: ");
                com_write_string(COM1_PORT, dep);
                com_write_string(COM1_PORT, " for module ");
                com_write_string(COM1_PORT, basename);
                com_write_string(COM1_PORT, "\n");

                if (optional) {
                    continue;
                }

                kfree((void*)stack);
                kfree(image);
                kfree(buf);
                return ldr;
            }
        }

        kfree((void*)stack);
    }

    // Resolve the init function. The authoritative source is the 'sqrm_init' symbol in
    // .symtab — this is the convention for all in-tree and SDK modules. e_entry is used
    // only as a fallback for modules that set it explicitly and have no sqrm_init symbol.
    sqrm_module_init_fn init = NULL;

    if (eh->e_shoff != 0 && eh->e_shnum != 0 &&
        eh->e_shentsize == sizeof(elf64_shdr_t) &&
        eh->e_shoff + (uint64_t)eh->e_shnum * sizeof(elf64_shdr_t) <= rd) {

        const elf64_shdr_t *sh = (const elf64_shdr_t *)(buf + eh->e_shoff);
        const elf64_shdr_t *symtab_sh = NULL;
        const elf64_shdr_t *strtab_sh = NULL;

        for (uint16_t i = 0; i < eh->e_shnum; i++) {
            if (sh[i].sh_type == SHT_SYMTAB) {
                symtab_sh = &sh[i];
                if (symtab_sh->sh_link < eh->e_shnum)
                    strtab_sh = &sh[symtab_sh->sh_link];
                break;
            }
        }

        if (symtab_sh && strtab_sh &&
            strtab_sh->sh_type == SHT_STRTAB &&
            symtab_sh->sh_offset + symtab_sh->sh_size <= rd &&
            strtab_sh->sh_offset + strtab_sh->sh_size <= rd) {

            const elf64_sym_t *syms = (const elf64_sym_t *)(buf + symtab_sh->sh_offset);
            size_t n_syms = (size_t)(symtab_sh->sh_size / sizeof(elf64_sym_t));
            const char *strings = (const char *)(buf + strtab_sh->sh_offset);

            for (size_t i = 0; i < n_syms; i++) {
                uint32_t noff = syms[i].st_name;
                if (noff >= strtab_sh->sh_size) continue;
                if (strcmp(strings + noff, "sqrm_init") != 0) continue;

                uint64_t sym_va = syms[i].st_value;
                uint64_t sym_off = 0;
                if (sqrm_map_va_to_off(sym_va, min_v, max_v, img_sz_aligned, &sym_off) == 0) {
                    init = (sqrm_module_init_fn)(void *)(image + sym_off);
                }
                break;
            }
        }
    }

    // Fall back to e_entry if sqrm_init was not found in .symtab.
    if (!init) {
        if (eh->e_entry < min_v || eh->e_entry >= max_v) {
            kfree(image);
            kfree(buf);
            return -13;
        }
        init = (sqrm_module_init_fn)(void *)(image + (eh->e_entry - min_v));
    }

    com_write_string(COM1_PORT, "[SQRM] Loading module: ");
    com_write_string(COM1_PORT, basename);
    com_write_string(COM1_PORT, "\n");

    // Allocate a stable per-module capability-gated API table.
    // Modules are allowed to keep the pointer they receive in sqrm_module_init().
    int load_cap_rc = sqrm_loaded_ensure_capacity(g_loaded_count + 1);
    if (load_cap_rc != 0) {
        kfree(image);
        kfree(buf);
        return load_cap_rc;
    }

    size_t slot_idx = g_loaded_count;
    memset(&g_loaded[slot_idx], 0, sizeof(g_loaded[slot_idx]));
    strncpy(g_loaded[slot_idx].name, basename, sizeof(g_loaded[slot_idx].name) - 1);
    g_loaded[slot_idx].name[sizeof(g_loaded[slot_idx].name) - 1] = 0;
    g_loaded[slot_idx].base = image;
    g_loaded[slot_idx].size = img_sz_aligned;
    g_loaded[slot_idx].desc = desc;
    g_loaded[slot_idx].desc_v2 = (desc.abi_version == SQRM_ABI_V2) ? desc_v2 : NULL;

    g_loaded[slot_idx].api = (sqrm_kernel_api_t *)kmalloc(sizeof(sqrm_kernel_api_t));
    if (!g_loaded[slot_idx].api) {
        kfree(image);
        kfree(buf);
        return -ENOMEM;
    }
    sqrm_build_api(&desc, g_loaded[slot_idx].api);
    sqrm_kernel_api_t *mod_api = g_loaded[slot_idx].api;

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

    if (rc != 0) {
        kfree(image);
        kfree(buf);
        memset(&g_loaded[slot_idx], 0, sizeof(g_loaded[slot_idx]));
        return rc;
    }

    if (desc.type == SQRM_TYPE_NET) {
        sqrm_net_autoregister_devfs(&desc);
    }

    // Commit loaded entry.
    g_loaded_count++;

    // Keep image resident; free file buffer.
    kfree(buf);
    return 0;
}

// Disk-backed entry point: reads the file from SQRM_MODULE_DIR on the boot
// mount, then hands the raw bytes to the shared in-memory loader.
static int sqrm_load_one(const char *path, const char *basename, const sqrm_kernel_api_t *unused_api,
                         const char **dep_stack, size_t dep_depth) {
    (void)unused_api;
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

    return sqrm_load_one_from_memory(buf, rd, basename, dep_stack, dep_depth);
}

static int sqrm_peek_desc(const char *path, const char *basename, sqrm_module_desc_t *out_desc) {
    if (!out_desc) return -1;
    memset(out_desc, 0, sizeof(*out_desc));

    fs_mount_t *mnt = kernel_get_boot_mount();
    if (!mnt || !mnt->valid) return -2;

    fs_file_info_t st;
    if (fs_stat(mnt, path, &st) != 0 || st.is_directory || st.size < sizeof(elf64_ehdr_t)) {
        return -3;
    }

    uint8_t *buf = (uint8_t*)kmalloc(st.size);
    if (!buf) return -4;

    size_t rd = 0;
    if (fs_read_file(mnt, path, buf, st.size, &rd) != 0 || rd < sizeof(elf64_ehdr_t)) {
        kfree(buf);
        return -5;
    }

    const elf64_ehdr_t *eh = (const elf64_ehdr_t*)buf;
    if (!(eh->e_ident[0] == 0x7F && eh->e_ident[1] == 'E' && eh->e_ident[2] == 'L' && eh->e_ident[3] == 'F')) {
        kfree(buf);
        return -6;
    }

    // Find descriptor symbol in SHT_SYMTAB (same requirement as full loader).
    if (eh->e_shoff == 0 || eh->e_shnum == 0 || eh->e_shentsize != sizeof(elf64_shdr_t)) {
        kfree(buf);
        return -7;
    }
    if (eh->e_shoff + (uint64_t)eh->e_shnum * sizeof(elf64_shdr_t) > rd) {
        kfree(buf);
        return -8;
    }

    const elf64_shdr_t *sh = (const elf64_shdr_t*)(buf + eh->e_shoff);
    const elf64_shdr_t *symtab = NULL;
    const elf64_shdr_t *strtab = NULL;
    for (uint16_t i = 0; i < eh->e_shnum; i++) {
        if (sh[i].sh_type == SHT_SYMTAB) {
            symtab = &sh[i];
            if (symtab->sh_link < eh->e_shnum) strtab = &sh[symtab->sh_link];
            break;
        }
    }

    if (!symtab || !strtab || strtab->sh_type != SHT_STRTAB) {
        kfree(buf);
        return -9;
    }
    if (symtab->sh_offset + symtab->sh_size > rd || strtab->sh_offset + strtab->sh_size > rd) {
        kfree(buf);
        return -10;
    }

    const char *strings = (const char*)(buf + strtab->sh_offset);
    const elf64_sym_t *syms = (const elf64_sym_t*)(buf + symtab->sh_offset);
    size_t n_syms = (size_t)(symtab->sh_size / sizeof(elf64_sym_t));

    uint64_t desc_va = 0;
    for (size_t i = 0; i < n_syms; i++) {
        uint32_t noff = syms[i].st_name;
        if (noff >= strtab->sh_size) continue;
        const char *nm = strings + noff;
        if (strcmp(nm, SQRM_DESC_SYMBOL) == 0) {
            desc_va = syms[i].st_value;
            break;
        }
    }

    if (!desc_va) {
        com_write_string(COM1_PORT, "[SQRM] peek: Missing sqrm_module_desc in ");
        com_write_string(COM1_PORT, basename);
        com_write_string(COM1_PORT, "\n");
        kfree(buf);
        return -11;
    }

    // Translate descriptor VA to file offset using PT_LOAD program headers.
    if (eh->e_phoff == 0 || eh->e_phnum == 0 || eh->e_phentsize != sizeof(elf64_phdr_t) ||
        eh->e_phoff + (uint64_t)eh->e_phnum * sizeof(elf64_phdr_t) > rd) {
        kfree(buf);
        return -12;
    }

    const elf64_phdr_t *ph = (const elf64_phdr_t*)(buf + eh->e_phoff);
    uint64_t file_off = UINT64_MAX;
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        // Use p_filesz for file mapping.
        if (desc_va >= ph[i].p_vaddr && desc_va + sizeof(sqrm_module_desc_t) <= ph[i].p_vaddr + ph[i].p_filesz) {
            file_off = ph[i].p_offset + (desc_va - ph[i].p_vaddr);
            break;
        }
    }

    if (file_off == UINT64_MAX || file_off + sizeof(sqrm_module_desc_t) > rd) {
        kfree(buf);
        return -13;
    }

    // NOTE: This is the raw descriptor from the file image. Pointers inside (like name)
    // are not guaranteed to be usable without relocations. For filtering we only need type/abi.
    const sqrm_module_desc_t *d = (const sqrm_module_desc_t*)(buf + file_off);
    out_desc->abi_version = d->abi_version;
    out_desc->type = d->type;

    kfree(buf);
    return 0;
}

static int sqrm_load_filtered(int (*want_type)(sqrm_module_type_t type)) {
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

        // Peek descriptor to decide whether to load.
        sqrm_module_desc_t dsc;
        memset(&dsc, 0, sizeof(dsc));
        if (sqrm_peek_desc(full, ent.name, &dsc) == 0) {
            if (want_type && !want_type(dsc.type)) {
                continue;
            }
        }

        int lr = sqrm_load_one(full, ent.name, &api, NULL, 0);
        if (lr == 0) loaded_any = 1;
    }
    fs_closedir(d);

    if (!loaded_any) {
        com_write_string(COM1_PORT, "[SQRM] No modules loaded\n");
    }

    return 0;
}

/* --- Multiboot2 "initmod.tpk" bootstrap module ---
 *
 * The bootloader may hand the kernel a small TPK (Tiny Package Archive; see
 * LTF-Spec section 1.1) as a Multiboot2 module. Its .sqrm entries are loaded
 * straight out of the module's memory, before anything is scanned from
 * SQRM_MODULE_DIR on the boot drive -- and before any boot drive is even
 * required to exist. Layout:
 *   offset 0: uint32_t magic ('TPK!' = 0x214B5054)
 *   offset 4: uint32_t file_count
 *   then file_count entries of 264 bytes: char name[256], u32 offset, u32 size
 *   followed by the raw file data.
 */
#define TPK_MAGIC      0x214B5054u
#define TPK_NAME_LEN   256u

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t file_count;
} tpk_header_t;

typedef struct __attribute__((packed)) {
    char name[TPK_NAME_LEN];
    uint32_t offset;
    uint32_t size;
} tpk_entry_t;

int sqrm_load_initmod(void) {
    uint64_t mod_phys = 0, mod_len = 0;
    if (multiboot2_get_initmod(&mod_phys, &mod_len) != 0 || mod_len < sizeof(tpk_header_t)) {
        com_write_string(COM1_PORT, "[SQRM] No initmod.tpk multiboot module supplied\n");
        return 0;
    }

    const uint8_t *base = (const uint8_t*)phys_to_virt_kernel(mod_phys);
    if (!base) {
        com_write_string(COM1_PORT, "[SQRM] initmod.tpk: could not map module memory\n");
        return -1;
    }

    const tpk_header_t *hdr = (const tpk_header_t*)base;
    if (hdr->magic != TPK_MAGIC) {
        com_write_string(COM1_PORT, "[SQRM] initmod.tpk: bad magic, ignoring\n");
        return -2;
    }

    uint64_t table_bytes = (uint64_t)hdr->file_count * (uint64_t)sizeof(tpk_entry_t);
    if (sizeof(tpk_header_t) + table_bytes > mod_len) {
        com_write_string(COM1_PORT, "[SQRM] initmod.tpk: truncated entry table, ignoring\n");
        return -3;
    }

    com_write_string(COM1_PORT, "[SQRM] initmod.tpk: ");
    char cbuf[16];
    itoa((int)hdr->file_count, cbuf, 10);
    com_write_string(COM1_PORT, cbuf);
    com_write_string(COM1_PORT, " file(s)\n");

    const tpk_entry_t *entries = (const tpk_entry_t*)(base + sizeof(tpk_header_t));
    int loaded_any = 0;

    for (uint32_t i = 0; i < hdr->file_count; i++) {
        char name[TPK_NAME_LEN];
        memcpy(name, entries[i].name, TPK_NAME_LEN);
        name[TPK_NAME_LEN - 1] = 0;

        if (!ends_with(name, ".sqrm")) continue;

        uint64_t data_off = entries[i].offset;
        uint64_t data_sz  = entries[i].size;
        if (data_off > mod_len || data_sz > mod_len - data_off) {
            com_write_string(COM1_PORT, "[SQRM] initmod.tpk: entry out of range: ");
            com_write_string(COM1_PORT, name);
            com_write_string(COM1_PORT, "\n");
            continue;
        }
        if (data_sz < sizeof(elf64_ehdr_t)) continue;

        uint8_t *buf = (uint8_t*)kmalloc((size_t)data_sz);
        if (!buf) {
            com_write_string(COM1_PORT, "[SQRM] initmod.tpk: OOM copying ");
            com_write_string(COM1_PORT, name);
            com_write_string(COM1_PORT, "\n");
            continue;
        }
        memcpy(buf, base + data_off, (size_t)data_sz);

        com_write_string(COM1_PORT, "[SQRM] initmod: loading ");
        com_write_string(COM1_PORT, name);
        com_write_string(COM1_PORT, "\n");

        int rc = sqrm_load_one_from_memory(buf, (size_t)data_sz, name, NULL, 0);
        if (rc == 0) {
            loaded_any = 1;
        } else {
            com_write_string(COM1_PORT, "[SQRM] initmod: failed to load ");
            com_write_string(COM1_PORT, name);
            com_write_string(COM1_PORT, " rc=");
            char rbuf[16];
            itoa(rc, rbuf, 10);
            com_write_string(COM1_PORT, rbuf);
            com_write_string(COM1_PORT, "\n");
        }
    }

    if (!loaded_any) {
        com_write_string(COM1_PORT, "[SQRM] initmod.tpk: no modules loaded\n");
    }

    return 0;
}

static int want_gpu_only(sqrm_module_type_t t) { return t == SQRM_TYPE_GPU; }
static int want_fs_only(sqrm_module_type_t t) { return t == SQRM_TYPE_FS; }
static int want_drive_only(sqrm_module_type_t t) { return t == SQRM_TYPE_DRIVE; }
static int want_late(sqrm_module_type_t t) {
    // Late: everything except GPU, FS and DRIVE (those load early).
    return !(t == SQRM_TYPE_GPU || t == SQRM_TYPE_FS || t == SQRM_TYPE_DRIVE);
}

int sqrm_load_early_drivers(void) {
    // Bootstrap modules bundled in the multiboot2 initmod.tpk (if any) load
    // first, ahead of anything scanned from the boot drive.
    sqrm_load_initmod();

    com_write_string(COM1_PORT, "[SQRM] Early load: GPU\n");
    sqrm_load_filtered(want_gpu_only);

    // DRIVE modules (third-party storage controller drivers) load here,
    // scanned from the boot drive's SQRM_MODULE_DIR like every other .sqrm
    // module. This means a DRIVE module cannot itself back the boot drive --
    // something has to be readable before .sqrm files can be found on it --
    // but it can bring up any additional/secondary controller (a second
    // AHCI HBA, NVMe, etc.) whose drives are then picked up by the
    // fs_rescan_all() the caller runs right after this returns.
    com_write_string(COM1_PORT, "[SQRM] Early load: DRIVE\n");
    sqrm_load_filtered(want_drive_only);

    com_write_string(COM1_PORT, "[SQRM] Early load: FS\n");
    sqrm_load_filtered(want_fs_only);
    return 0;
}

int sqrm_load_late_drivers(void) {
    com_write_string(COM1_PORT, "[SQRM] Late load\n");
    return sqrm_load_filtered(want_late);
}

int sqrm_load_all(void) {
    // Backwards compatible: load early then late.
    sqrm_load_early_drivers();
    return sqrm_load_late_drivers();
}