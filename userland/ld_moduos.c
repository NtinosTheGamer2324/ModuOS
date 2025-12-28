#include "libc.h"
#include "string.h"

/* ld-moduos: userland ELF interpreter (PT_INTERP) + dynamic linker.
 *
 * Kernel launches this program when an executable has PT_INTERP.
 * argv layout:
 *   argv[0] = interpreter path/name
 *   argv[1] = target executable path
 *   argv[2..] = original user arguments
 *
 * Features:
 * - Load ET_EXEC and ET_DYN (PIE) executables
 * - Load DT_NEEDED shared objects (.sqrl) from /ModuOS/shared/usr/lib
 * - Apply x86_64 RELA relocations:
 *     R_X86_64_RELATIVE, R_X86_64_64, R_X86_64_GLOB_DAT, R_X86_64_JUMP_SLOT
 *
 * Notes / limitations:
 * - Uses SysV DT_HASH to determine symbol count (we force --hash-style=sysv in build.sh).
 * - No TLS, no init/fini arrays, no lazy binding, no versioning.
 */

#define EI_NIDENT 16
#define PT_LOAD    1
#define PT_DYNAMIC 2

#define ET_EXEC 2
#define ET_DYN  3

#define PF_X 1
#define PF_W 2
#define PF_R 4

#define DT_NULL    0
#define DT_NEEDED  1
#define DT_HASH    4
#define DT_STRTAB  5
#define DT_SYMTAB  6
#define DT_RELA    7
#define DT_RELASZ  8
#define DT_RELAENT 9
#define DT_STRSZ   10

#define STB_LOCAL  0
#define STB_GLOBAL 1
#define STB_WEAK   2

#define SHN_UNDEF  0

#define ELF64_ST_BIND(i)   ((i) >> 4)
#define ELF64_ST_TYPE(i)   ((i) & 0xF)

#define ELF64_R_SYM(i)     ((uint32_t)((i) >> 32))
#define ELF64_R_TYPE(i)    ((uint32_t)((i) & 0xffffffffu))

/* x86_64 relocs */
#define R_X86_64_64       1
#define R_X86_64_GLOB_DAT 6
#define R_X86_64_JUMP_SLOT 7
#define R_X86_64_RELATIVE 8

typedef uint64_t u64;
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t  u8;

typedef struct {
    u8  e_ident[EI_NIDENT];
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
} __attribute__((packed)) ehdr_t;

typedef struct {
    u32 p_type;
    u32 p_flags;
    u64 p_offset;
    u64 p_vaddr;
    u64 p_paddr;
    u64 p_filesz;
    u64 p_memsz;
    u64 p_align;
} __attribute__((packed)) phdr_t;

typedef struct {
    u64 d_tag;
    u64 d_val;
} __attribute__((packed)) dyn_t;

typedef struct {
    u64 r_offset;
    u64 r_info;
    u64 r_addend;
} __attribute__((packed)) rela_t;

typedef struct {
    u32 st_name;
    u8  st_info;
    u8  st_other;
    u16 st_shndx;
    u64 st_value;
    u64 st_size;
} __attribute__((packed)) sym_t;

typedef struct so_obj so_obj_t;
struct so_obj {
    char path[256];
    u64 base;
    u64 entry;

    const dyn_t *dyn;

    const char *strtab;
    u64 strsz;

    const sym_t *symtab;
    u32 nsyms; /* from DT_HASH nchain */

    const u32 *hash; /* DT_HASH, points to nbucket,nchain,buckets[],chains[] */

    const rela_t *rela;
    u64 relasz;
    u64 relaent;

    char **needed; /* malloc'd copies */
    u32 needed_count;

    so_obj_t *next;
};

static inline void *mm_mmap(void *addr, size_t size, int prot, int flags) {
    return (void*)syscall4(SYS_MMAP, (long)addr, (long)size, (long)prot, (long)flags);
}
static inline int mm_munmap(void *addr, size_t size) {
    return (int)syscall(SYS_MUNMAP, (long)addr, (long)size, 0);
}

static u64 align_down(u64 v) { return v & ~0xFFFULL; }
static u64 align_up(u64 v) { return (v + 0xFFFULL) & ~0xFFFULL; }

static int read_all(int fd, void *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, (u8*)buf + got, n - got);
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}

static int load_file(const char *path, void **out_buf, size_t *out_sz) {
    fs_file_info_t st;
    if (stat(path, &st) != 0) return -1;
    if (st.is_directory) return -1;

    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return -1;

    void *b = malloc(st.size);
    if (!b) { close(fd); return -1; }

    if (read_all(fd, b, st.size) != 0) {
        close(fd);
        free(b);
        return -1;
    }
    close(fd);

    *out_buf = b;
    *out_sz = (size_t)st.size;
    return 0;
}

static int map_load_segments(const void *file, u64 base, u64 *out_entry, u64 *out_dyn_vaddr) {
    const ehdr_t *eh = (const ehdr_t*)file;
    const phdr_t *ph = (const phdr_t*)((const u8*)file + eh->e_phoff);

    *out_dyn_vaddr = 0;

    for (u16 i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_DYNAMIC) {
            *out_dyn_vaddr = base + ph[i].p_vaddr;
        }
        if (ph[i].p_type != PT_LOAD) continue;

        u64 seg_vaddr = base + ph[i].p_vaddr;
        u64 seg_start = align_down(seg_vaddr);
        u64 seg_end = align_up(seg_vaddr + ph[i].p_memsz);
        size_t map_sz = (size_t)(seg_end - seg_start);

        int prot = 1; // R
        if (ph[i].p_flags & PF_W) prot |= 2; // W

        void *m = mm_mmap((void*)(uintptr_t)seg_start, map_sz, prot, 1 /*FIXED*/ | 2 /*ANON*/);
        if ((long)m == -1) {
            printf("ld-moduos: mmap failed for segment\n");
            return -1;
        }

        if (ph[i].p_filesz) {
            memcpy((void*)(uintptr_t)seg_vaddr, (const u8*)file + ph[i].p_offset, (size_t)ph[i].p_filesz);
        }
    }

    *out_entry = base + eh->e_entry;
    return 0;
}

static void obj_free_needed(so_obj_t *o) {
    if (!o || !o->needed) return;
    for (u32 i = 0; i < o->needed_count; i++) {
        if (o->needed[i]) free(o->needed[i]);
    }
    free(o->needed);
    o->needed = NULL;
    o->needed_count = 0;
}

static int obj_parse_dynamic(so_obj_t *o, u64 dyn_vaddr) {
    if (!dyn_vaddr) return 0;

    o->dyn = (const dyn_t*)(uintptr_t)dyn_vaddr;

    const dyn_t *dyn = o->dyn;

    /* First pass: find key pointers */
    for (size_t i = 0;; i++) {
        if (dyn[i].d_tag == DT_NULL) break;
        switch (dyn[i].d_tag) {
            case DT_STRTAB: o->strtab = (const char*)(uintptr_t)(o->base + dyn[i].d_val); break;
            case DT_STRSZ:  o->strsz  = dyn[i].d_val; break;
            case DT_SYMTAB: o->symtab = (const sym_t*)(uintptr_t)(o->base + dyn[i].d_val); break;
            case DT_HASH:   o->hash   = (const u32*)(uintptr_t)(o->base + dyn[i].d_val); break;
            case DT_RELA:   o->rela   = (const rela_t*)(uintptr_t)(o->base + dyn[i].d_val); break;
            case DT_RELASZ: o->relasz = dyn[i].d_val; break;
            case DT_RELAENT:o->relaent = dyn[i].d_val; break;
            default: break;
        }
    }

    if (o->hash) {
        /* DT_HASH: [nbucket, nchain, buckets..., chains...] */
        o->nsyms = o->hash[1];
    }

    /* Gather DT_NEEDED */
    obj_free_needed(o);
    u32 count = 0;
    for (size_t i = 0;; i++) {
        if (dyn[i].d_tag == DT_NULL) break;
        if (dyn[i].d_tag == DT_NEEDED) count++;
    }

    if (count) {
        o->needed = (char**)malloc((size_t)count * sizeof(char*));
        if (!o->needed) return -1;
        memset(o->needed, 0, (size_t)count * sizeof(char*));
        o->needed_count = count;

        u32 out = 0;
        for (size_t i = 0;; i++) {
            if (dyn[i].d_tag == DT_NULL) break;
            if (dyn[i].d_tag != DT_NEEDED) continue;
            if (!o->strtab) return -1;
            const char *s = o->strtab + dyn[i].d_val;
            size_t n = strlen(s);
            o->needed[out] = (char*)malloc(n + 1);
            if (!o->needed[out]) return -1;
            memcpy(o->needed[out], s, n + 1);
            out++;
            if (out >= count) break;
        }
    }

    return 0;
}

static so_obj_t *g_objs = NULL;

static so_obj_t *obj_find_loaded_by_path(const char *path) {
    for (so_obj_t *o = g_objs; o; o = o->next) {
        if (strcmp(o->path, path) == 0) return o;
    }
    return NULL;
}

static so_obj_t *obj_add_loaded(const char *path) {
    so_obj_t *o = (so_obj_t*)malloc(sizeof(*o));
    if (!o) return NULL;
    memset(o, 0, sizeof(*o));
    strncpy(o->path, path, sizeof(o->path) - 1);
    o->path[sizeof(o->path) - 1] = 0;
    o->relaent = sizeof(rela_t);

    o->next = g_objs;
    g_objs = o;
    return o;
}

static int obj_load(const char *path, so_obj_t **out_obj) {
    if (!path || !out_obj) return -1;

    so_obj_t *already = obj_find_loaded_by_path(path);
    if (already) { *out_obj = already; return 0; }

    void *file = NULL;
    size_t file_sz = 0;
    if (load_file(path, &file, &file_sz) != 0) return -1;

    const ehdr_t *eh = (const ehdr_t*)file;

    so_obj_t *o = obj_add_loaded(path);
    if (!o) { free(file); return -1; }

    /* Choose base for ET_DYN */
    if (eh->e_type == ET_DYN) {
        /* reserve some range and use it as a base */
        void *tmp = mm_mmap(NULL, 0x400000, 3, 2 /*ANON*/);
        if ((long)tmp == -1) { free(file); return -1; }
        o->base = (u64)(uintptr_t)tmp;
        mm_munmap(tmp, 0x400000);
    } else {
        o->base = 0;
    }

    u64 dyn_vaddr = 0;
    if (map_load_segments(file, o->base, &o->entry, &dyn_vaddr) != 0) {
        free(file);
        return -1;
    }

    if (obj_parse_dynamic(o, dyn_vaddr) != 0) {
        free(file);
        return -1;
    }

    free(file);
    *out_obj = o;
    return 0;
}

static int obj_load_deps_recursive(so_obj_t *o) {
    if (!o) return -1;

    for (u32 i = 0; i < o->needed_count; i++) {
        const char *soname = o->needed[i];
        if (!soname || !soname[0]) continue;

        char full[256];
        strcpy(full, "/ModuOS/shared/usr/lib/");
        strncat(full, soname, sizeof(full) - strlen(full) - 1);

        so_obj_t *dep = NULL;
        if (obj_load(full, &dep) != 0) {
            printf("ld-moduos: cannot load needed %s\n", full);
            return -1;
        }

        if (obj_load_deps_recursive(dep) != 0) return -1;
    }

    return 0;
}

static u64 obj_sym_addr(so_obj_t *o, const sym_t *s) {
    return o->base + s->st_value;
}

static int sym_is_usable_definition(const sym_t *s) {
    if (!s) return 0;
    if (s->st_shndx == SHN_UNDEF) return 0;
    u8 bind = ELF64_ST_BIND(s->st_info);
    if (bind == STB_LOCAL) return 0;
    return 1;
}

static u64 resolve_symbol_addr(const char *name, int *out_is_weak) {
    if (out_is_weak) *out_is_weak = 0;
    if (!name || !name[0]) return 0;

    u64 weak_addr = 0;

    for (so_obj_t *o = g_objs; o; o = o->next) {
        if (!o->symtab || !o->strtab || o->nsyms == 0) continue;

        for (u32 i = 0; i < o->nsyms; i++) {
            const sym_t *s = &o->symtab[i];
            if (s->st_name >= o->strsz) continue;

            const char *sname = o->strtab + s->st_name;
            if (strcmp(sname, name) != 0) continue;

            if (s->st_shndx == SHN_UNDEF) {
                continue;
            }

            u8 bind = ELF64_ST_BIND(s->st_info);
            if (bind == STB_GLOBAL) {
                return obj_sym_addr(o, s);
            }
            if (bind == STB_WEAK && weak_addr == 0) {
                weak_addr = obj_sym_addr(o, s);
            }
        }
    }

    if (weak_addr) {
        if (out_is_weak) *out_is_weak = 1;
        return weak_addr;
    }

    return 0;
}

static int relocate_one_object(so_obj_t *o) {
    if (!o || !o->rela || !o->relasz) return 0;

    u64 ent = o->relaent ? o->relaent : sizeof(rela_t);
    u64 count = o->relasz / ent;

    for (u64 i = 0; i < count; i++) {
        const rela_t *r = (const rela_t*)((const u8*)o->rela + (size_t)(i * ent));
        u32 type = ELF64_R_TYPE(r->r_info);
        u32 symi = ELF64_R_SYM(r->r_info);

        u64 *where = (u64*)(uintptr_t)(o->base + r->r_offset);

        if (type == R_X86_64_RELATIVE) {
            *where = o->base + (u64)r->r_addend;
            continue;
        }

        if (type == R_X86_64_64 || type == R_X86_64_GLOB_DAT || type == R_X86_64_JUMP_SLOT) {
            if (!o->symtab || !o->strtab || o->nsyms == 0) {
                printf("ld-moduos: missing symtab/strtab for reloc\n");
                return -1;
            }
            if (symi >= o->nsyms) {
                printf("ld-moduos: bad sym index\n");
                return -1;
            }

            const sym_t *sym = &o->symtab[symi];
            const char *name = (sym->st_name < o->strsz) ? (o->strtab + sym->st_name) : "";

            /* If the symbol is defined in this object, use it directly; otherwise resolve globally. */
            u64 S = 0;
            if (sym_is_usable_definition(sym)) {
                S = obj_sym_addr(o, sym);
            } else {
                int is_weak = 0;
                S = resolve_symbol_addr(name, &is_weak);
                if (S == 0 && ELF64_ST_BIND(sym->st_info) != STB_WEAK) {
                    printf("ld-moduos: unresolved symbol '%s'\n", name);
                    return -1;
                }
            }

            u64 A = (u64)r->r_addend;
            *where = S + A;
            continue;
        }

        /* Unknown relocation */
        printf("ld-moduos: unsupported reloc type %u\n", (unsigned)type);
        return -1;
    }

    return 0;
}

static int relocate_all_objects(void) {
    /* We relocate in multiple passes in case of forward refs.
     * With full symbol lookup across all loaded objects, one pass is usually enough,
     * but doing two is cheap.
     */
    for (int pass = 0; pass < 2; pass++) {
        for (so_obj_t *o = g_objs; o; o = o->next) {
            if (relocate_one_object(o) != 0) return -1;
        }
    }
    return 0;
}

int md_main(long argc, char **argv) {
    if (argc < 2) {
        printf("Usage: ld-moduos <program> [args...]\n");
        return 1;
    }

    const char *target = argv[1];

    so_obj_t *main_obj = NULL;
    if (obj_load(target, &main_obj) != 0) {
        printf("ld-moduos: cannot load target %s\n", target);
        return 1;
    }

    if (obj_load_deps_recursive(main_obj) != 0) {
        return 1;
    }

    if (relocate_all_objects() != 0) {
        return 1;
    }

    /* Jump to program entry using ModuOS ABI (_start(argc, argv)).
     * Target argc/argv are argv[1..], so we pass argc-1 and &argv[1].
     */
    void (*entry_fn)(long, char**) = (void(*)(long, char**))(uintptr_t)main_obj->entry;
    long targc = argc - 1;
    char **targv = &argv[1];

    entry_fn(targc, targv);

    return 0;
}
