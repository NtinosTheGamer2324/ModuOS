// SPDX-License-Identifier: GPL-2.0-only
//
// ModuOS Kernel (GPLv2)
// env_impl.c - kernel environment variable syscalls
// Included by syscall.c

#include "moduos/kernel/errno.h"
#include "moduos/kernel/process/process.h"
#include "moduos/kernel/memory/memory.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/memory/usercopy.h"

#define ENV_MAX_ITEMS 256
#define ENV_MAX_KV    512

static int env_key_len(const char *kv) {
    int n = 0;
    while (kv && kv[n] && kv[n] != '=' && n < ENV_MAX_KV) n++;
    return (kv && kv[n] == '=') ? n : -1;
}

static int env_find_idx(process_t *p, const char *key, int klen) {
    if (!p || !p->envp || klen <= 0) return -1;
    for (int i = 0; i < p->envc; i++) {
        const char *s = p->envp[i];
        if (!s) continue;
        if (strncmp(s, key, (size_t)klen) == 0 && s[klen] == '=') return i;
    }
    return -1;
}

static int env_set_kv(process_t *p, const char *kv) {
    if (!p || !kv) return -EINVAL;
    int klen = env_key_len(kv);
    if (klen <= 0) return -EINVAL;

    int idx = env_find_idx(p, kv, klen);

    size_t len = strlen(kv) + 1;
    char *copy = (char*)kmalloc(len);
    if (!copy) return -ENOMEM;
    memcpy(copy, kv, len);

    if (idx >= 0) {
        if (p->envp[idx]) kfree(p->envp[idx]);
        p->envp[idx] = copy;
        return 0;
    }

    if (p->envc >= ENV_MAX_ITEMS) {
        kfree(copy);
        return -E2BIG;
    }

    // grow envp array
    int newc = p->envc + 1;
    char **nvec = (char**)kmalloc((size_t)(newc + 1) * sizeof(char*));
    if (!nvec) { kfree(copy); return -ENOMEM; }
    memset(nvec, 0, (size_t)(newc + 1) * sizeof(char*));

    for (int i = 0; i < p->envc; i++) nvec[i] = p->envp[i];
    nvec[p->envc] = copy;
    nvec[newc] = NULL;

    if (p->envp) kfree(p->envp);
    p->envp = nvec;
    p->envc = newc;
    return 0;
}

int sys_putenv(const char *user_kv) {
    process_t *p = process_get_current();
    if (!p || !p->is_user) return -EACCES;
    if (!user_kv) return -EFAULT;

    char kv[ENV_MAX_KV];
    if (usercopy_string_from_user(kv, user_kv, sizeof(kv)) != 0) return -EFAULT;
    if (kv[0] == 0) return -EINVAL;

    return env_set_kv(p, kv);
}

int sys_envlist(char *user_buf, size_t buflen);

int sys_envlist(char *user_buf, size_t buflen);
int sys_envlist2(size_t *user_off_inout, char *user_buf, size_t buflen);

static int envlist_write(process_t *p, size_t start_off, char *user_buf, size_t buflen, size_t *out_next_off) {
    size_t off = 0;
    size_t global = 0;

    for (int i = 0; i < p->envc; i++) {
        const char *kv = (p->envp) ? p->envp[i] : NULL;
        if (!kv) continue;
        size_t len = strlen(kv);
        size_t rec_len = len + 1; // +\n

        if (global + rec_len <= start_off) {
            global += rec_len;
            continue;
        }

        // we are inside or at start of this record
        size_t skip = 0;
        if (start_off > global) skip = start_off - global;

        // Copy kv (possibly skipping some prefix)
        if (skip < len) {
            size_t n = len - skip;
            if (off + n >= buflen) n = (buflen > off) ? (buflen - off) : 0;
            if (n == 0) break;
            if (usercopy_to_user(user_buf + off, kv + skip, n) != 0) return -EFAULT;
            off += n;
            skip = 0;
        } else {
            skip -= len;
        }

        // Copy newline
        if (skip == 0) {
            if (off + 1 >= buflen) break;
            char nl = '\n';
            if (usercopy_to_user(user_buf + off, &nl, 1) != 0) return -EFAULT;
            off += 1;
        }

        global += rec_len;
        if (off + 1 >= buflen) break;
    }

    // NUL terminate if possible
    if (off < buflen) {
        char z = 0;
        if (usercopy_to_user(user_buf + off, &z, 1) != 0) return -EFAULT;
    }

    *out_next_off = start_off + off;
    return (int)off;
}

int sys_envlist(char *user_buf, size_t buflen) {
    process_t *p = process_get_current();
    if (!p || !p->is_user) return -EACCES;
    if (!user_buf || buflen == 0) return -EINVAL;

    size_t next = 0;
    int rc = envlist_write(p, 0, user_buf, buflen, &next);
    if (rc < 0) return rc;

    // If buffer fills before full env, return -E2BIG (legacy behavior).
    // Caller can use envlist2 for streaming.
    // We conservatively detect truncation by checking whether next advanced by < total.
    // Compute total size.
    size_t total = 0;
    for (int i = 0; i < p->envc; i++) {
        const char *kv = (p->envp) ? p->envp[i] : NULL;
        if (!kv) continue;
        total += strlen(kv) + 1;
    }
    if (next < total) return -E2BIG;

    return rc;
}

int sys_envlist2(size_t *user_off_inout, char *user_buf, size_t buflen) {
    process_t *p = process_get_current();
    if (!p || !p->is_user) return -EACCES;
    if (!user_off_inout || !user_buf || buflen == 0) return -EINVAL;

    size_t start = 0;
    if (usercopy_from_user(&start, user_off_inout, sizeof(start)) != 0) return -EFAULT;

    size_t next = start;
    int rc = envlist_write(p, start, user_buf, buflen, &next);
    if (rc < 0) return rc;

    if (usercopy_to_user(user_off_inout, &next, sizeof(next)) != 0) return -EFAULT;
    return rc;
}

int sys_unsetenv(const char *user_key);

int sys_unsetenv(const char *user_key) {
    process_t *p = process_get_current();
    if (!p || !p->is_user) return -EACCES;
    if (!user_key) return -EFAULT;

    char key[128];
    if (usercopy_string_from_user(key, user_key, sizeof(key)) != 0) return -EFAULT;

    int klen = (int)strlen(key);
    if (klen <= 0) return -EINVAL;

    int idx = env_find_idx(p, key, klen);
    if (idx < 0) return 0;

    if (p->envp[idx]) kfree(p->envp[idx]);

    // compact
    for (int i = idx; i < p->envc - 1; i++) {
        p->envp[i] = p->envp[i + 1];
    }
    p->envc--;
    if (p->envp) p->envp[p->envc] = NULL;

    return 0;
}

int sys_getenv(const char *user_key, char *user_buf, size_t buflen) {
    process_t *p = process_get_current();
    if (!p || !p->is_user) return -EACCES;
    if (!user_key) return -EFAULT;

    char key[128];
    if (usercopy_string_from_user(key, user_key, sizeof(key)) != 0) return -EFAULT;

    int klen = (int)strlen(key);
    if (klen <= 0) return -EINVAL;

    int idx = env_find_idx(p, key, klen);
    if (idx < 0) return -ENOENT;

    const char *kv = p->envp[idx];
    if (!kv) return -ENOENT;

    const char *val = kv + klen + 1;
    size_t vlen = strlen(val);

    if (!user_buf || buflen == 0) {
        // allow querying length only
        return (int)vlen;
    }

    // copy as much as fits (including NUL)
    size_t ncopy = vlen + 1;
    if (ncopy > buflen) ncopy = buflen;

    if (usercopy_to_user(user_buf, val, ncopy) != 0) return -EFAULT;

    // if truncated, ensure last byte is NUL
    if (ncopy == buflen) {
        char z = 0;
        (void)usercopy_to_user(user_buf + buflen - 1, &z, 1);
    }

    return (int)vlen;
}
