/*
 * userfs.c — Userland RAM filesystem for IPC services
 *
 * Rooted at $/user. Processes register named nodes and expose them
 * via read/write callbacks. Other processes open those nodes by path
 * and communicate through them.
 *
 * Tree structure:
 *   g_root (DIR)
 *     └── <dir> (DIR)
 *           └── <name> (NODE)  ← registered by a user process
 *
 * Nodes are owned by an owner_id string. When a process exits, all
 * nodes it registered are removed and empty parent dirs are pruned.
 *
 * Reads and writes go directly through the node's ops callbacks so
 * the registering process controls the data — no shared kernel ring
 * buffer, no per-open state confusion.
 */

#include "moduos/fs/userfs.h"
#include "moduos/fs/fd.h"
#include "moduos/kernel/memory/memory.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/interrupts/irq_lock.h"
#include "moduos/kernel/COM/com.h"
#include "moduos/kernel/process/process.h"
#include "moduos/kernel/memory/usercopy.h"

/* =========================================================
 * Internal tree types
 * ========================================================= */

typedef enum {
    UNODE_DIR = 0,
    UNODE_DEV = 1,
} unode_type_t;

typedef struct unode {
    unode_type_t     type;
    char             name[64];

    struct unode    *parent;
    struct unode    *children;  /* first child (linked via sibling) */
    struct unode    *sibling;   /* next sibling in parent's child list */

    /* DEV-only fields */
    userfs_user_ops_t ops;
    void             *ops_ctx;
    char             *owner_id; /* heap-allocated copy */
    uint32_t          owner_pid; /* PID of registering process */
    uint32_t          perms;
    struct unode     *owner_next; /* linked list per-owner for fast cleanup */
} unode_t;

typedef struct {
    unode_t *node;
    int      flags;
} uhandle_t;

/* =========================================================
 * Globals
 * ========================================================= */

static unode_t *g_root        = NULL;
static unode_t *g_owned_head  = NULL; /* head of all DEV nodes via owner_next */
static int      g_inited      = 0;

/* =========================================================
 * Logging
 * ========================================================= */

static void ufs_log(const char *op, const char *path, int ok) {
    com_write_string(COM1_PORT, "[USERFS] ");
    com_write_string(COM1_PORT, op);
    if (path && *path) {
        com_write_string(COM1_PORT, " ");
        com_write_string(COM1_PORT, path);
    }
    com_write_string(COM1_PORT, ok ? " OK\n" : " FAIL\n");
}

/* =========================================================
 * Init
 * ========================================================= */

static void ufs_init(void) {
    if (g_inited) return;
    g_root = (unode_t *)kmalloc(sizeof(unode_t));
    if (!g_root) return;
    memset(g_root, 0, sizeof(unode_t));
    g_root->type = UNODE_DIR;
    g_inited = 1;
}

/* =========================================================
 * Path helpers
 * ========================================================= */

/*
 * Strip the $/user/ prefix so everything below works on bare
 * relative paths like "foo/bar".
 *
 * Accepted: $/user/foo  $user/foo  /user/foo  user/foo  foo
 * Returns pointer into the original string, or NULL for root-only.
 */
static const char *ufs_strip_prefix(const char *path) {
    if (!path) return NULL;

    com_write_string(COM1_PORT, "[STRIP] in='");
    com_write_string(COM1_PORT, path);
    com_write_string(COM1_PORT, "'\n");

    if (path[0] == '$') {
        path++;
        if (path[0] == '/') path++;
    } else if (path[0] == '/') {
        path++;
    }

    if (strncmp(path, "user", 4) == 0) {
        if (path[4] == '/') {
            path += 5;
        } else if (path[4] == '\0') {
            return NULL;
        }
    }

    while (*path == '/') path++;

    com_write_string(COM1_PORT, "[STRIP] out='");
    if (*path) com_write_string(COM1_PORT, path);
    else com_write_string(COM1_PORT, "(null)");
    com_write_string(COM1_PORT, "'\n");

    return *path ? path : NULL;
}

/*
 * Read the next path segment from *pp into seg[seg_sz].
 * Advances *pp past the segment and any trailing slashes.
 * Returns 1 if a segment was written, 0 if the path is exhausted.
 */
static int ufs_next_seg(const char **pp, char *seg, size_t seg_sz) {
    const char *p = *pp;

    com_write_string(COM1_PORT, "[SEG] input='");
    if (p && *p) com_write_string(COM1_PORT, p);
    else com_write_string(COM1_PORT, "(null)");
    com_write_string(COM1_PORT, "'\n");

    while (*p == '/') p++;
    if (!*p) {
        com_write_string(COM1_PORT, "[SEG] END\n");
        return 0;
    }

    size_t i = 0;
    while (p[i] && p[i] != '/') {
        if (i + 1 < seg_sz) seg[i] = p[i];
        i++;
    }

    if (seg_sz > 0)
        seg[i < seg_sz ? i : seg_sz - 1] = '\0';

    if (i == 0) {
        com_write_string(COM1_PORT, "[BUG] zero-length segment!\n");
    }

    com_write_string(COM1_PORT, "[SEG] out='");
    com_write_string(COM1_PORT, seg);
    com_write_string(COM1_PORT, "' len=");
    char ibuf[16];
    itoa(i, ibuf, 10);
    com_write_string(COM1_PORT, ibuf);
    com_write_string(COM1_PORT, "\n");

    p += i;

    while (*p == '/') p++;

    /* CRITICAL POINTER LOG */
    com_write_string(COM1_PORT, "[SEG] next='");
    if (*p) com_write_string(COM1_PORT, p);
    else com_write_string(COM1_PORT, "(end)");
    com_write_string(COM1_PORT, "'\n\n");

    *pp = p;
    return 1;
}

/* =========================================================
 * Tree operations
 * ========================================================= */

static unode_t *ufs_find_child(unode_t *dir, const char *name) {
    if (!dir || dir->type != UNODE_DIR || !name) return NULL;
    for (unode_t *c = dir->children; c; c = c->sibling) {
        if (strcmp(c->name, name) == 0) return c;
    }
    return NULL;
}

static void ufs_add_child(unode_t *dir, unode_t *child) {
    child->parent  = dir;
    child->sibling = dir->children;
    dir->children  = child;
}

static unode_t *ufs_new_node(unode_type_t type, const char *name, unode_t *parent) {
    unode_t *n = (unode_t *)kmalloc(sizeof(unode_t));
    if (!n) return NULL;
    memset(n, 0, sizeof(unode_t));
    n->type   = type;
    n->parent = parent;
    if (name) {
        strncpy(n->name, name, sizeof(n->name) - 1);
        n->name[sizeof(n->name) - 1] = '\0';
    }
    return n;
}

/*
 * Walk the tree along 'rel_path' (bare, no leading $/user).
 * NULL or empty path returns g_root.
 */
static unode_t *ufs_walk(const char *rel_path) {
    ufs_init();
    if (!g_root) return NULL;
    if (!rel_path || !*rel_path) return g_root;

    unode_t    *cur = g_root;
    const char *p   = rel_path;
    char        seg[64];

    while (ufs_next_seg(&p, seg, sizeof(seg))) {
        unode_t *c = ufs_find_child(cur, seg);
        if (!c) return NULL;
        cur = c;
    }
    return cur;
}

/* Unlink child from parent's child list (does NOT free). */
static void ufs_unlink_child(unode_t *parent, unode_t *child) {
    if (!parent || !child) return;
    unode_t **pp = &parent->children;
    while (*pp) {
        if (*pp == child) { *pp = child->sibling; child->sibling = NULL; return; }
        pp = &(*pp)->sibling;
    }
}

/* Recursively free a node and all descendants. */
static void ufs_free_node(unode_t *n) {
    if (!n) return;
    /* free children first */
    unode_t *c = n->children;
    while (c) {
        unode_t *next = c->sibling;
        ufs_free_node(c);
        c = next;
    }
    if (n->owner_id) { kfree(n->owner_id); n->owner_id = NULL; }
    kfree(n);
}

/* Walk up and prune empty directories. */
static void ufs_prune(unode_t *dir) {
    while (dir && dir->parent && dir->type == UNODE_DIR && !dir->children) {
        unode_t *p = dir->parent;
        ufs_unlink_child(p, dir);
        kfree(dir);
        dir = p;
    }
}

/* =========================================================
 * Permission check
 * ========================================================= */

static int ufs_check_access(unode_t *node, int flags, int is_invoke)
{
    if (!node) return 0;

    /* Special case for invoke */
    if (is_invoke) {
        return (node->perms & USERFS_PERM_INVOKE) != 0;
    }

    /* Normal read/write checks */
    int want_r = ((flags & O_WRONLY) == 0);
    int want_w = ((flags & (O_WRONLY | O_RDWR)) != 0);

    int can_r = (node->perms & USERFS_PERM_READ_ONLY)  ||
                (node->perms & USERFS_PERM_READ_WRITE);

    int can_w = (node->perms & USERFS_PERM_WRITE_ONLY) ||
                (node->perms & USERFS_PERM_READ_WRITE);

    if (want_r && !can_r) return 0;
    if (want_w && !can_w) return 0;

    return 1;
}

/* =========================================================
 * Public API
 * ========================================================= */

/*
 * Register a DEV node at 'path' (relative to $/user) owned by
 * 'owner_id'.  Intermediate directories are created automatically.
 *
 * The node's read/write callbacks are set later via the ops fields of
 * userfs_user_node_t — for now the kernel side stores perms only; the
 * syscall layer patches ops/ctx after registration.
 */
int userfs_register_user_path(const char *path, const char *owner_id, uint32_t perms) {
    ufs_init();
    if (!g_root || !path || !*path || !owner_id) return -1;

    const char *rel = ufs_strip_prefix(path);
    if (!rel) return -1; /* registering root itself is not allowed */

    /* Collect segments */
#define UFS_MAX_SEGS 32
    char       segs[UFS_MAX_SEGS][64];
    int        nseg = 0;
    const char *p   = rel;
    char        seg[64];

    while (ufs_next_seg(&p, seg, sizeof(seg))) {
        com_write_string(COM1_PORT, "[PATH BUILD] segment='");
        com_write_string(COM1_PORT, seg);
        com_write_string(COM1_PORT, "'\n");

        if (nseg >= UFS_MAX_SEGS) return -8;

        strncpy(segs[nseg], seg, sizeof(segs[nseg]) - 1);
        segs[nseg][sizeof(segs[nseg]) - 1] = '\0';
        nseg++;
    }
    if (nseg == 0) return -4;

    /* Walk / create intermediate dirs */
    unode_t *cur = g_root;
    for (int i = 0; i < nseg - 1; i++) {
        unode_t *c = ufs_find_child(cur, segs[i]);
        if (c) {
            if (c->type != UNODE_DIR) return -2; /* collision with a file */
            cur = c;
        } else {
            unode_t *nd = ufs_new_node(UNODE_DIR, segs[i], cur);
            if (!nd) return -3;
            ufs_add_child(cur, nd);
            cur = nd;
        }
    }

    /* Leaf node */
    const char *leaf = segs[nseg - 1];
    if (ufs_find_child(cur, leaf)) return -5; /* already registered */

    unode_t *nd = ufs_new_node(UNODE_DEV, leaf, cur);
    if (!nd) return -6;

    /* Heap-copy owner_id so the node owns its lifetime */
    size_t olen   = strlen(owner_id) + 1;
    char  *oid_cp = (char *)kmalloc(olen);
    if (!oid_cp) { kfree(nd); return -7; }
    memcpy(oid_cp, owner_id, olen);

    nd->owner_id  = oid_cp;
    nd->owner_pid = process_get_current() ? process_get_current()->pid : 0;
    nd->perms     = perms ? perms : USERFS_PERM_READ_WRITE;

    ufs_add_child(cur, nd);

    /* Add to global owner list for fast cleanup */
    nd->owner_next = g_owned_head;
    g_owned_head   = nd;

    ufs_log("register", rel, 1);
    return 0;
}

/*
 * Open a node at 'path'.  Returns an opaque handle or NULL on failure.
 */
void *userfs_open_path(const char *path, int flags) {
    if (!path) return NULL;

    const char *rel = ufs_strip_prefix(path);
    if (!rel) return NULL;

    unode_t *node = ufs_walk(rel);
    if (!node || node->type != UNODE_DEV) {
        ufs_log("open", rel, 0);
        return NULL;
    }

    /* Use new signature with is_invoke=0 */
    if (!ufs_check_access(node, flags, 0)) {
        ufs_log("open", rel, 0);
        return NULL;
    }

    uhandle_t *h = (uhandle_t *)kmalloc(sizeof(uhandle_t));
    if (!h) return NULL;
    h->node  = node;
    h->flags = flags;

    ufs_log("open", rel, 1);
    return h;
}

static uint64_t userfs_get_owner_cr3(unode_t *node) {
    if (!node || node->owner_pid == 0) return 0;
    process_t *owner = process_find(node->owner_pid);
    if (!owner) return 0;
    if (owner->page_table) return owner->page_table & ~0xFFFULL;
    return owner->cr3 & ~0xFFFULL;
}

static ssize_t userfs_invoke_owner_callback(unode_t *node, int is_read,
                                            const void *in_buf, size_t in_size,
                                            void *out_buf, size_t out_size)
{
    if (!node) return -1;

    uint64_t owner_cr3 = userfs_get_owner_cr3(node);
    if (node->owner_pid != 0 && owner_cr3 == 0) {
        return -1;
    }

    uint64_t irq_flags = irq_save();

    uint64_t old_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(old_cr3));

    if (owner_cr3 && (old_cr3 & ~0xFFFULL) != owner_cr3) {
        __asm__ volatile("mov %0, %%cr3" :: "r"(owner_cr3) : "memory");
    }

    ssize_t rc = -1;

    if (is_read == 1) {
        if (node->ops.read)
            rc = node->ops.read(node->ops_ctx, out_buf, out_size);
    }
    else if (is_read == 0) {
        if (node->ops.write)
            rc = node->ops.write(node->ops_ctx, in_buf, in_size);
    }
    else {
        rc = node->ops.invoke(node->ops_ctx, in_buf, in_size, out_buf, out_size);
    }

    if (owner_cr3 && (old_cr3 & ~0xFFFULL) != owner_cr3) {
        __asm__ volatile("mov %0, %%cr3" :: "r"(old_cr3) : "memory");
    }

    irq_restore(irq_flags);

    return rc;
}

/*
 * Read from handle.  Delegates straight to the node's read callback.
 * If no callback is set, returns -1.
 */
ssize_t userfs_read(void *handle, void *buf, size_t count)
{
    uhandle_t *h = (uhandle_t *)handle;
    if (!h || !h->node || h->node->type != UNODE_DEV) return -1;
    if (!buf || count == 0) return -1;

    if (!ufs_check_access(h->node, h->flags | O_RDONLY, 0)) return -2;

    if (!h->node->ops.read) return -1;

    void *kbuf = kmalloc(count);
    if (!kbuf) return -1;

    ssize_t rc = userfs_invoke_owner_callback(h->node, 1, NULL, 0, kbuf, count);

    if (rc > 0) {
        if (usercopy_to_user(buf, kbuf, (size_t)rc) != 0) {
            rc = -1;
        }
    }

    kfree(kbuf);
    return rc;
}

/*
 * Write to handle.  Delegates straight to the node's write callback.
 */
ssize_t userfs_write(void *handle, const void *buf, size_t count)
{
    uhandle_t *h = (uhandle_t *)handle;
    if (!h || !h->node || h->node->type != UNODE_DEV) return -1;
    if (!buf || count == 0) return -1;

    if (!ufs_check_access(h->node, h->flags | O_WRONLY, 0)) return -2;

    if (!h->node->ops.write) return -1;

    void *kbuf = kmalloc(count);
    if (!kbuf) return -1;

    if (usercopy_from_user(kbuf, buf, count) != 0) {
        kfree(kbuf);
        return -1;
    }

    ssize_t rc = userfs_invoke_owner_callback(h->node, 0, kbuf, count, NULL, 0);

    kfree(kbuf);
    return rc;
}

/*
 * Invoke (ioctl-style) on a node.
 * Sends input buffer and receives output buffer in one call.
 */
ssize_t userfs_invoke(void *handle,
                      const void *in_buf,  size_t in_size,
                      void *out_buf, size_t out_size)
{
    uhandle_t *h = (uhandle_t *)handle;
    if (!h || !h->node || h->node->type != UNODE_DEV)
        return -1;

    if (!ufs_check_access(h->node, 0, 1))
        return -2;

    if (!h->node->ops.invoke)
        return -1;

    void *kin  = (in_size  > 0) ? kmalloc(in_size)  : NULL;
    void *kout = (out_size > 0) ? kmalloc(out_size) : NULL;

    if ((in_size > 0 && !kin) || (out_size > 0 && !kout)) {
        kfree(kin);
        kfree(kout);
        return -1;
    }

    if (in_size > 0) {
        if (usercopy_from_user(kin, in_buf, in_size) != 0) {
            kfree(kin); kfree(kout);
            return -1;
        }
    }

    ssize_t rc = userfs_invoke_owner_callback(h->node, -1, kin, in_size, kout, out_size);

    if (rc > 0 && out_size > 0) {
        if (usercopy_to_user(out_buf, kout, (size_t)rc) != 0) {
            rc = -1;
        }
    }

    kfree(kin);
    kfree(kout);
    return rc;
}

/*
 * Close handle.  Just frees the handle; the node stays registered.
 */
void userfs_close(void *handle) {
    if (!handle) return;
    kfree(handle);
}

/*
 * Returns 1 if 'path' resolves to an existing directory node.
 */
int userfs_directory_exists(const char *path) {
    const char *rel  = path && *path ? ufs_strip_prefix(path) : NULL;
    unode_t    *node = ufs_walk(rel ? rel : "");
    return (node && node->type == UNODE_DIR) ? 1 : 0;
}

/*
 * Directory listing.  *cookie starts at 0; each call advances it.
 * Returns 1 and fills name_buf/is_dir on success, 0 when exhausted.
 */
int userfs_list_dir_next(const char *path, int *cookie, char *name_buf, size_t buf_size, int *is_dir) {
    if (!cookie || !name_buf || buf_size == 0) return -1;

    const char *rel  = path && *path ? ufs_strip_prefix(path) : NULL;
    unode_t    *dir  = ufs_walk(rel ? rel : "");

    if (!dir || dir->type != UNODE_DIR) return -1;

    int idx = 0, target = *cookie;
    for (unode_t *c = dir->children; c; c = c->sibling) {
        if (idx == target) {
            strncpy(name_buf, c->name, buf_size - 1);
            name_buf[buf_size - 1] = '\0';
            if (is_dir) *is_dir = (c->type == UNODE_DIR);
            *cookie = target + 1;
            return 1;
        }
        idx++;
    }
    return 0;
}

/*
 * Called when a process exits.  Removes all nodes owned by owner_id
 * and prunes any directories that become empty as a result.
 */
void userfs_owner_exited(const char *owner_id) {
    if (!owner_id) return;

    unode_t **pp = &g_owned_head;
    while (*pp) {
        unode_t *n = *pp;
        if (n->owner_id && strcmp(n->owner_id, owner_id) == 0) {
            *pp = n->owner_next; /* remove from owner list */
            unode_t *parent = n->parent;
            if (parent) ufs_unlink_child(parent, n);
            ufs_free_node(n);
            if (parent) ufs_prune(parent);
            /* don't advance pp — *pp is already the next element */
            continue;
        }
        pp = &(*pp)->owner_next;
    }

    ufs_log("owner_exited", owner_id, 1);
}

/*
 * Called when a process exits.  Removes all nodes whose owner_pid matches
 * the given pid, regardless of owner_id.  This is the correct cleanup path
 * for process exit — owner_id is an arbitrary user-supplied string and cannot
 * be reliably derived from process metadata (e.g. user_identity_get only
 * returns "root" for uid=0 processes, not the actual service name).
 */
void userfs_pid_exited(uint32_t pid) {
    if (pid == 0) return;

    char pid_str[16];
    /* Build a pid string for the log message */
    int i = 0;
    uint32_t v = pid;
    char tmp[12]; int tlen = 0;
    if (v == 0) { tmp[tlen++] = '0'; }
    while (v > 0 && tlen < 11) { tmp[tlen++] = '0' + (v % 10); v /= 10; }
    while (tlen > 0 && i < 14) pid_str[i++] = tmp[--tlen];
    pid_str[i] = '\0';

    unode_t **pp = &g_owned_head;
    while (*pp) {
        unode_t *n = *pp;
        if (n->owner_pid == pid) {
            *pp = n->owner_next;
            unode_t *parent = n->parent;
            if (parent) ufs_unlink_child(parent, n);
            ufs_free_node(n);
            if (parent) ufs_prune(parent);
            continue;
        }
        pp = &(*pp)->owner_next;
    }

    ufs_log("pid_exited", pid_str, 1);
}

/*
 * Set read/write callbacks on an already-registered node.
 * Called by the syscall layer after userfs_register_user_path succeeds.
 *
 * Returns 0 on success, -1 if the path cannot be found or is not a DEV node.
 */
int userfs_set_ops(const char *path, const userfs_user_ops_t *ops, void *ctx) {
    if (!path || !ops) return -1;

    const char *rel  = ufs_strip_prefix(path);
    unode_t    *node = ufs_walk(rel ? rel : "");

    if (!node || node->type != UNODE_DEV) return -1;

    node->ops     = *ops;
    node->ops_ctx = ctx;
    return 0;
}