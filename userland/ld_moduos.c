/*
 * ld-moduos.sqrl - ModuOS dynamic linker (the ld-linux.so equivalent).
 *
 * The kernel maps the executable (SQR, ET_EXEC) and this interpreter into one
 * address space and enters here. The executable's program headers and entry
 * point arrive through SYS_DLINFO. This file then:
 *   1. loads every DT_NEEDED library (SQRL, ET_DYN) breadth-first,
 *   2. applies x86-64 RELA relocations against the global scope,
 *   3. runs initialisers dependencies-first,
 *   4. jumps to the executable's entry with the original argc/argv.
 *
 * Deliberately freestanding: no libc.h, no malloc. The application's heap
 * must not be touched before it starts, and a second allocator sharing the
 * brk area would corrupt it. All state is static; library images come from
 * anonymous mmap (non-fixed, so the kernel places them in the mmap region).
 *
 * Search order: LD_LIBRARY_PATH (colon separated), then /ModuOS/shared/lib.
 * Limits: DT_HASH only (link with --hash-style=sysv), eager binding, no TLS,
 * no symbol versioning, no dlopen. Executables must be ET_EXEC.
 */

#include <stddef.h>
#include <stdint.h>

#include "../include/moduos/kernel/loader/dlinfo.h"
#include "../include/moduos/kernel/syscall/syscall_numbers.h"

typedef uint64_t u64;
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;

#define LD_DEFAULT_DIR "/ModuOS/shared/lib"
#define LD_MAX_OBJECTS 64
#define LD_MAX_NEEDED 16
#define LD_PATH_MAX 256
#define LD_HDR_MAX 8192

#define PT_LOAD 1
#define PT_DYNAMIC 2

#define ET_DYN 3
#define EM_X86_64 62

#define DT_NULL 0
#define DT_NEEDED 1
#define DT_PLTRELSZ 2
#define DT_HASH 4
#define DT_STRTAB 5
#define DT_SYMTAB 6
#define DT_RELA 7
#define DT_RELASZ 8
#define DT_RELAENT 9
#define DT_STRSZ 10
#define DT_INIT 12
#define DT_JMPREL 23
#define DT_INIT_ARRAY 25
#define DT_INIT_ARRAYSZ 27

#define SHN_UNDEF 0
#define STB_LOCAL 0
#define STB_WEAK 2
#define STT_TLS 6

#define R_X86_64_NONE 0
#define R_X86_64_64 1
#define R_X86_64_COPY 5
#define R_X86_64_GLOB_DAT 6
#define R_X86_64_JUMP_SLOT 7
#define R_X86_64_RELATIVE 8

#define MMAP_FIXED 0x1
#define MMAP_ANON 0x2
#define PROT_RWX 7

typedef struct {
    u8 e_ident[16];
    u16 e_type;
    u16 e_machine;
    u32 e_version;
    u64 e_entry;
    u64 e_phoff;
    u64 e_shoff;
    u32 e_flags;
    u16 e_ehsize;
    u16 e_phentsize;
    u16 e_phnum;
    u16 e_shentsize;
    u16 e_shnum;
    u16 e_shstrndx;
} ld_ehdr_t;

typedef struct {
    u32 p_type;
    u32 p_flags;
    u64 p_offset;
    u64 p_vaddr;
    u64 p_paddr;
    u64 p_filesz;
    u64 p_memsz;
    u64 p_align;
} ld_phdr_t;

typedef struct {
    u64 d_tag;
    u64 d_val;
} ld_dyn_t;

typedef struct {
    u64 r_offset;
    u64 r_info;
    int64_t r_addend;
} ld_rela_t;

typedef struct {
    u32 st_name;
    u8 st_info;
    u8 st_other;
    u16 st_shndx;
    u64 st_value;
    u64 st_size;
} ld_sym_t;

typedef struct ld_object ld_object_t;
struct ld_object {
    char name[LD_PATH_MAX];
    u64 base;
    const ld_dyn_t *dyn;
    const char *strtab;
    u64 strsz;
    const ld_sym_t *symtab;
    const u32 *hash;
    const ld_rela_t *rela;
    u64 relasz;
    const ld_rela_t *jmprel;
    u64 jmprelsz;
    void (*init_fn)(void);
    void (**init_array)(void);
    u64 init_arraysz;
    u32 needed[LD_MAX_NEEDED];
    u32 needed_count;
    ld_object_t *deps[LD_MAX_NEEDED];
    int is_exe;
    int initialised;
};

/* Load order == global symbol scope order (executable first). */
static ld_object_t g_objects[LD_MAX_OBJECTS];
static u32 g_object_count;
static u8 g_hdr[LD_HDR_MAX];
static char g_libpath[LD_PATH_MAX * 2];

typedef void (*ld_entry_fn_t)(long argc, char **argv, char **envp);

void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);

/* The compiler may lower struct copies and zeroing to these. */
void *memcpy(void *dst, const void *src, size_t n) {
    u8 *d = dst;
    const u8 *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memset(void *dst, int c, size_t n) {
    u8 *d = dst;
    while (n--) *d++ = (u8)c;
    return dst;
}

static inline long ld_syscall(long num, long a1, long a2, long a3, long a4, long a5) {
    register long r10 __asm__("r10") = a4;
    register long r8 __asm__("r8") = a5;
    long ret;

    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(num), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
                     : "rcx", "r11", "memory");
    return ret;
}

static size_t ld_strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static int ld_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (u8)*a - (u8)*b;
}

/* Bounded append; returns 0 on truncation so callers can skip the candidate. */
static int ld_append(char *dst, size_t cap, const char *src) {
    size_t n = ld_strlen(dst);
    size_t m = ld_strlen(src);
    if (n + m + 1 > cap) return 0;
    memcpy(dst + n, src, m + 1);
    return 1;
}

static void ld_write(const char *s) {
    ld_syscall(SYS_WRITE, 1, (long)s, (long)ld_strlen(s), 0, 0);
}

static __attribute__((noreturn)) void ld_die(const char *msg, const char *arg) {
    ld_write("ld-moduos: ");
    ld_write(msg);
    if (arg) {
        ld_write(": ");
        ld_write(arg);
    }
    ld_write("\n");
    ld_syscall(SYS_EXIT, 127, 0, 0, 0, 0);
    for (;;) {
    }
}

static int ld_mmap_failed(long r) {
    return (u64)r >= (u64)-4095LL;
}

static u32 ld_elf_hash(const char *name) {
    u32 h = 0;
    while (*name) {
        h = (h << 4) + (u8)*name++;
        u32 g = h & 0xf0000000u;
        if (g) h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

/* A JUMP_SLOT whose symbol is missing binds here, so a never-called import
 * does not stop the program (matches what lazy binding would have done). */
static __attribute__((noreturn)) void ld_unresolved_trap(void) {
    ld_die("call through unresolved symbol", NULL);
}

static void ld_parse_dynamic(ld_object_t *o) {
    const ld_dyn_t *d;

    for (d = o->dyn; d->d_tag != DT_NULL; d++) {
        u64 v = d->d_val;
        switch (d->d_tag) {
        case DT_STRTAB: o->strtab = (const char *)(o->base + v); break;
        case DT_STRSZ: o->strsz = v; break;
        case DT_SYMTAB: o->symtab = (const ld_sym_t *)(o->base + v); break;
        case DT_HASH: o->hash = (const u32 *)(o->base + v); break;
        case DT_RELA: o->rela = (const ld_rela_t *)(o->base + v); break;
        case DT_RELASZ: o->relasz = v; break;
        case DT_JMPREL: o->jmprel = (const ld_rela_t *)(o->base + v); break;
        case DT_PLTRELSZ: o->jmprelsz = v; break;
        case DT_INIT: o->init_fn = (void (*)(void))(o->base + v); break;
        case DT_INIT_ARRAY: o->init_array = (void (**)(void))(o->base + v); break;
        case DT_INIT_ARRAYSZ: o->init_arraysz = v; break;
        default: break;
        }
    }

    /* DT_NEEDED holds strtab offsets, so it needs DT_STRTAB already known. */
    for (d = o->dyn; d->d_tag != DT_NULL; d++) {
        if (d->d_tag != DT_NEEDED) continue;
        if (o->needed_count >= LD_MAX_NEEDED) ld_die("too many DT_NEEDED entries", o->name);
        o->needed[o->needed_count++] = (u32)d->d_val;
    }
}

static int ld_read_full(int fd, void *buf, u64 n) {
    u8 *p = buf;
    while (n) {
        long r = ld_syscall(SYS_READ, fd, (long)p, (long)n, 0, 0);
        if (r <= 0) return -1;
        p += r;
        n -= (u64)r;
    }
    return 0;
}

static ld_object_t *ld_alloc_object(const char *name) {
    if (g_object_count >= LD_MAX_OBJECTS) ld_die("too many loaded objects", name);
    ld_object_t *o = &g_objects[g_object_count++];
    memset(o, 0, sizeof(*o));
    if (ld_strlen(name) >= sizeof(o->name)) ld_die("name too long", name);
    memcpy(o->name, name, ld_strlen(name) + 1);
    return o;
}

/* Maps one ET_DYN image. Returns NULL if the file cannot be opened so the
 * caller can try the next search directory; any later failure is fatal. */
static ld_object_t *ld_map_library(const char *path, const char *soname) {
    int fd = (int)ld_syscall(SYS_OPEN, (long)path, 0, 0, 0, 0);
    if (fd < 0) return NULL;

    long got = ld_syscall(SYS_READ, fd, (long)g_hdr, LD_HDR_MAX, 0, 0);
    const ld_ehdr_t *eh = (const ld_ehdr_t *)g_hdr;
    if (got < (long)sizeof(*eh) || eh->e_ident[0] != 0x7f || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F' || eh->e_ident[4] != 2 ||
        eh->e_machine != EM_X86_64 || eh->e_type != ET_DYN)
        ld_die("not an x86-64 shared object", path);

    u64 phsz = (u64)eh->e_phnum * sizeof(ld_phdr_t);
    if (eh->e_phentsize != sizeof(ld_phdr_t) || eh->e_phoff + phsz > (u64)got)
        ld_die("program headers out of range", path);
    const ld_phdr_t *ph = (const ld_phdr_t *)(g_hdr + eh->e_phoff);

    u64 lo = ~0ULL, hi = 0;
    for (u16 i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD || ph[i].p_memsz == 0) continue;
        u64 s = ph[i].p_vaddr & ~0xFFFULL;
        u64 e = (ph[i].p_vaddr + ph[i].p_memsz + 0xFFFULL) & ~0xFFFULL;
        if (s < lo) lo = s;
        if (e > hi) hi = e;
    }
    if (lo == ~0ULL) ld_die("no loadable segments", path);

    long m = ld_syscall(SYS_MMAP, 0, (long)(hi - lo), PROT_RWX, MMAP_ANON, -1);
    if (ld_mmap_failed(m)) ld_die("out of address space for", path);

    ld_object_t *o = ld_alloc_object(soname);
    o->base = (u64)m - lo;

    for (u16 i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_DYNAMIC) o->dyn = (const ld_dyn_t *)(o->base + ph[i].p_vaddr);
        if (ph[i].p_type != PT_LOAD || ph[i].p_filesz == 0) continue;
        /* mmap already zero-filled the span, which covers .bss. */
        if (ld_syscall(SYS_LSEEK, fd, (long)ph[i].p_offset, 0, 0, 0) < 0 ||
            ld_read_full(fd, (void *)(o->base + ph[i].p_vaddr), ph[i].p_filesz) != 0)
            ld_die("short read", path);
    }
    ld_syscall(SYS_CLOSE, fd, 0, 0, 0, 0);

    if (!o->dyn) ld_die("missing PT_DYNAMIC", path);
    ld_parse_dynamic(o);
    return o;
}

static int ld_has_slash(const char *s) {
    for (; *s; s++)
        if (*s == '/') return 1;
    return 0;
}

static ld_object_t *ld_find_loaded(const char *soname) {
    for (u32 i = 0; i < g_object_count; i++)
        if (ld_strcmp(g_objects[i].name, soname) == 0) return &g_objects[i];
    return NULL;
}

static ld_object_t *ld_try_dir(const char *dir, size_t dir_len, const char *soname) {
    char path[LD_PATH_MAX];

    if (dir_len + 1 + ld_strlen(soname) + 1 > sizeof(path)) return NULL;
    memcpy(path, dir, dir_len);
    path[dir_len] = 0;
    if (dir_len == 0 || !ld_append(path, sizeof(path), "/")) return NULL;
    if (!ld_append(path, sizeof(path), soname)) return NULL;
    return ld_map_library(path, soname);
}

static ld_object_t *ld_load_needed(const char *soname) {
    ld_object_t *o = ld_find_loaded(soname);
    if (o) return o;

    if (ld_has_slash(soname)) {
        o = ld_map_library(soname, soname);
        if (!o) ld_die("cannot open library", soname);
        return o;
    }

    for (const char *p = g_libpath; *p;) {
        const char *end = p;
        while (*end && *end != ':') end++;
        if (end > p && (o = ld_try_dir(p, (size_t)(end - p), soname)) != NULL) return o;
        p = *end ? end + 1 : end;
    }

    o = ld_try_dir(LD_DEFAULT_DIR, sizeof(LD_DEFAULT_DIR) - 1, soname);
    if (!o) ld_die("cannot find library", soname);
    return o;
}

/* Breadth-first so the global scope matches the DT_NEEDED order the linker saw. */
static void ld_load_dependencies(void) {
    for (u32 i = 0; i < g_object_count; i++) {
        ld_object_t *o = &g_objects[i];
        for (u32 n = 0; n < o->needed_count; n++)
            o->deps[n] = ld_load_needed(o->strtab + o->needed[n]);
    }
}

static const ld_sym_t *ld_lookup_in(const ld_object_t *o, const char *name, u32 h) {
    if (!o->hash || !o->symtab || !o->strtab) return NULL;

    u32 nbucket = o->hash[0];
    const u32 *bucket = o->hash + 2;
    const u32 *chain = bucket + nbucket;

    if (nbucket == 0) return NULL;
    for (u32 i = bucket[h % nbucket]; i != 0; i = chain[i]) {
        const ld_sym_t *s = &o->symtab[i];
        if (s->st_shndx == SHN_UNDEF || (s->st_info & 0xF) == STT_TLS) continue;
        if (ld_strcmp(o->strtab + s->st_name, name) == 0) return s;
    }
    return NULL;
}

/* A strong definition anywhere beats an earlier weak one, as in ld.so. */
static int ld_resolve_symbol(const char *name, const ld_object_t *skip, u64 *out_addr,
                             u64 *out_size) {
    u32 h = ld_elf_hash(name);
    const ld_object_t *weak_obj = NULL;
    const ld_sym_t *weak_sym = NULL;

    for (u32 i = 0; i < g_object_count; i++) {
        const ld_object_t *o = &g_objects[i];
        if (o == skip) continue;
        const ld_sym_t *s = ld_lookup_in(o, name, h);
        if (!s) continue;
        if ((s->st_info >> 4) == STB_WEAK) {
            if (!weak_sym) {
                weak_sym = s;
                weak_obj = o;
            }
            continue;
        }
        *out_addr = o->base + s->st_value;
        if (out_size) *out_size = s->st_size;
        return 1;
    }
    if (weak_sym) {
        *out_addr = weak_obj->base + weak_sym->st_value;
        if (out_size) *out_size = weak_sym->st_size;
        return 1;
    }
    return 0;
}

static void ld_apply_relocs(ld_object_t *o, const ld_rela_t *r, u64 bytes) {
    for (u64 i = 0; i < bytes / sizeof(ld_rela_t); i++) {
        u32 type = (u32)(r[i].r_info & 0xffffffffu);
        u32 symi = (u32)(r[i].r_info >> 32);
        u64 *where = (u64 *)(o->base + r[i].r_offset);

        if (type == R_X86_64_NONE) continue;
        if (type == R_X86_64_RELATIVE) {
            *where = o->base + (u64)r[i].r_addend;
            continue;
        }
        if (type != R_X86_64_64 && type != R_X86_64_GLOB_DAT && type != R_X86_64_JUMP_SLOT &&
            type != R_X86_64_COPY)
            ld_die("unsupported relocation type", o->name);
        if (!o->symtab || !o->strtab) ld_die("relocation without symbol table", o->name);

        const ld_sym_t *sym = &o->symtab[symi];
        const char *name = o->strtab + sym->st_name;
        u64 addr = 0, size = 0;
        int found;

        if (type == R_X86_64_COPY) {
            /* Only the executable has these; the data lives in a library. */
            if (!ld_resolve_symbol(name, o, &addr, &size)) ld_die("undefined symbol", name);
            memcpy(where, (const void *)addr, sym->st_size ? sym->st_size : size);
            continue;
        }

        if ((sym->st_info >> 4) == STB_LOCAL && sym->st_shndx != SHN_UNDEF) {
            addr = o->base + sym->st_value;
            found = 1;
        } else {
            found = ld_resolve_symbol(name, NULL, &addr, NULL);
        }

        if (!found) {
            if ((sym->st_info >> 4) == STB_WEAK) {
                addr = 0;
            } else if (type == R_X86_64_JUMP_SLOT) {
                addr = (u64)(uintptr_t)ld_unresolved_trap;
            } else {
                ld_die("undefined symbol", name);
            }
        }

        *where = (type == R_X86_64_64) ? addr + (u64)r[i].r_addend : addr;
    }
}

/* Dependencies first, so a library's initialisers never see an uninitialised
 * dependency. */
static void ld_run_init(ld_object_t *o) {
    if (o->initialised) return;
    o->initialised = 1;

    for (u32 n = 0; n < o->needed_count; n++)
        if (o->deps[n]) ld_run_init(o->deps[n]);

    if (o->init_fn) o->init_fn();
    for (u64 i = 0; i < o->init_arraysz / sizeof(void *); i++)
        if (o->init_array[i]) o->init_array[i]();
}

static void ld_setup_executable(const dlinfo_t *info) {
    if (!info->exe_phdr || info->exe_phent != sizeof(ld_phdr_t))
        ld_die("executable program headers are not mapped", NULL);

    ld_object_t *o = ld_alloc_object("<executable>");
    o->is_exe = 1;
    o->base = 0;

    const ld_phdr_t *ph = (const ld_phdr_t *)info->exe_phdr;
    for (u64 i = 0; i < info->exe_phnum; i++)
        if (ph[i].p_type == PT_DYNAMIC) o->dyn = (const ld_dyn_t *)ph[i].p_vaddr;
    if (!o->dyn) ld_die("executable has PT_INTERP but no PT_DYNAMIC", NULL);
    ld_parse_dynamic(o);
}

__attribute__((noreturn, used)) void _start(long argc, char **argv, char **envp) {
    dlinfo_t info;

    if (ld_syscall(SYS_DLINFO, (long)&info, 0, 0, 0, 0) != 0)
        ld_die("kernel did not provide dynamic-link info", NULL);

    if (ld_syscall(SYS_GETENV, (long)"LD_LIBRARY_PATH", (long)g_libpath,
                   (long)sizeof(g_libpath) - 1, 0, 0) < 0)
        g_libpath[0] = 0;

    ld_setup_executable(&info);
    ld_load_dependencies();

    for (u32 i = 0; i < g_object_count; i++) {
        ld_object_t *o = &g_objects[i];
        if (o->rela) ld_apply_relocs(o, o->rela, o->relasz);
        if (o->jmprel) ld_apply_relocs(o, o->jmprel, o->jmprelsz);
    }

    /* Nothing else runs the executable's own init_array: its _start is the
     * application entry, there is no crt. */
    for (u32 i = g_object_count; i-- > 0;) ld_run_init(&g_objects[i]);

    ((ld_entry_fn_t)(uintptr_t)info.exe_entry)(argc, argv, envp);
    ld_syscall(SYS_EXIT, 0, 0, 0, 0, 0);
    for (;;) {
    }
}
