// SPDX-License-Identifier: GPL-2.0-only
//
// ModuOS Kernel (GPLv2)
// execve_impl.c - Linux-like execve implementation
// NOTE: this file is included by syscall.c (kept local to syscalls for now)

#include "moduos/kernel/errno.h"
#include "moduos/kernel/process/process_new.h"
#include "moduos/kernel/memory/memory.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/memory/usercopy.h"
#include "moduos/kernel/loader/elf.h"
#include "moduos/kernel/memory/paging.h"
#include "moduos/kernel/memory/phys.h"
#include "moduos/fs/fs.h"
#include "moduos/fs/path_norm.h"
#include "moduos/fs/path.h"
#include "moduos/fs/fd.h"
#include "moduos/kernel/COM/com.h"

#ifndef PAGE_SIZE
#define PAGE_SIZE 0x1000
#endif

#define ARG_MAX (128 * 1024)

extern void amd64_enter_user_now(uint64_t user_rip, uint64_t user_rsp,
                                 uint64_t argc, uint64_t argv, uint64_t envp);

__attribute__((unused))
static int streq_prefix(const char *s, const char *pfx) {
    while (*pfx) {
        if (*s++ != *pfx++) return 0;
    }
    return 1;
}

static const char *proc_getenv(process_t *p, const char *key) {
    if (!p || !key) return NULL;
    size_t klen = strlen(key);
    if (klen == 0) return NULL;

    for (int i = 0; i < p->envc; i++) {
        const char *kv = p->envp ? p->envp[i] : NULL;
        if (!kv) continue;
        if (strncmp(kv, key, klen) == 0 && kv[klen] == '=') {
            return kv + klen + 1;
        }
    }
    return NULL;
}

static int has_slash(const char *s) {
    if (!s) return 0;
    for (; *s; s++) if (*s == '/') return 1;
    return 0;
}

static int ends_with_sqr(const char *s) {
    if (!s) return 0;
    size_t n = strlen(s);
    return (n >= 4 && s[n-4]=='.' && s[n-3]=='s' && s[n-2]=='q' && s[n-1]=='r');
}

static void join_dir_file(const char *dir, const char *file, char *out, size_t out_sz) {
    if (!out || out_sz == 0) return;
    out[0] = 0;
    if (!dir) dir = "";
    if (!file) file = "";

    if (dir[0] == 0) dir = ".";

    strncpy(out, dir, out_sz - 1);
    out[out_sz - 1] = 0;

    size_t len = strlen(out);
    if (len == 0 || out[len-1] != '/') {
        if (len + 1 < out_sz) {
            out[len] = '/';
            out[len+1] = 0;
        }
    }

    strncat(out, file, out_sz - strlen(out) - 1);
}

static void resolve_against_cwd(process_t *p, const char *path, char *out, size_t out_sz) {
    if (!out || out_sz == 0) return;
    out[0] = 0;
    if (!path) path = "";

    if (path[0] == '/' || (path[0] == '$' && path[1] == '/')) {
        strncpy(out, path, out_sz - 1);
        out[out_sz - 1] = 0;
        return;
    }

    const char *cwd = (p && p->cwd[0]) ? p->cwd : "/";
    strncpy(out, cwd, out_sz - 1);
    out[out_sz - 1] = 0;

    size_t len = strlen(out);
    if (len == 0 || out[len-1] != '/') {
        strncat(out, "/", out_sz - strlen(out) - 1);
    }
    strncat(out, path, out_sz - strlen(out) - 1);
}

static int execve_try_path(process_t *p, const char *candidate, char *out_full, size_t out_full_sz,
                           int *out_slot, char *out_rel, size_t out_rel_sz) {
    if (!p || !candidate || !out_full || !out_slot || !out_rel) return -EINVAL;

    resolve_against_cwd(p, candidate, out_full, out_full_sz);
    path_normalize_inplace(out_full);

    fs_path_resolved_t r;
    if (fs_resolve_path(p, out_full, &r) != 0) return -ENOENT;
    if (r.route == FS_ROUTE_DEVVFS) return -EACCES;

    int slot = (r.route == FS_ROUTE_MOUNT) ? r.mount_slot : p->current_slot;
    if (slot < 0) return -ENOENT;

    fs_mount_t *mnt = fs_get_mount(slot);
    if (!mnt || !mnt->valid) return -ENOENT;

    fs_file_info_t st;
    if (fs_stat(mnt, r.rel_path, &st) != 0) return -ENOENT;
    if (st.is_directory) return -EACCES;

    *out_slot = slot;
    strncpy(out_rel, r.rel_path, out_rel_sz - 1);
    out_rel[out_rel_sz - 1] = 0;
    return 0;
}

static int resolve_execve_path(process_t *p, const char *path_in, char *out_full, size_t out_full_sz,
                               int *out_slot, char *out_rel, size_t out_rel_sz) {
    if (!p || !path_in || !out_full || !out_slot || !out_rel) return -EINVAL;

    if (has_slash(path_in) || (path_in[0] == '$' && path_in[1] == '/')) {
        int rc = execve_try_path(p, path_in, out_full, out_full_sz, out_slot, out_rel, out_rel_sz);
        if (rc == 0) return 0;

        if (!ends_with_sqr(path_in)) {
            char tmp[256];
            strncpy(tmp, path_in, sizeof(tmp) - 1);
            tmp[sizeof(tmp) - 1] = 0;
            strncat(tmp, ".sqr", sizeof(tmp) - strlen(tmp) - 1);
            rc = execve_try_path(p, tmp, out_full, out_full_sz, out_slot, out_rel, out_rel_sz);
            if (rc == 0) return 0;
        }
        return rc;
    }

    const char *path_env = proc_getenv(p, "PATH");
    if (!path_env) return -ENOENT;

    char *path_buf = (char*)kmalloc(strlen(path_env) + 1);
    if (!path_buf) return -ENOMEM;
    strcpy(path_buf, path_env);

    int rc_final = -ENOENT;

    char *cur = path_buf;
    while (cur) {
        char *next = cur;
        while (*next && *next != ':' && *next != ';') next++;
        if (*next) { *next = 0; next++; } else { next = NULL; }

        char cand[256];
        join_dir_file(cur, path_in, cand, sizeof(cand));

        int slot = -1;
        char rel[256];
        char full[256];
        int rc = execve_try_path(p, cand, full, sizeof(full), &slot, rel, sizeof(rel));
        if (rc == 0) {
            strncpy(out_full, full, out_full_sz - 1);
            out_full[out_full_sz - 1] = 0;
            *out_slot = slot;
            strncpy(out_rel, rel, out_rel_sz - 1);
            out_rel[out_rel_sz - 1] = 0;
            kfree(path_buf);
            return 0;
        }

        if (!ends_with_sqr(path_in)) {
            char cand2[256];
            strncpy(cand2, cand, sizeof(cand2) - 1);
            cand2[sizeof(cand2) - 1] = 0;
            strncat(cand2, ".sqr", sizeof(cand2) - strlen(cand2) - 1);
            rc = execve_try_path(p, cand2, full, sizeof(full), &slot, rel, sizeof(rel));
            if (rc == 0) {
                strncpy(out_full, full, out_full_sz - 1);
                out_full[out_full_sz - 1] = 0;
                *out_slot = slot;
                strncpy(out_rel, rel, out_rel_sz - 1);
                out_rel[out_rel_sz - 1] = 0;
                kfree(path_buf);
                return 0;
            }
        }

        if (rc != -ENOENT) rc_final = rc;
        cur = next;
    }

    kfree(path_buf);
    return rc_final;
}

static int copy_user_strv(char *const *user_vec, char ***out_vec, int *out_count,
                          size_t *inout_budget) {
    if (!out_vec || !out_count || !inout_budget) return -EINVAL;
    *out_vec = NULL;
    *out_count = 0;

    if (!user_vec) return 0;

    const int MAX_ITEMS = 4096;

    char **kvec = (char**)kmalloc((MAX_ITEMS + 1) * sizeof(char*));
    if (!kvec) return -ENOMEM;
    memset(kvec, 0, (MAX_ITEMS + 1) * sizeof(char*));

    int count = 0;
    for (; count < MAX_ITEMS; count++) {
        uint64_t uptr = 0;
        if (usercopy_from_user(&uptr, &user_vec[count], sizeof(uptr)) != 0) {
            for (int i = 0; i < count; i++) if (kvec[i]) kfree(kvec[i]);
            kfree(kvec);
            return -EFAULT;
        }
        if (uptr == 0) {
            kvec[count] = NULL;
            break;
        }

        size_t max_copy = *inout_budget;
        if (max_copy == 0) {
            for (int i = 0; i < count; i++) if (kvec[i]) kfree(kvec[i]);
            kfree(kvec);
            return -E2BIG;
        }
        if (max_copy > 4096) max_copy = 4096;

        char tmp[4096];
        if (usercopy_string_from_user(tmp, (const char*)(uintptr_t)uptr, max_copy) != 0) {
            for (int i = 0; i < count; i++) if (kvec[i]) kfree(kvec[i]);
            kfree(kvec);
            return -EFAULT;
        }

        size_t len = strlen(tmp) + 1;
        if (len > *inout_budget) {
            for (int i = 0; i < count; i++) if (kvec[i]) kfree(kvec[i]);
            kfree(kvec);
            return -E2BIG;
        }

        char *s = (char*)kmalloc(len);
        if (!s) {
            for (int i = 0; i < count; i++) if (kvec[i]) kfree(kvec[i]);
            kfree(kvec);
            return -ENOMEM;
        }
        memcpy(s, tmp, len);
        kvec[count] = s;
        *inout_budget -= len;
    }

    if (count == MAX_ITEMS) {
        for (int i = 0; i < count; i++) if (kvec[i]) kfree(kvec[i]);
        kfree(kvec);
        return -E2BIG;
    }

    char **final = (char**)kmalloc((count + 1) * sizeof(char*));
    if (!final) {
        for (int i = 0; i < count; i++) if (kvec[i]) kfree(kvec[i]);
        kfree(kvec);
        return -ENOMEM;
    }
    memcpy(final, kvec, (count + 1) * sizeof(char*));
    kfree(kvec);

    *out_vec = final;
    *out_count = count;
    return 0;
}

static void free_strv(char **v, int c) {
    if (!v) return;
    for (int i = 0; i < c; i++) if (v[i]) kfree(v[i]);
    kfree(v);
}

static int build_user_stack(process_t *p, int argc, char **kargv, int envc, char **kenvp,
                            uint64_t *out_user_rsp, uint64_t *out_user_argv, uint64_t *out_user_envp) {
    if (!p || !p->is_user || !out_user_rsp || !out_user_argv || !out_user_envp) return -EINVAL;

    const uint64_t user_stack_top   = 0x00007ffffffff000ULL;
    const uint64_t guard_pages      = 1;
    const uint64_t initial_pages    = 4;
    const uint64_t max_pages        = 2048;

    const uint64_t user_stack_guard = user_stack_top - guard_pages * PAGE_SIZE;
    const uint64_t user_stack_base  = user_stack_guard - initial_pages * PAGE_SIZE;
    const uint64_t user_stack_limit = user_stack_top - max_pages * PAGE_SIZE;

    uint64_t phys_base = phys_alloc_contiguous((size_t)initial_pages);
    if (!phys_base) return -ENOMEM;

    /* Walk the CURRENT CR3 — caller must have already switched to new_cr3
     * before calling build_user_stack so this maps into the right PML4. */
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    uint64_t pml4_phys = cr3 & 0xFFFFFFFFFFFFF000ULL;
    uint64_t *proc_pml4 = (uint64_t*)phys_to_virt_kernel(pml4_phys);

    if (!proc_pml4 || paging_map_range_to_pml4(proc_pml4, user_stack_base, phys_base,
                                               initial_pages * PAGE_SIZE,
                                               PFLAG_PRESENT | PFLAG_WRITABLE | PFLAG_USER) != 0) {
        for (size_t i = 0; i < (size_t)initial_pages; i++)
            phys_ref_dec(phys_base + (uint64_t)i * PAGE_SIZE);
        return -ENOMEM;
    }

    p->user_stack       = (void*)(uintptr_t)user_stack_base;
    p->user_stack_top   = user_stack_top;
    p->user_stack_low   = user_stack_base;
    p->user_stack_limit = user_stack_limit;

    uint64_t sp = user_stack_base + (initial_pages * PAGE_SIZE) - 16;

    if (argc < 0) argc = 0;
    if (envc < 0) envc = 0;

    uint64_t *argv_str_ptrs = (uint64_t*)kmalloc((size_t)argc * sizeof(uint64_t));
    uint64_t *env_str_ptrs  = (uint64_t*)kmalloc((size_t)envc * sizeof(uint64_t));
    if ((argc && !argv_str_ptrs) || (envc && !env_str_ptrs)) {
        if (argv_str_ptrs) kfree(argv_str_ptrs);
        if (env_str_ptrs)  kfree(env_str_ptrs);
        return -ENOMEM;
    }

    for (int i = argc - 1; i >= 0; i--) {
        const char *s = (kargv && kargv[i]) ? kargv[i] : "";
        size_t len = strlen(s) + 1;
        if (sp < user_stack_base + len + 256) {
            kfree(argv_str_ptrs);
            if (env_str_ptrs) kfree(env_str_ptrs);
            return -E2BIG;
        }
        sp -= len;
        memcpy((void*)(uintptr_t)sp, s, len);
        argv_str_ptrs[i] = sp;
    }

    for (int i = envc - 1; i >= 0; i--) {
        const char *s = (kenvp && kenvp[i]) ? kenvp[i] : "";
        size_t len = strlen(s) + 1;
        if (sp < user_stack_base + len + 256) {
            kfree(argv_str_ptrs);
            kfree(env_str_ptrs);
            return -E2BIG;
        }
        sp -= len;
        memcpy((void*)(uintptr_t)sp, s, len);
        env_str_ptrs[i] = sp;
    }

    sp &= ~0xFULL;

    sp -= (uint64_t)(envc + 1) * sizeof(uint64_t);
    uint64_t *user_envp = (uint64_t*)(uintptr_t)sp;
    for (int i = 0; i < envc; i++) user_envp[i] = env_str_ptrs[i];
    user_envp[envc] = 0;

    sp -= (uint64_t)(argc + 1) * sizeof(uint64_t);
    uint64_t *user_argv = (uint64_t*)(uintptr_t)sp;
    for (int i = 0; i < argc; i++) user_argv[i] = argv_str_ptrs[i];
    user_argv[argc] = 0;

    if (((sp - 8) & 0xFULL) != 8) {
        sp -= 8;
        *(uint64_t*)(uintptr_t)sp = 0;
    }
    sp -= 8;
    *(uint64_t*)(uintptr_t)sp = 0;

    *out_user_rsp  = sp;
    *out_user_argv = (uint64_t)(uintptr_t)user_argv;
    *out_user_envp = (uint64_t)(uintptr_t)user_envp;

    kfree(argv_str_ptrs);
    kfree(env_str_ptrs);
    return 0;
}

int sys_execve_impl(const char *path_user, char *const *argv_user, char *const *envp_user) {
    extern volatile process_t *current;
    com_write_string(COM1_PORT, "[EXECVE] Entry: current=0x");
    com_write_hex64(COM1_PORT, (uint64_t)(uintptr_t)current);
    com_write_string(COM1_PORT, "\n");

    process_t *p = process_get_current();
    com_write_string(COM1_PORT, "[EXECVE] process_get_current()=0x");
    com_write_hex64(COM1_PORT, (uint64_t)(uintptr_t)p);
    com_write_string(COM1_PORT, "\n");

    if (!p) {
        com_write_string(COM1_PORT, "[EXECVE] ERROR: current is NULL!\n");
        return -ESRCH;
    }
    if (!p->is_user) return -EACCES;

    /* ── 1. Copy path ─────────────────────────────────────────────────── */
    char kpath[256];
    if (!path_user) return -EFAULT;
    if (usercopy_string_from_user(kpath, path_user, sizeof(kpath)) != 0) return -EFAULT;
    if (kpath[0] == 0) return -ENOENT;

    /* ── 2. Resolve executable path ───────────────────────────────────── */
    char full[256];
    char rel[256];
    int  slot = -1;
    int  rc   = resolve_execve_path(p, kpath, full, sizeof(full), &slot, rel, sizeof(rel));
    if (rc != 0) return rc;

    fs_mount_t *mnt = fs_get_mount(slot);
    if (!mnt || !mnt->valid) return -ENOENT;

    fs_file_info_t st;
    if (fs_stat(mnt, rel, &st) != 0 || st.is_directory || st.size < sizeof(elf64_ehdr_t))
        return -ENOENT;

    /* ── 3. Copy argv / envp into kernel ──────────────────────────────── */
    size_t budget = ARG_MAX;

    char **kargv = NULL;
    int   argc   = 0;
    rc = copy_user_strv(argv_user, &kargv, &argc, &budget);
    if (rc != 0) return rc;

    if (argc == 0) {
        kfree(kargv);
        kargv = (char**)kmalloc(2 * sizeof(char*));
        if (!kargv) return -ENOMEM;
        kargv[0] = (char*)kmalloc(strlen(kpath) + 1);
        if (!kargv[0]) { kfree(kargv); return -ENOMEM; }
        strcpy(kargv[0], kpath);
        kargv[1] = NULL;
        argc = 1;
    }

    char **kenvp = NULL;
    int   envc   = 0;
    if (envp_user == NULL) {
        envc = p->envc;
        if (envc > 0 && p->envp) {
            kenvp = (char**)kmalloc((envc + 1) * sizeof(char*));
            if (!kenvp) { free_strv(kargv, argc); return -ENOMEM; }
            memset(kenvp, 0, (envc + 1) * sizeof(char*));
            for (int i = 0; i < envc; i++) {
                const char *s = p->envp[i] ? p->envp[i] : "";
                size_t len = strlen(s) + 1;
                if (len > budget) { free_strv(kargv, argc); free_strv(kenvp, i); return -E2BIG; }
                char *d = (char*)kmalloc(len);
                if (!d) { free_strv(kargv, argc); free_strv(kenvp, i); return -ENOMEM; }
                memcpy(d, s, len);
                kenvp[i] = d;
                budget -= len;
            }
            kenvp[envc] = NULL;
        }
    } else {
        rc = copy_user_strv(envp_user, &kenvp, &envc, &budget);
        if (rc != 0) { free_strv(kargv, argc); return rc; }
    }

    /* ── 4. Read ELF into kernel buffer ───────────────────────────────── */
    void *elf_buf = kmalloc(st.size);
    if (!elf_buf) { free_strv(kargv, argc); free_strv(kenvp, envc); return -ENOMEM; }
    size_t rd = 0;
    if (fs_read_file(mnt, rel, elf_buf, st.size, &rd) != 0 || rd < sizeof(elf64_ehdr_t)) {
        kfree(elf_buf);
        free_strv(kargv, argc);
        free_strv(kenvp, envc);
        return -ENOENT;
    }

    /* ── 5. Create new PML4 WHILE STILL ON KERNEL / PARENT CR3 ───────── *
     *                                                                      *
     *  INVARIANT: paging_create_process_pml4() copies kernel mappings     *
     *  from the CURRENT CR3. We must still be on a CR3 that has valid     *
     *  kernel mappings (kernel CR3 or the shell's CR3). Do NOT switch to  *
     *  the old process CR3 before this call.                               *
     *                                                                      *
     * ─────────────────────────────────────────────────────────────────── */
    extern void process_set_build_pml4(uint64_t *virt, uint64_t phys);
    uint64_t new_cr3 = paging_create_process_pml4();
    if (!new_cr3) {
        kfree(elf_buf);
        free_strv(kargv, argc);
        free_strv(kenvp, envc);
        return -ENOMEM;
    }

    /* ── 6. Load ELF into new PML4 ───────────────────────────────────── *
     *                                                                      *
     *  elf_load_with_args() calls kmalloc internally. We must NOT have    *
     *  freed the old process pages yet — if we had, the allocator could   *
     *  hand those same physical pages back to kmalloc while they're still *
     *  referenced by the old PML4, causing silent memory corruption.      *
     *                                                                      *
     *  g_build_pml4 directs paging_map_page() inside elf_load into       *
     *  new_cr3, so no CR3 switch is needed here.                          *
     * ─────────────────────────────────────────────────────────────────── */
    uint64_t *new_pml4_virt = (uint64_t*)phys_to_virt_kernel(new_cr3 & ~0xFFFULL);
    process_set_build_pml4(new_pml4_virt, new_cr3);

    uint64_t entry = 0, img_base = 0, img_end = 0;
    rc = elf_load_with_args(elf_buf, rd, &entry, 0, NULL, &img_base, &img_end);
    kfree(elf_buf);
    process_set_build_pml4(NULL, 0);

    if (rc != 0) {
        free_strv(kargv, argc);
        free_strv(kenvp, envc);
        /* new_cr3 pages were allocated inside paging_create_process_pml4;
         * leak them for now — a full unmap here is complex and this path
         * is already an error path. TODO: paging_free_process_pml4(new_cr3) */
        return -ENOEXEC;
    }

    /* ── 7. Free OLD address space ────────────────────────────────────── *
     *                                                                      *
     *  Switch to the OLD process CR3 first so process_free_user_memory()  *
     *  walks the correct (old) page tables when calling                   *
     *  paging_virt_to_phys() / paging_unmap_page(). Any kmalloc that     *
     *  happened above (steps 5-6) has already completed, so the allocator *
     *  will not hand out these physical pages while we free them.         *
     *                                                                      *
     *  IMPORTANT: do NOT call phys_free_frame(old_pml4_phys) here.       *
     *  build_user_stack() (step 9) calls alloc_pt_page →                 *
     *  phys_alloc_frame_below(64MB), which would immediately reclaim the  *
     *  just-freed frame and memset it to zero — silently corrupting       *
     *  whatever kernel struct (e.g. another process's process_t) is       *
     *  backed by that physical page. Defer the free until after step 9.  *
     * ─────────────────────────────────────────────────────────────────── */
    extern void paging_switch_cr3(uint64_t);

    com_write_string(COM1_PORT, "[EXECVE] PID=");
    char ebuf[16];
    itoa((int)p->pid, ebuf, 10);
    com_write_string(COM1_PORT, ebuf);
    com_write_string(COM1_PORT, " freeing old AS: old_cr3=0x");
    com_write_hex64(COM1_PORT, p->cr3);
    com_write_string(COM1_PORT, " new_cr3=0x");
    com_write_hex64(COM1_PORT, new_cr3);
    com_write_string(COM1_PORT, "\n");

    paging_switch_cr3(p->cr3);
    process_free_user_memory(p);
    p->user_image_base = 0;
    p->user_image_end  = 0;
    p->user_heap_end   = p->user_heap_base;
    p->user_mmap_end   = p->user_mmap_base;

    /* Save old PML4 phys — defer phys_free_frame until after build_user_stack */
    uint64_t old_pml4_phys_to_free = 0;
    {
        uint64_t candidate = p->cr3 & ~0xFFFULL;
        uint64_t kernel_cr3 = paging_get_master_cr3() & ~0xFFFULL;
        if (candidate && candidate != kernel_cr3)
            old_pml4_phys_to_free = candidate;
    }

    /* ── 8. Commit new address space ──────────────────────────────────── */
    p->cr3        = new_cr3;
    p->page_table = new_cr3;
    paging_switch_cr3(new_cr3);

    com_write_string(COM1_PORT, "[EXECVE] Switched to new CR3=0x");
    com_write_hex64(COM1_PORT, new_cr3);
    com_write_string(COM1_PORT, "\n");

    p->user_image_base = img_base;
    p->user_image_end  = img_end;

    /* ── 9. Build user stack in new address space ─────────────────────── *
     *                                                                      *
     *  CR3 is now new_cr3. build_user_stack() reads CR3 directly to find  *
     *  the PML4 to map the stack into — it will now correctly target      *
     *  new_cr3 instead of whatever was loaded before.                     *
     *                                                                      *
     *  The old PML4 frame is freed AFTER this call returns so that        *
     *  alloc_pt_page inside build_user_stack cannot reclaim and zero it.  *
     * ─────────────────────────────────────────────────────────────────── */
    uint64_t user_rsp = 0, user_argv = 0, user_envp_out = 0;
    rc = build_user_stack(p, argc, kargv, envc, kenvp,
                          &user_rsp, &user_argv, &user_envp_out);
    if (rc != 0) {
        free_strv(kargv, argc);
        free_strv(kenvp, envc);
        /* Safe to free now — no more alloc_pt_page calls will happen */
        if (old_pml4_phys_to_free)
            phys_free_frame(old_pml4_phys_to_free);
        return rc;
    }

    /* All alloc_pt_page calls for the new address space are done.
     * Now it is safe to return the old PML4 frame to the allocator. */
    if (old_pml4_phys_to_free)
        phys_free_frame(old_pml4_phys_to_free);

    /* ── 10a. Update process name to reflect the new executable ───────── *
     * Without this, p->name retains the parent's name across execve()    *
     * (inherited via fork()'s strncpy of parent->name), which makes       *
     * scheduler dumps and process listings show the old program name      *
     * (e.g. "automan.sqr") for a process that is actually running a       *
     * completely different binary (e.g. "zenith5.1.sqr").                 */
    {
        const char *base = rel;
        for (const char *q = rel; *q; q++) {
            if (*q == '/') base = q + 1;
        }
        strncpy(p->name, base, PROCESS_NAME_MAX - 1);
        p->name[PROCESS_NAME_MAX - 1] = 0;
    }

    /* ── 10b. Reset FPU state for the new image ────────────────────────── *
     * execve() reuses the existing process_t rather than allocating a     *
     * fresh one, so p->fpu_state still holds whatever fork() copied from  *
     * the parent. That value is only guaranteed correct if the parent     *
     * itself never drifted from the safe default — reset explicitly here  *
     * so every newly exec'd image starts from known-good FCW/MXCSR        *
     * regardless of what the parent process did to its own FPU state.     *
     * If this process was the live FPU owner, force a re-fault into       *
     * fpu_lazy_handle_nm on its next FP instruction so it doesn't keep    *
     * running with stale hardware register contents belonging to the      *
     * previous image.                                                     */
    {
        extern void fpu_lazy_on_process_exit(process_t *p);
        fpu_lazy_on_process_exit(p);
        memset(p->fpu_state, 0, 512);
        fpu_state_init_default(p->fpu_state);
    }

    /* ── 10. Update process env ───────────────────────────────────────── */
    if (p->envp) {
        for (int i = 0; i < p->envc; i++) if (p->envp[i]) kfree(p->envp[i]);
        kfree(p->envp);
    }
    p->envp = kenvp;
    p->envc = envc;

    free_strv(kargv, argc);

    /* ── 11. Enter userland ───────────────────────────────────────────── */
    p->user_rip = entry;
    p->user_rsp = user_rsp;

    com_write_string(COM1_PORT, "[EXECVE] Entering user: RIP=0x");
    com_write_hex64(COM1_PORT, entry);
    com_write_string(COM1_PORT, " RSP=0x");
    com_write_hex64(COM1_PORT, user_rsp);
    com_write_string(COM1_PORT, "\n");

    paging_sync_kernel_mappings((uint64_t*)phys_to_virt_kernel(new_cr3 & ~0xFFFULL));
    __asm__ volatile("cli");
    amd64_enter_user_now(entry, user_rsp, (uint64_t)argc, user_argv, user_envp_out);

    /* not reached */
    return 0;
}