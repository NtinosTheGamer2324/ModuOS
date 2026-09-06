#include "moduos/fs/devfs.h"
#include "moduos/kernel/memory/memory.h"
#include "moduos/kernel/memory/paging.h"
#include "moduos/kernel/memory/phys.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/process/process.h"
#include "moduos/kernel/COM/com.h"
#include "moduos/kernel/interrupts/irq_lock.h"
#include "moduos/kernel/interrupts/hlt_wait.h"
#include "moduos/kernel/spinlock.h"

/*
 * devfs.c — ModuOS DevFS
 *
 * Exposes $/dev as a tree of character-device nodes.  Each node has
 * an associated devfs_device_ops_t that provides open/read/write/close
 * and, optionally, mmap.
 *
 * Graphics ($/dev/graphics/video0 / VIDEOCTL2) were removed in this
 * revision.  All graphics go through MVC3 ($/dev/mvc/mvi0) which is
 * registered by the mvc3.sqrm kernel module via devfs_register_path().
 *
 * The net directory ($/dev/net/) is created on demand by SQRM network
 * modules; devfs_net_init() has been removed accordingly.
 */

#define DEVFS_MAX_DEVICES 32

/* ── MAP_FAILED sentinel ────────────────────────────────────────────── */
#define DEVFS_MAP_FAILED ((void*)-1)

/* ── Tree node types ────────────────────────────────────────────────── */

typedef enum {
    DEVFS_NODE_DIR = 0,
    DEVFS_NODE_DEV = 1,
} devfs_node_type_t;

typedef struct devfs_node {
    devfs_node_type_t type;
    char name[64];

    /* directory */
    struct devfs_node *parent;
    struct devfs_node *children;
    struct devfs_node *next;

    /* device */
    const devfs_device_ops_t *ops;
    void *ctx;
    devfs_owner_t owner;
    int user_owned;
    void *user_ctx;
} devfs_node_t;

/* Legacy flat device table (kept for vDrives/other root devices). */
typedef struct {
    const devfs_device_ops_t *ops;
    void *ctx;
    int in_use;
} devfs_device_t;

/* Per-open handle (wraps either a tree node or a legacy flat device). */
typedef struct {
    int flags;
    int is_tree;
    const devfs_device_ops_t *ops;
    void *opened_ctx;
    union {
        devfs_node_t  *node;   /* tree device  */
        devfs_device_t *legacy; /* legacy flat  */
    } u;
} devfs_handle_t;

/* ── Global state ────────────────────────────────────────────────────── */

static devfs_device_t g_devices[DEVFS_MAX_DEVICES];
static devfs_node_t  *g_root   = NULL;
static int            g_inited = 0;

/* ── Input device state ──────────────────────────────────────────────── */

typedef struct {
    char buf[512];
    volatile uint32_t r;
    volatile uint32_t w;
    volatile uint32_t count;
    int flags;
} devfs_kbd_stream_t;

typedef struct {
    Event buf[128];
    volatile uint32_t r;
    volatile uint32_t w;
    volatile uint32_t count;
    int flags;
} devfs_event_stream_t;

static devfs_kbd_stream_t   g_kbd0;
static devfs_event_stream_t g_evt0;

/* ── Node helpers ────────────────────────────────────────────────────── */

static devfs_node_t *devfs_new_node(devfs_node_type_t type,
                                    const char *name,
                                    devfs_node_t *parent) {
    devfs_node_t *n = (devfs_node_t *)kmalloc(sizeof(devfs_node_t));
    if (!n) return NULL;
    memset(n, 0, sizeof(*n));
    n->type   = type;
    n->parent = parent;
    if (name) {
        strncpy(n->name, name, sizeof(n->name) - 1);
        n->name[sizeof(n->name) - 1] = 0;
    }
    return n;
}

static devfs_node_t *devfs_find_child(devfs_node_t *dir, const char *name) {
    if (!dir || dir->type != DEVFS_NODE_DIR || !name) return NULL;
    for (devfs_node_t *c = dir->children; c; c = c->next)
        if (strcmp(c->name, name) == 0) return c;
    return NULL;
}

static devfs_node_t *devfs_add_child(devfs_node_t *dir, devfs_node_t *child) {
    if (!dir || dir->type != DEVFS_NODE_DIR || !child) return NULL;
    child->next    = dir->children;
    dir->children  = child;
    child->parent  = dir;
    return child;
}

static void devfs_free_owner(devfs_owner_t *owner) {
    if (!owner) return;
    if (owner->kind == DEVFS_OWNER_USER && owner->id) {
        kfree((void *)owner->id);
        owner->id = NULL;
    }
}

static void devfs_free_user_ctx(devfs_node_t *node) {
    if (!node) return;
    if (node->owner.kind == DEVFS_OWNER_USER && node->ctx) {
        kfree(node->ctx);
        node->ctx = NULL;
    }
}

/* Strip accepted prefixes so callers can pass $/dev/..., /dev/..., or bare paths. */
static const char *devfs_normalize_path(const char *path) {
    if (!path) return NULL;
    if (strncmp(path, "$/dev", 5) == 0) {
        path += 5;
        if (*path == '/') path++;
    } else if (strncmp(path, "/dev", 4) == 0) {
        path += 4;
        if (*path == '/') path++;
    }
    while (*path == '/') path++;
    return path;
}

static int devfs_is_reserved_for_sqrm(const char *path) {
    if (!path || !path[0]) return 0;
    if (strstr(path, "vDrive") != NULL) return 1;
    if (strncmp(path, "mnt", 3) == 0) return 1;
    return 0;
}

static void devfs_init_once(void) {
    if (g_inited) return;
    memset(g_devices, 0, sizeof(g_devices));
    g_root   = devfs_new_node(DEVFS_NODE_DIR, "dev", NULL);
    g_inited = 1;
}

/* Walk `p` forward by one path segment, writing the segment to `seg`. */
static const char *devfs_path_next(const char *p, char *seg, size_t seg_sz) {
    while (*p == '/') p++;
    if (!*p) return NULL;
    size_t i = 0;
    while (p[i] && p[i] != '/') {
        if (i + 1 < seg_sz) seg[i] = p[i];
        i++;
    }
    if (seg_sz) seg[(i < seg_sz) ? i : (seg_sz - 1)] = 0;
    return p + i;
}

/* ── Public: mkdir -p ────────────────────────────────────────────────── */

int devfs_mkdir_p(const char *path, devfs_owner_t owner) {
    (void)owner;
    devfs_init_once();
    if (!g_root || !path || !path[0]) return 0;

    devfs_node_t *cur = g_root;
    const char   *p   = path;
    char          seg[64];

    while ((p = devfs_path_next(p, seg, sizeof(seg))) != NULL) {
        if (!seg[0]) break;
        devfs_node_t *c = devfs_find_child(cur, seg);
        if (c) {
            if (c->type != DEVFS_NODE_DIR) return -2;
            cur = c;
        } else {
            devfs_node_t *nd = devfs_new_node(DEVFS_NODE_DIR, seg, cur);
            if (!nd) return -3;
            devfs_add_child(cur, nd);
            cur = nd;
        }
        while (*p == '/') p++;
    }
    return 0;
}

/* ── Public: register a device at a path ────────────────────────────── */

int devfs_register_path(const char *path, const devfs_device_ops_t *ops,
                        void *ctx, devfs_owner_t owner) {
    devfs_init_once();
    if (!g_root)                              return -1;
    if (!path || !path[0])                    return -2;
    if (!ops || !ops->name || !ops->name[0]) return -3;

    const char *path_in = devfs_normalize_path(path);
    if (!path_in || !path_in[0]) return -2;

    if (owner.kind == DEVFS_OWNER_SQRM && devfs_is_reserved_for_sqrm(path_in))
        return -4;

    devfs_node_t *cur = g_root;
    const char   *p   = path_in;
    char          seg[64];
    char          last[64];
    last[0] = 0;

    while ((p = devfs_path_next(p, seg, sizeof(seg))) != NULL) {
        if (!seg[0]) break;
        strncpy(last, seg, sizeof(last) - 1);
        last[sizeof(last) - 1] = 0;

        /* Is there another segment after this one? */
        const char *q = p;
        while (*q == '/') q++;
        int has_more = (*q != 0);
        if (!has_more) break;

        devfs_node_t *c = devfs_find_child(cur, seg);
        if (c) {
            if (c->type != DEVFS_NODE_DIR) return -5;
            cur = c;
        } else {
            devfs_node_t *nd = devfs_new_node(DEVFS_NODE_DIR, seg, cur);
            if (!nd) return -6;
            devfs_add_child(cur, nd);
            cur = nd;
        }
        p = q;
    }

    if (!last[0]) return -7;

    devfs_node_t *existing = devfs_find_child(cur, last);
    if (existing) {
        if (existing->type == DEVFS_NODE_DIR) return -8;
        if (existing->owner.kind == DEVFS_OWNER_KERNEL) return -9;
        if (!existing->ops || !existing->ops->can_replace) return -10;

        devfs_replace_decision_t d = existing->ops->can_replace(
            existing->ctx, path,
            owner.id ? owner.id : "");
        if (d != DEVFS_REPLACE_ALLOW) return -11;

        if (existing->owner.kind == DEVFS_OWNER_USER ||
            owner.kind            == DEVFS_OWNER_USER) {
            const char *old_id = existing->owner.id ? existing->owner.id : "";
            const char *new_id = owner.id            ? owner.id            : "";
            if (strcmp(old_id, new_id) != 0) return -11;
        }

        devfs_free_user_ctx(existing);
        devfs_free_owner(&existing->owner);
        existing->ops        = ops;
        existing->user_owned = (owner.kind == DEVFS_OWNER_USER);
        existing->user_ctx   = NULL;
        existing->ctx        = ctx;
        existing->owner      = owner;
        return 0;
    }

    devfs_node_t *ndev = devfs_new_node(DEVFS_NODE_DEV, last, cur);
    if (!ndev) return -12;
    ndev->ops        = ops;
    ndev->user_owned = (owner.kind == DEVFS_OWNER_USER);
    ndev->user_ctx   = NULL;
    ndev->ctx        = ctx;
    ndev->owner      = owner;
    devfs_add_child(cur, ndev);
    return 0;
}

/* Legacy flat registration at root. */
int devfs_register(const devfs_device_ops_t *ops, void *ctx) {
    devfs_owner_t owner = { .kind = DEVFS_OWNER_KERNEL, .id = "kernel" };
    int r = devfs_register_path(ops->name, ops, ctx, owner);
    if (r == 0) {
        for (int i = 0; i < DEVFS_MAX_DEVICES; i++) {
            if (!g_devices[i].in_use) {
                g_devices[i].ops    = ops;
                g_devices[i].ctx    = ctx;
                g_devices[i].in_use = 1;
                break;
            }
        }
    }
    return r;
}

/* ── Internal: find legacy device by name ───────────────────────────── */

static devfs_device_t *devfs_find(const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < DEVFS_MAX_DEVICES; i++) {
        if (g_devices[i].in_use && g_devices[i].ops &&
            strcmp(g_devices[i].ops->name, name) == 0)
            return &g_devices[i];
    }
    return NULL;
}

/* ── Internal: walk the tree to find a node ─────────────────────────── */

static devfs_node_t *devfs_find_path_node(const char *path) {
    devfs_init_once();
    if (!g_root || !path) return NULL;

    devfs_node_t *cur = g_root;
    const char   *p   = path;
    char          seg[64];

    while ((p = devfs_path_next(p, seg, sizeof(seg))) != NULL) {
        if (!seg[0]) break;
        devfs_node_t *c = devfs_find_child(cur, seg);
        if (!c) return NULL;
        cur = c;
        while (*p == '/') p++;
    }
    return cur;
}

/* ── Helper: apply open flags to well-known stream devices ──────────── */

static void devfs_apply_open_flags(const devfs_device_ops_t *ops,
                                   void *dev_ctx, int flags) {
    if (!ops || !ops->name || !dev_ctx) return;
    if (strcmp(ops->name, "kbd0")   == 0)
        ((devfs_kbd_stream_t   *)dev_ctx)->flags = flags;
    else if (strcmp(ops->name, "event0") == 0)
        ((devfs_event_stream_t *)dev_ctx)->flags = flags;
}

/* ── Public: open ────────────────────────────────────────────────────── */

void *devfs_open(const char *name, int flags) {
    devfs_init_once();
    devfs_device_t *d = devfs_find(name);
    if (!d) return NULL;

    devfs_apply_open_flags(d->ops, d->ctx, flags);

    devfs_handle_t *h = (devfs_handle_t *)kmalloc(sizeof(devfs_handle_t));
    if (!h) return NULL;
    h->flags       = flags;
    h->is_tree     = 0;
    h->ops         = d->ops;
    h->opened_ctx  = (h->ops && h->ops->open)
                   ? h->ops->open(d->ctx, flags)
                   : d->ctx;
    h->u.legacy    = d;
    return h;
}

void *devfs_open_path(const char *path, int flags) {
    devfs_init_once();
    devfs_node_t *n = devfs_find_path_node(path);
    if (!n || n->type != DEVFS_NODE_DEV || !n->ops) return NULL;

    devfs_apply_open_flags(n->ops, n->ctx, flags);

    devfs_handle_t *h = (devfs_handle_t *)kmalloc(sizeof(devfs_handle_t));
    if (!h) return NULL;
    h->flags      = flags;
    h->is_tree    = 1;
    h->ops        = n->ops;
    h->opened_ctx = (h->ops && h->ops->open)
                  ? h->ops->open(n->ctx, flags)
                  : n->ctx;
    h->u.node     = n;
    return h;
}

/* ── Public: directory listing ──────────────────────────────────────── */

int devfs_list_dir_next(const char *dir_path, int *cookie,
                        char *name_buf, size_t buf_size, int *is_dir) {
    devfs_init_once();
    if (!cookie || !name_buf || buf_size == 0) return -1;

    devfs_node_t *dir = (dir_path && dir_path[0])
                      ? devfs_find_path_node(dir_path)
                      : g_root;
    if (!dir)                            return -2;
    if (dir->type != DEVFS_NODE_DIR)     return -3;

    int idx = 0;
    for (devfs_node_t *c = dir->children; c; c = c->next) {
        if (idx == *cookie) {
            strncpy(name_buf, c->name, buf_size - 1);
            name_buf[buf_size - 1] = 0;
            if (is_dir) *is_dir = (c->type == DEVFS_NODE_DIR);
            (*cookie)++;
            return 1;
        }
        idx++;
    }
    return 0;
}

/* ── Public: read / write / close ───────────────────────────────────── */

ssize_t devfs_read(void *handle, void *buf, size_t count) {
    devfs_handle_t *h = (devfs_handle_t *)handle;
    if (!h || !h->ops || !h->ops->read) return -1;
    return h->ops->read(h->opened_ctx, buf, count);
}

ssize_t devfs_write(void *handle, const void *buf, size_t count) {
    devfs_handle_t *h = (devfs_handle_t *)handle;
    if (!h || !h->ops || !h->ops->write) return -1;
    return h->ops->write(h->opened_ctx, buf, count);
}

int devfs_close(void *handle) {
    devfs_handle_t *h = (devfs_handle_t *)handle;
    if (!h) return -1;
    if (h->ops && h->ops->close)
        h->ops->close(h->opened_ctx);
    kfree(h);
    return 0;
}

ssize_t devfs_invoke(void *handle, const void *in_buf, size_t in_size,
                     void *out_buf, size_t out_size) {
    devfs_handle_t *h = (devfs_handle_t *)handle;
    if (!h || !h->ops || !h->ops->invoke) return -1;
    return h->ops->invoke(h->opened_ctx, in_buf, in_size, out_buf, out_size);
}

/* ── Public: mmap ────────────────────────────────────────────────────── */

/*
 * devfs_mmap — route an mmap() syscall to the device's mmap hook.
 *
 * Called from sys_mmap (or a dedicated SYS_DEV_MMAP syscall) when the
 * address being mapped refers to an open device file descriptor.
 *
 * Contract:
 *  • If the device has no mmap hook → return MAP_FAILED immediately.
 *  • Otherwise delegate entirely to ops->mmap; that function is responsible
 *    for paging_map_range() and returning the user VA.
 *  • length must be > 0; we reject 0 here before touching the device.
 *
 * Example device mmap implementation (e.g. for a framebuffer):
 *
 *   static void *mydev_mmap(void *ctx, void *hint, size_t length,
 *                           int prot, int flags, uint64_t offset) {
 *       mydev_t *d = ctx;
 *       uint64_t phys = d->phys_base + offset;
 *       uint64_t sz   = (length + 0xFFFULL) & ~0xFFFULL;
 *       uint64_t ua   = pick_user_va(hint, sz);   // device-managed VA bump ptr
 *       uint64_t pflags = PFLAG_PRESENT | PFLAG_USER;
 *       if (prot & 2) pflags |= PFLAG_WRITABLE;
 *       if (paging_map_range(ua, phys, sz, pflags) != 0)
 *           return (void*)-1;
 *       return (void*)(uintptr_t)ua;
 *   }
 */
void *devfs_mmap(void *handle, void *hint, size_t length,
                 int prot, int flags, uint64_t offset) {
    devfs_handle_t *h = (devfs_handle_t *)handle;
    if (!h || !h->ops) return DEVFS_MAP_FAILED;
    if (!h->ops->mmap)  return DEVFS_MAP_FAILED;   /* device does not support mmap */
    if (length == 0)    return DEVFS_MAP_FAILED;

    return h->ops->mmap(h->opened_ctx, hint, length, prot, flags, offset);
}

void *devfs_mmap_region(uint64_t phys_or_virt, size_t size, int prot, int is_phys) {
    if (size == 0) return (void*)-1;

    uint64_t sz = ((uint64_t)size + 0xFFFULL) & ~0xFFFULL;

    uint64_t phys;
    if (is_phys) {
        phys = phys_or_virt & ~0xFFFULL;
    } else {
        /* Kernel heap VA -> physical via physmap offset.
         * Do NOT use paging_virt_to_phys() here — it walks the live CR3
         * which during a syscall is the process CR3, and the process page
         * tables may not have the kernel heap mapped correctly (or may map
         * it to a wrong physical page due to shared PML4 entries).
         * The physmap is a direct offset: phys = kva - phys_offset.
         */
        uint64_t offset = paging_get_phys_offset();
        phys = (phys_or_virt & ~0xFFFULL) - offset;
    }

    /* Get current process */
    process_t *p = process_get_current();
    if (!p || !p->is_user) return (void*)-1;

    /* Pick user VA from mmap bump pointer */
    uint64_t uva = (p->user_mmap_end + 0xFFFULL) & ~0xFFFULL;
    if (uva < p->user_mmap_base) uva = p->user_mmap_base;
    if (uva + sz > p->user_mmap_limit) return (void*)-1;

    /* Build page flags */
    uint64_t pflags = PFLAG_PRESENT | PFLAG_USER;
    if (prot & 2) pflags |= PFLAG_WRITABLE;

    /* Map into the process's own page table */
    uint64_t proc_cr3   = p->page_table;
    uint64_t *proc_pml4 = (uint64_t*)phys_to_virt_kernel(proc_cr3 & ~0xFFFULL);
    if (!proc_pml4) return (void*)-1;

    /* Map page by page using the correct physical address from physmap math */
    for (uint64_t off = 0; off < sz; off += 0x1000ULL) {
        if (paging_map_range_to_pml4(proc_pml4,
                                     uva + off,
                                     phys + off,
                                     0x1000ULL,
                                     pflags) != 0)
            return (void*)-1;
    }

    /* Advance the bump pointer */
    p->user_mmap_end = uva + sz;

    return (void*)(uintptr_t)uva;
}

/* ── Public: legacy list ─────────────────────────────────────────────── */

int devfs_list_next(int *cookie, char *name_buf, size_t buf_size) {
    devfs_init_once();
    if (!cookie || !name_buf || buf_size == 0) return -1;

    int idx = *cookie;
    if (idx < 0) idx = 0;

    for (; idx < DEVFS_MAX_DEVICES; idx++) {
        if (g_devices[idx].in_use && g_devices[idx].ops &&
            g_devices[idx].ops->name) {
            strncpy(name_buf, g_devices[idx].ops->name, buf_size - 1);
            name_buf[buf_size - 1] = 0;
            *cookie = idx + 1;
            return 1;
        }
    }
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════
   Input devices  ─  $/dev/input/kbd0  and  $/dev/input/event0
   ══════════════════════════════════════════════════════════════════════ */

static void kbd_stream_push(devfs_kbd_stream_t *s, char c) {
    if (!s) return;
    uint64_t f = irq_save();
    uint32_t cap = (uint32_t)(sizeof(s->buf) / sizeof(s->buf[0]));
    if (s->count < cap) {
        s->buf[s->w] = c;
        s->w = (s->w + 1) % cap;
        s->count++;
    }
    irq_restore(f);
}

static int kbd_stream_pop(devfs_kbd_stream_t *s, char *out) {
    if (!s || !out) return 0;
    uint64_t f = irq_save();
    if (s->count == 0) { irq_restore(f); return 0; }
    *out = s->buf[s->r];
    s->r = (s->r + 1) % (uint32_t)(sizeof(s->buf) / sizeof(s->buf[0]));
    s->count--;
    irq_restore(f);
    return 1;
}

static void evt_stream_push(devfs_event_stream_t *s, const Event *e) {
    if (!s || !e) return;
    uint64_t f = irq_save();
    uint32_t cap = (uint32_t)(sizeof(s->buf) / sizeof(s->buf[0]));
    if (s->count < cap) {
        s->buf[s->w] = *e;
        s->w = (s->w + 1) % cap;
        s->count++;
    }
    irq_restore(f);
}

static int evt_stream_pop(devfs_event_stream_t *s, Event *out) {
    if (!s || !out) return 0;
    uint64_t f = irq_save();
    if (s->count == 0) { irq_restore(f); return 0; }
    *out = s->buf[s->r];
    s->r = (s->r + 1) % (uint32_t)(sizeof(s->buf) / sizeof(s->buf[0]));
    s->count--;
    irq_restore(f);
    return 1;
}

static ssize_t dev_kbd_read(void *ctx, void *buf, size_t count) {
    devfs_kbd_stream_t *s = (devfs_kbd_stream_t *)ctx;
    if (!s || !buf) return -1;
    if ((s->flags & O_NONBLOCK) && s->count == 0) return 0;
    while (s->count == 0) hlt_wait_preserve_if();
    char *out = (char *)buf;
    size_t n = 0;
    while (n < count) {
        char c;
        if (!kbd_stream_pop(s, &c)) break;
        out[n++] = c;
        if (c == '\n') break;
    }
    return (ssize_t)n;
}

static ssize_t dev_evt_read(void *ctx, void *buf, size_t count) {
    devfs_event_stream_t *s = (devfs_event_stream_t *)ctx;
    if (!s || !buf) return -1;
    if ((s->flags & O_NONBLOCK) && s->count == 0) return 0;
    if (count < sizeof(Event)) return -2;
    while (s->count == 0) hlt_wait_preserve_if();
    Event e;
    if (!evt_stream_pop(s, &e)) return 0;
    memcpy(buf, &e, sizeof(Event));
    return (ssize_t)sizeof(Event);
}

static const devfs_device_ops_t g_dev_kbd0_ops = {
    .name  = "kbd0",
    .read  = dev_kbd_read,
    .write = NULL,
    .close = NULL,
    .mmap  = NULL,
};

static const devfs_device_ops_t g_dev_evt0_ops = {
    .name  = "event0",
    .read  = dev_evt_read,
    .write = NULL,
    .close = NULL,
    .mmap  = NULL,
};

int devfs_input_init(void) {
    devfs_init_once();
    memset(&g_kbd0, 0, sizeof(g_kbd0));
    memset(&g_evt0, 0, sizeof(g_evt0));
    g_kbd0.flags = 0;
    g_evt0.flags = 0;
    devfs_owner_t owner = { .kind = DEVFS_OWNER_KERNEL, .id = "kernel" };

    devfs_mkdir_p("input", owner);
    int r1 = devfs_register_path("input/kbd0",   &g_dev_kbd0_ops, &g_kbd0, owner);
    int r2 = devfs_register_path("input/event0", &g_dev_evt0_ops, &g_evt0, owner);
    if (r1 != 0) return r1;
    if (r2 != 0) return r2;
    com_write_string(COM1_PORT,
        "[DEVFS] Registered input devices: "
        "$/dev/input/kbd0, $/dev/input/event0\n");
    return 0;
}

/* ── Input event injection (called by PS/2 / USB HID) ──────────────── */

void devfs_input_push_event(const Event *e) {
    if (!e) return;

    evt_stream_push(&g_evt0, e);

    if (e->type != EVENT_KEY_PRESSED) return;

    /* Emit VT100/ANSI escape sequences for special keys onto kbd0. */
    const char *seq = NULL;
    switch (e->data.keyboard.keycode) {
        case KEY_ARROW_UP:    seq = "\x1b[A";  break;
        case KEY_ARROW_DOWN:  seq = "\x1b[B";  break;
        case KEY_ARROW_RIGHT: seq = "\x1b[C";  break;
        case KEY_ARROW_LEFT:  seq = "\x1b[D";  break;
        case KEY_HOME:        seq = "\x1b[H";  break;
        case KEY_END:         seq = "\x1b[F";  break;
        case KEY_INSERT:      seq = "\x1b[2~"; break;
        case KEY_DELETE:      seq = "\x1b[3~"; break;
        case KEY_PAGE_UP:     seq = "\x1b[5~"; break;
        case KEY_PAGE_DOWN:   seq = "\x1b[6~"; break;
        default: break;
    }
    if (seq) {
        for (const char *p = seq; *p; ++p)
            kbd_stream_push(&g_kbd0, *p);
        return;
    }

    char c = e->data.keyboard.ascii;
    if (e->data.keyboard.keycode == KEY_ENTER    && c == 0) c = '\n';
    if (e->data.keyboard.keycode == KEY_BACKSPACE && c == 0) c = '\b';
    if (c) kbd_stream_push(&g_kbd0, c);
}

/* ══════════════════════════════════════════════════════════════════════
   GUI stub  ─  $/dev/gui0  (no-op; a real GUI server registers itself)
   ══════════════════════════════════════════════════════════════════════ */

int devfs_gui_init(void) {
    return 0;
}

uint32_t devfs_gui_server_pid(void) {
    return 0;
}