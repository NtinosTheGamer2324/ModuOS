#include "moduos/fs/devfs.h"
#include "moduos/kernel/md64api_grp.h"
#include "moduos/kernel/memory/memory.h"
#include "moduos/kernel/memory/paging.h"
#include "moduos/kernel/memory/phys.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/process/process.h"
#include "moduos/kernel/COM/com.h"
#include "moduos/drivers/graphics/VGA.h"
#include "moduos/drivers/graphics/framebuffer.h"
#include "moduos/drivers/graphics/videoctl.h"
#include "moduos/kernel/interrupts/irq_lock.h"
#include "moduos/kernel/interrupts/hlt_wait.h"
#include "moduos/kernel/spinlock.h"

// Forward declarations
static ssize_t dev_video0_write(void *ctx, const void *buf, size_t count);

#define DEVFS_MAX_DEVICES 32

typedef enum {
    DEVFS_NODE_DIR = 0,
    DEVFS_NODE_DEV = 1,
} devfs_node_type_t;

typedef struct devfs_node {
    devfs_node_type_t type;
    char name[64];

    // directory
    struct devfs_node *parent;
    struct devfs_node *children;
    struct devfs_node *next;

    // device
    const devfs_device_ops_t *ops;
    void *ctx;
    devfs_owner_t owner;
    int user_owned;
    void *user_ctx;
} devfs_node_t;

// Legacy flat device table (kept for vDrives/other root devices until fd.c is migrated)
typedef struct {
    const devfs_device_ops_t *ops;
    void *ctx;
    int in_use;
} devfs_device_t;

typedef struct {
    int flags;
    int is_tree;
    const devfs_device_ops_t *ops;
    void *opened_ctx;
    union {
        devfs_node_t *node;      // tree device
        devfs_device_t *legacy;  // legacy flat device
    } u;
} devfs_handle_t;


// -------------------- Input devices --------------------

typedef struct {
    // byte stream of ASCII chars (like /dev/tty)
    char buf[512];
    volatile uint32_t r;
    volatile uint32_t w;
    volatile uint32_t count;
    int flags; /* open flags (O_NONBLOCK etc.) */
} devfs_kbd_stream_t;

typedef struct {
    // stream of Event structs
    Event buf[128];
    volatile uint32_t r;
    volatile uint32_t w;
    volatile uint32_t count;
    int flags; /* open flags (O_NONBLOCK etc.) */
} devfs_event_stream_t;

static devfs_kbd_stream_t g_kbd0;
static devfs_event_stream_t g_evt0;

static devfs_device_t g_devices[DEVFS_MAX_DEVICES];
static devfs_node_t *g_root = NULL;
static int g_inited = 0;

static devfs_node_t* devfs_new_node(devfs_node_type_t type, const char *name, devfs_node_t *parent) {
    devfs_node_t *n = (devfs_node_t*)kmalloc(sizeof(devfs_node_t));
    if (!n) return NULL;
    memset(n, 0, sizeof(*n));
    n->type = type;
    n->user_owned = 0;
    n->parent = parent;
    if (name) {
        strncpy(n->name, name, sizeof(n->name) - 1);
        n->name[sizeof(n->name) - 1] = 0;
    }
    return n;
}

static devfs_node_t* devfs_find_child(devfs_node_t *dir, const char *name) {
    if (!dir || dir->type != DEVFS_NODE_DIR || !name) return NULL;
    for (devfs_node_t *c = dir->children; c; c = c->next) {
        if (strcmp(c->name, name) == 0) return c;
    }
    return NULL;
}

static devfs_node_t* devfs_add_child(devfs_node_t *dir, devfs_node_t *child) {
    if (!dir || dir->type != DEVFS_NODE_DIR || !child) return NULL;
    child->next = dir->children;
    dir->children = child;
    child->parent = dir;
    return child;
}

static void devfs_free_owner(devfs_owner_t *owner) {
    if (!owner) return;
    if (owner->kind == DEVFS_OWNER_USER && owner->id) {
        kfree((void*)owner->id);
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

static const char *devfs_normalize_path(const char *path) {
    if (!path) return NULL;

    // Normalize accepted prefixes: allow $/dev, /dev, or raw user paths.
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
    // Path is relative to $/dev, case-sensitive.
    if (!path || !path[0]) return 0;
    // forbid any attempt to create vDrive* at any level
    if (strstr(path, "vDrive") != NULL) return 1;
    // forbid mnt (even though it shouldn't be under /dev)
    if (strncmp(path, "mnt", 3) == 0) return 1;
    return 0;
}

static void devfs_init_once(void) {
    if (g_inited) return;
    memset(g_devices, 0, sizeof(g_devices));
    g_root = devfs_new_node(DEVFS_NODE_DIR, "dev", NULL);
    g_inited = 1;
}

static const char* devfs_path_next(const char *p, char *seg, size_t seg_sz) {
    // Skip leading '/'
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

int devfs_mkdir_p(const char *path, devfs_owner_t owner) {
    (void)owner; // directories are shared (recommended policy)
    devfs_init_once();
    if (!g_root) return -1;
    if (!path || !path[0]) return 0;

    devfs_node_t *cur = g_root;
    const char *p = path;
    char seg[64];

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

        // advance past '/'
        while (*p == '/') p++;
    }

    return 0;
}

int devfs_register_path(const char *path, const devfs_device_ops_t *ops, void *ctx, devfs_owner_t owner) {
    devfs_init_once();
    if (!g_root) return -1;
    if (!path || !path[0]) return -2;
    if (!ops || !ops->name || !ops->name[0] || !ops->read) return -3;

    /* Normalize user paths: accept $/dev/... or /dev/... and strip prefix. */
    const char *path_in = devfs_normalize_path(path);
    if (!path_in || !path_in[0]) return -2;

    if (owner.kind == DEVFS_OWNER_SQRM) {
        if (devfs_is_reserved_for_sqrm(path_in)) return -4;
    }

    devfs_node_t *cur = g_root;
    const char *p = path_in;
    char seg[64];
    char last[64];
    last[0] = 0;

    while ((p = devfs_path_next(p, seg, sizeof(seg))) != NULL) {
        if (!seg[0]) break;
        strncpy(last, seg, sizeof(last) - 1);
        last[sizeof(last) - 1] = 0;

        // Determine if there are more segments after this
        const char *q = p;
        while (*q == '/') q++;
        int has_more = (*q != 0);

        if (!has_more) break; // last component

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

        // existing is device
        if (existing->owner.kind == DEVFS_OWNER_KERNEL) {
            return -9;
        }
        if (!existing->ops || !existing->ops->can_replace) {
            return -10;
        }
        devfs_replace_decision_t d = existing->ops->can_replace(existing->ctx, path, owner.id ? owner.id : "");
        if (d != DEVFS_REPLACE_ALLOW) return -11;

        if (existing->owner.kind == DEVFS_OWNER_USER || owner.kind == DEVFS_OWNER_USER) {
            const char *old_id = existing->owner.id ? existing->owner.id : "";
            const char *new_id = owner.id ? owner.id : "";
            if (strcmp(old_id, new_id) != 0) return -11;
        }

        // replace in-place
        devfs_free_user_ctx(existing);
        devfs_free_owner(&existing->owner);
        existing->ops = ops;
        existing->user_owned = (owner.kind == DEVFS_OWNER_USER);
        if (existing->user_owned) {
            existing->user_ctx = NULL;
        }
        existing->ctx = ctx;
        existing->owner = owner;
        return 0;
    }

    devfs_node_t *ndev = devfs_new_node(DEVFS_NODE_DEV, last, cur);
    if (!ndev) return -12;
    ndev->ops = ops;
    ndev->user_owned = (owner.kind == DEVFS_OWNER_USER);
    if (ndev->user_owned) {
        ndev->user_ctx = NULL;
    }
    ndev->ctx = ctx;
    ndev->owner = owner;
    devfs_add_child(cur, ndev);

    return 0;
}

int devfs_register(const devfs_device_ops_t *ops, void *ctx) {
    // Legacy flat registration at root
    devfs_owner_t owner = { .kind = DEVFS_OWNER_KERNEL, .id = "kernel" };
    int r = devfs_register_path(ops->name, ops, ctx, owner);
    if (r == 0) {
        // Keep legacy list for root devices
        for (int i = 0; i < DEVFS_MAX_DEVICES; i++) {
            if (!g_devices[i].in_use) {
                g_devices[i].ops = ops;
                g_devices[i].ctx = ctx;
                g_devices[i].in_use = 1;
                break;
            }
        }
    }
    return r;
}

static devfs_device_t* devfs_find(const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < DEVFS_MAX_DEVICES; i++) {
        if (g_devices[i].in_use && g_devices[i].ops && strcmp(g_devices[i].ops->name, name) == 0) {
            return &g_devices[i];
        }
    }
    return NULL;
}

static devfs_node_t* devfs_find_path_node(const char *path) {
    devfs_init_once();
    if (!g_root || !path) return NULL;

    devfs_node_t *cur = g_root;
    const char *p = path;
    char seg[64];

    while ((p = devfs_path_next(p, seg, sizeof(seg))) != NULL) {
        if (!seg[0]) break;
        devfs_node_t *c = devfs_find_child(cur, seg);
        if (!c) return NULL;
        cur = c;
        while (*p == '/') p++;
    }
    return cur;
}

void* devfs_open(const char *name, int flags) {
    // legacy: root-only open
    devfs_init_once();
    devfs_device_t *d = devfs_find(name);
    if (!d) return NULL;

    /* Apply open flags to device context for devices that care (kbd0/event0). */
    if (d->ops && d->ops->name) {
        if (strcmp(d->ops->name, "kbd0") == 0) {
            ((devfs_kbd_stream_t*)d->ctx)->flags = flags;
        } else if (strcmp(d->ops->name, "event0") == 0) {
            ((devfs_event_stream_t*)d->ctx)->flags = flags;
        }
    }

    devfs_handle_t *h = (devfs_handle_t*)kmalloc(sizeof(devfs_handle_t));
    if (!h) return NULL;
    h->flags = flags;
    h->is_tree = 0;
    h->ops = d->ops;
    h->opened_ctx = (h->ops && h->ops->open) ? h->ops->open(d->ctx, flags) : d->ctx;
    h->u.legacy = d;
    return h;
}

void* devfs_open_path(const char *path, int flags) {
    devfs_init_once();
    devfs_node_t *n = devfs_find_path_node(path);
    if (!n || n->type != DEVFS_NODE_DEV || !n->ops) return NULL;

    /* Apply open flags to device context for devices that care (kbd0/event0). */
    if (n->ops && n->ops->name && n->ctx) {
        if (strcmp(n->ops->name, "kbd0") == 0) {
            ((devfs_kbd_stream_t*)n->ctx)->flags = flags;
        } else if (strcmp(n->ops->name, "event0") == 0) {
            ((devfs_event_stream_t*)n->ctx)->flags = flags;
        }
    }

    devfs_handle_t *h = (devfs_handle_t*)kmalloc(sizeof(devfs_handle_t));
    if (!h) return NULL;
    h->flags = flags;
    h->is_tree = 1;
    h->ops = n->ops;
    h->opened_ctx = (h->ops && h->ops->open) ? h->ops->open(n->ctx, flags) : n->ctx;
    h->u.node = n;
    return h;
}

int devfs_list_dir_next(const char *dir_path, int *cookie, char *name_buf, size_t buf_size, int *is_dir) {
    devfs_init_once();
    if (!cookie || !name_buf || buf_size == 0) return -1;

    devfs_node_t *dir = dir_path && dir_path[0] ? devfs_find_path_node(dir_path) : g_root;
    if (!dir) return -2;
    if (dir->type != DEVFS_NODE_DIR) return -3;

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

ssize_t devfs_read(void *handle, void *buf, size_t count) {
    devfs_handle_t *h = (devfs_handle_t*)handle;
    if (!h) return -1;

    if (h->is_tree) {
        devfs_node_t *n = h->u.node;
        if (!n || n->type != DEVFS_NODE_DEV || !n->ops || !n->ops->read) return -1;
        return h->ops->read(h->opened_ctx, buf, count);
    }

    devfs_device_t *d = h->u.legacy;
    if (!d || !d->ops || !d->ops->read) return -1;
    return h->ops->read(h->opened_ctx, buf, count);
}

ssize_t devfs_write(void *handle, const void *buf, size_t count) {
    devfs_handle_t *h = (devfs_handle_t*)handle;
    if (!h) return -1;

    if (h->is_tree) {
        devfs_node_t *n = h->u.node;
        if (!n || n->type != DEVFS_NODE_DEV || !n->ops || !n->ops->write) return -1;
        return h->ops->write(h->opened_ctx, buf, count);
    }

    devfs_device_t *d = h->u.legacy;
    if (!d || !d->ops || !d->ops->write) return -1;
    return h->ops->write(h->opened_ctx, buf, count);
}

int devfs_close(void *handle) {
    devfs_handle_t *h = (devfs_handle_t*)handle;
    if (!h) return -1;

    if (h->is_tree) {
        (void)h->u.node;
        if (h->ops && h->ops->close) {
            h->ops->close(h->opened_ctx);
        }
    } else {
        (void)h->u.legacy;
        if (h->ops && h->ops->close) {
            h->ops->close(h->opened_ctx);
        }
    }

    kfree(h);
    return 0;
}

int devfs_list_next(int *cookie, char *name_buf, size_t buf_size) {
    devfs_init_once();
    if (!cookie || !name_buf || buf_size == 0) return -1;

    int idx = *cookie;
    if (idx < 0) idx = 0;

    for (; idx < DEVFS_MAX_DEVICES; idx++) {
        if (g_devices[idx].in_use && g_devices[idx].ops && g_devices[idx].ops->name) {
            strncpy(name_buf, g_devices[idx].ops->name, buf_size - 1);
            name_buf[buf_size - 1] = 0;
            *cookie = idx + 1;
            return 1;
        }
    }

    return 0;
}

static void kbd_stream_push(devfs_kbd_stream_t *s, char c) {
    if (!s) return;

    uint64_t f = irq_save();
    if (s->count >= (uint32_t)(sizeof(s->buf) / sizeof(s->buf[0]))) {
        irq_restore(f);
        return;
    }
    s->buf[s->w] = c;
    s->w = (s->w + 1) % (uint32_t)(sizeof(s->buf) / sizeof(s->buf[0]));
    s->count++;
    irq_restore(f);
}

static int kbd_stream_pop(devfs_kbd_stream_t *s, char *out) {
    if (!s || !out) return 0;

    uint64_t f = irq_save();
    if (s->count == 0) {
        irq_restore(f);
        return 0;
    }
    *out = s->buf[s->r];
    s->r = (s->r + 1) % (uint32_t)(sizeof(s->buf) / sizeof(s->buf[0]));
    s->count--;
    irq_restore(f);
    return 1;
}

static void evt_stream_push(devfs_event_stream_t *s, const Event *e) {
    if (!s || !e) return;

    uint64_t f = irq_save();
    if (s->count >= (uint32_t)(sizeof(s->buf) / sizeof(s->buf[0]))) {
        irq_restore(f);
        return;
    }
    s->buf[s->w] = *e;
    s->w = (s->w + 1) % (uint32_t)(sizeof(s->buf) / sizeof(s->buf[0]));
    s->count++;
    irq_restore(f);
}

static int evt_stream_pop(devfs_event_stream_t *s, Event *out) {
    if (!s || !out) return 0;

    uint64_t f = irq_save();
    if (s->count == 0) {
        irq_restore(f);
        return 0;
    }
    *out = s->buf[s->r];
    s->r = (s->r + 1) % (uint32_t)(sizeof(s->buf) / sizeof(s->buf[0]));
    s->count--;
    irq_restore(f);
    return 1;
}

static ssize_t dev_kbd_read(void *ctx, void *buf, size_t count) {
    devfs_kbd_stream_t *s = (devfs_kbd_stream_t*)ctx;
    if (!s || !buf) return -1;

    /* Non-blocking mode: if opened with O_NONBLOCK and no data, return 0. */
    if ((s->flags & O_NONBLOCK) && s->count == 0) {
        return 0;
    }

    char *out = (char*)buf;
    size_t n = 0;

    // blocking semantics: wait for at least 1 byte
    // Note: syscalls enter with IF=0 (interrupt gate), so a pure spin would deadlock.
    // Wait without changing the caller's IF state.
    while (s->count == 0) {
        hlt_wait_preserve_if();
    }

    while (n < count) {
        char c;
        if (!kbd_stream_pop(s, &c)) break;
        out[n++] = c;
        // stop early on newline to emulate canonical line read if caller wants
        if (c == '\n') break;
    }

    return (ssize_t)n;
}

static ssize_t dev_evt_read(void *ctx, void *buf, size_t count) {
    devfs_event_stream_t *s = (devfs_event_stream_t*)ctx;
    if (!s || !buf) return -1;

    /* Non-blocking mode: if opened with O_NONBLOCK and no data, return 0. */
    if ((s->flags & O_NONBLOCK) && s->count == 0) {
        return 0;
    }

    // only whole events
    if (count < sizeof(Event)) return -2;

    // blocking wait
    // Note: syscalls enter with IF=0 (interrupt gate), so a pure spin would deadlock.
    // Allow interrupts while waiting.
    while (s->count == 0) {
        hlt_wait_preserve_if();
    }

    Event e;
    if (!evt_stream_pop(s, &e)) return 0;
    memcpy(buf, &e, sizeof(Event));
    return (ssize_t)sizeof(Event);
}

static const devfs_device_ops_t g_dev_kbd0_ops = {
    .name = "kbd0",
    .read = dev_kbd_read,
    .write = NULL,
    .close = NULL,
};

static const devfs_device_ops_t g_dev_evt0_ops = {
    .name = "event0",
    .read = dev_evt_read,
    .write = NULL,
    .close = NULL,
};

int devfs_input_init(void) {
    devfs_init_once();
    memset(&g_kbd0, 0, sizeof(g_kbd0));
    memset(&g_evt0, 0, sizeof(g_evt0));
    /* default: blocking */
    g_kbd0.flags = 0;
    g_evt0.flags = 0;
    devfs_owner_t owner = { .kind = DEVFS_OWNER_KERNEL, .id = "kernel" };

    // Ensure directories exist
    devfs_mkdir_p("input", owner);

    int r1 = devfs_register_path("input/kbd0", &g_dev_kbd0_ops, &g_kbd0, owner);
    int r2 = devfs_register_path("input/event0", &g_dev_evt0_ops, &g_evt0, owner);
    if (r1 != 0) return r1;
    if (r2 != 0) return r2;
    com_write_string(COM1_PORT, "[DEVFS] Registered input devices: $/dev/input/kbd0, $/dev/input/event0\n");
    return 0;
}

// -------------------- Graphics devices --------------------

// VIDEO0 v2 per-open state (write->read responses + buffer handles)
#define VIDEO0_MAX_BUFS 16

typedef struct {
    uint32_t handle;
    uint32_t fmt;
    uint32_t pitch;
    uint32_t size_bytes;
    uint64_t phys_base;
    uint64_t user_addr; // 0 if not mapped yet
    uint8_t in_use;
} video0_buf_t;

typedef struct {
    uint8_t resp[512];
    uint32_t resp_len;
    uint32_t resp_off;

    // dirty rect accumulated since last FLUSH
    uint32_t dirty_x0, dirty_y0, dirty_x1, dirty_y1;
    int dirty_valid;

    video0_buf_t bufs[VIDEO0_MAX_BUFS];
    uint32_t next_handle;
} video0_open_ctx_t;

static uint64_t g_video0_next_user_va = 0x0000005000000000ULL;

/* Stage 3: Mapped command buffer (zero-copy submission) */
static void *g_video0_cmdbuf = NULL;      // Kernel-side command buffer
static uint64_t g_video0_cmdbuf_user = 0; // User-side mapped address
static uint32_t g_video0_cmdbuf_size = 0; // Buffer size in bytes

static uint64_t video0_alloc_user_va(uint64_t size_bytes) {
    uint64_t sz = (size_bytes + 0xFFFULL) & ~0xFFFULL;
    uint64_t base = g_video0_next_user_va;
    g_video0_next_user_va = base + sz + 0x1000ULL;
    return base;
}

static video0_buf_t* video0_find_buf(video0_open_ctx_t *c, uint32_t handle) {
    if (!c || handle == 0) return NULL;
    for (uint32_t i = 0; i < VIDEO0_MAX_BUFS; i++) {
        if (c->bufs[i].in_use && c->bufs[i].handle == handle) return &c->bufs[i];
    }
    return NULL;
}

static video0_buf_t* video0_alloc_buf_slot(video0_open_ctx_t *c) {
    if (!c) return NULL;
    for (uint32_t i = 0; i < VIDEO0_MAX_BUFS; i++) {
        if (!c->bufs[i].in_use) return &c->bufs[i];
    }
    return NULL;
}

static void* dev_video0_open(void *ctx, int flags) {
    (void)ctx; (void)flags;
    video0_open_ctx_t *c = (video0_open_ctx_t*)kzalloc(sizeof(video0_open_ctx_t));
    if (!c) return NULL;
    c->next_handle = 1;
    return c;
}

static int dev_video0_close(void *ctx) {
    video0_open_ctx_t *c = (video0_open_ctx_t*)ctx;
    if (!c) return 0;
    // NOTE: buffer phys memory currently leaked (no contiguous free API).
    kfree(c);
    return 0;
}


typedef struct __attribute__((packed)) {
    uint64_t fb_addr;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint8_t  bpp;
    uint8_t  mode;
    uint8_t  fmt;
    uint8_t  reserved;
} devfs_video_info_t;

static uint64_t g_user_fb_addr = 0;

static uint64_t dev_video0_get_user_fb_addr(const framebuffer_t *fb) {
    /*
     * Map framebuffer MMIO/VRAM into user space so ring3 apps can draw.
     * This kernel currently uses one global address space, so a single mapping works.
     *
     * NOTE: This is effectively a shared global mapping; any user process can write to it.
     * For now this matches the existing model (single-user, trusted apps).
     */
    if (!fb || fb->phys_addr == 0 || fb->size_bytes == 0) return 0;

    if (g_user_fb_addr) return g_user_fb_addr;

    /* Pick a fixed low canonical address far away from program text/stack. */
    const uint64_t user_fb_base = 0x0000004000000000ULL;

    if (paging_map_range(user_fb_base, fb->phys_addr, fb->size_bytes, PFLAG_PRESENT | PFLAG_WRITABLE | PFLAG_USER) != 0) {
        return 0;
    }

    g_user_fb_addr = user_fb_base;
    return g_user_fb_addr;
}

static ssize_t dev_video0_read(void *ctx, void *buf, size_t count) {
    (void)ctx;
    video0_open_ctx_t *c = (video0_open_ctx_t*)ctx;
    if (c && c->resp_len > 0) {
        uint32_t remain = c->resp_len - c->resp_off;
        uint32_t n = (remain < count) ? remain : (uint32_t)count;
        memcpy(buf, c->resp + c->resp_off, n);
        c->resp_off += n;
        if (c->resp_off >= c->resp_len) { c->resp_len = 0; c->resp_off = 0; }
        return (ssize_t)n;
    }
    if (!buf) return -1;
    if (count < sizeof(devfs_video_info_t)) return -2;

    devfs_video_info_t info;
    memset(&info, 0, sizeof(info));

    framebuffer_mode_t m = VGA_GetFrameBufferMode();
    info.mode = (uint8_t)m;

    framebuffer_t fb;
    if (VGA_GetFrameBuffer(&fb) == 0) {
        /* Return a user-mapped framebuffer address, not the kernel ioremap address. */
        info.fb_addr = dev_video0_get_user_fb_addr(&fb);
        info.width = fb.width;
        info.height = fb.height;
        info.pitch = fb.pitch;
        info.bpp = fb.bpp;

        /* Map internal framebuffer format to MD64API GRP format (stable ABI) */
        if (fb.bpp == 32) info.fmt = (uint8_t)MD64API_GRP_FMT_XRGB8888;
        else if (fb.bpp == 16) info.fmt = (uint8_t)MD64API_GRP_FMT_RGB565;
        else info.fmt = (uint8_t)MD64API_GRP_FMT_UNKNOWN;
    }

    memcpy(buf, &info, sizeof(info));
    return (ssize_t)sizeof(info);
}

static const devfs_device_ops_t g_dev_video0_ops = {
    .name = "video0",
    .open = dev_video0_open,
    .read = dev_video0_read,
    .write = dev_video0_write,
    .close = dev_video0_close,
};

int devfs_graphics_init(void) {
    devfs_owner_t owner = { .kind = DEVFS_OWNER_KERNEL, .id = "kernel" };
    devfs_init_once();
    devfs_mkdir_p("graphics", owner);
    int r = devfs_register_path("graphics/video0", &g_dev_video0_ops, NULL, owner);
    if (r == 0) {
        com_write_string(COM1_PORT, "[DEVFS] Registered graphics devices: $/dev/graphics/video0\n");
    }
    return r;
}

void devfs_input_push_event(const Event *e) {
    if (!e) return;

    // publish structured event
    evt_stream_push(&g_evt0, e);

    // publish ASCII/ANSI into kbd stream (very linux-like: $/dev/input/event* vs $/dev/tty)
    if (e->type == EVENT_KEY_PRESSED) {
        char c = e->data.keyboard.ascii;

        /*
         * Special keys: emit VT100/ANSI escape sequences on kbd0 so shells can implement
         * Unix-like line editing/history by parsing stdin.
         *
         * Common sequences:
         *   Up    : ESC [ A
         *   Down  : ESC [ B
         *   Right : ESC [ C
         *   Left  : ESC [ D
         *   Home  : ESC [ H
         *   End   : ESC [ F
         *   Insert: ESC [ 2 ~
         *   Delete: ESC [ 3 ~
         *   PgUp  : ESC [ 5 ~
         *   PgDn  : ESC [ 6 ~
         */
        const char *seq = NULL;
        switch (e->data.keyboard.keycode) {
            case KEY_ARROW_UP:    seq = "\x1b[A"; break;
            case KEY_ARROW_DOWN:  seq = "\x1b[B"; break;
            case KEY_ARROW_RIGHT: seq = "\x1b[C"; break;
            case KEY_ARROW_LEFT:  seq = "\x1b[D"; break;
            case KEY_HOME:        seq = "\x1b[H"; break;
            case KEY_END:         seq = "\x1b[F"; break;
            case KEY_INSERT:      seq = "\x1b[2~"; break;
            case KEY_DELETE:      seq = "\x1b[3~"; break;
            case KEY_PAGE_UP:     seq = "\x1b[5~"; break;
            case KEY_PAGE_DOWN:   seq = "\x1b[6~"; break;
            default: break;
        }
        if (seq) {
            for (const char *p = seq; *p; ++p) {
                kbd_stream_push(&g_kbd0, *p);
            }
            return;
        }

        if (e->data.keyboard.keycode == KEY_ENTER) {
            if (c == 0) c = '\n';
        }
        if (e->data.keyboard.keycode == KEY_BACKSPACE) {
            /* Provide a usable ASCII backspace for line editing */
            if (c == 0) c = '\b';
        }

        if (c) {
            kbd_stream_push(&g_kbd0, c);
        }
    }
}


static inline void video0_dirty_union(video0_open_ctx_t *c, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (!c || w == 0 || h == 0) return;
    uint32_t x1 = x + w;
    uint32_t y1 = y + h;
    if (!c->dirty_valid) {
        c->dirty_x0 = x; c->dirty_y0 = y; c->dirty_x1 = x1; c->dirty_y1 = y1;
        c->dirty_valid = 1;
        return;
    }
    if (x < c->dirty_x0) c->dirty_x0 = x;
    if (y < c->dirty_y0) c->dirty_y0 = y;
    if (x1 > c->dirty_x1) c->dirty_x1 = x1;
    if (y1 > c->dirty_y1) c->dirty_y1 = y1;
}

static inline void video0_resp_set(video0_open_ctx_t *c, const void *data, uint32_t len) {
    if (!c) return;
    if (len > sizeof(c->resp)) len = (uint32_t)sizeof(c->resp);
    memcpy(c->resp, data, len);
    c->resp_len = len;
    c->resp_off = 0;
}

static ssize_t dev_video0_write(void *ctx, const void *buf, size_t count) {
    video0_open_ctx_t *c = (video0_open_ctx_t*)ctx;
    if (!c || !buf || count < sizeof(videoctl2_hdr_t)) return -1;

    framebuffer_t fb;
    memset(&fb, 0, sizeof(fb));
    VGA_GetFrameBuffer(&fb);
    
    // Process ALL commands in the buffer (batched writes)
    size_t offset = 0;
    size_t total_processed = 0;
    
    while (offset + sizeof(videoctl2_hdr_t) <= count) {
        const videoctl2_hdr_t *h = (const videoctl2_hdr_t*)((const uint8_t*)buf + offset);
        
        if (h->magic != VIDEOCTL_MAGIC2) break;
        if (h->abi_version != VIDEOCTL_ABI_VERSION) break;
        if (h->size_bytes == 0 || offset + h->size_bytes > count) break;

    switch (h->cmd) {
        case VIDEOCTL_CMD2_GET_INFO: {
            // Commands with responses can't be batched - must be sent alone
            if (offset > 0) {
                // Already processed some commands, stop here and return
                return (ssize_t)total_processed;
            }
            
            videoctl2_info_t resp;
            memset(&resp, 0, sizeof(resp));
            resp.hdr = *h;
            resp.hdr.size_bytes = sizeof(resp);

            if (fb.addr) {
                resp.width = fb.width;
                resp.height = fb.height;
                resp.pitch = fb.pitch;
                resp.bpp = fb.bpp;
                if (fb.bpp == 32) resp.fmt = MD64API_GRP_FMT_XRGB8888;
                else if (fb.bpp == 16) resp.fmt = MD64API_GRP_FMT_RGB565;
                else resp.fmt = MD64API_GRP_FMT_UNKNOWN;
            }

            resp.caps = VIDEOCTL2_CAP_ENQUEUE_FILL_RECT |
                        VIDEOCTL2_CAP_ENQUEUE_BLIT |
                        VIDEOCTL2_CAP_ENQUEUE_BLIT_BUF |
                        VIDEOCTL2_CAP_FLUSH |
                        VIDEOCTL2_CAP_BUF_HANDLES |
                        VIDEOCTL2_CAP_BUF_SG_PAGES;
            strncpy(resp.driver, "kernel_sw", sizeof(resp.driver)-1);

            video0_resp_set(c, &resp, sizeof(resp));
            return (ssize_t)h->size_bytes; // Return immediately for response commands
        }

        case VIDEOCTL_CMD2_ALLOC_BUF: {
            // Commands with responses can't be batched
            if (offset > 0) return (ssize_t)total_processed;
            if (h->size_bytes < sizeof(videoctl2_alloc_buf_t)) return -1;
            videoctl2_alloc_buf_t resp;
            memcpy(&resp, buf, sizeof(resp));

            if (resp.size_bytes == 0 || resp.size_bytes > (64u * 1024u * 1024u)) {
                resp.handle = 0; resp.pitch = 0;
                video0_resp_set(c, &resp, sizeof(resp));
                break;
            }

            video0_buf_t *slot = video0_alloc_buf_slot(c);
            if (!slot) {
                resp.handle = 0; resp.pitch = 0;
                video0_resp_set(c, &resp, sizeof(resp));
                break;
            }

            uint64_t pages = (((uint64_t)resp.size_bytes + 0xFFFULL) >> 12);
            uint64_t phys = phys_alloc_contiguous((size_t)pages);
            if (!phys) {
                resp.handle = 0; resp.pitch = 0;
                video0_resp_set(c, &resp, sizeof(resp));
                break;
            }
            void *kptr = phys_to_virt_kernel(phys);
            if (kptr) memset(kptr, 0, pages * 0x1000ULL);

            uint32_t handle = c->next_handle++;
            slot->in_use = 1;
            slot->handle = handle;
            slot->fmt = resp.fmt;
            slot->size_bytes = resp.size_bytes;
            slot->phys_base = phys;
            slot->user_addr = 0;

            /*
             * Derive pitch from the requested format and the framebuffer width.
             * If the buffer size is an exact multiple of fb.height we can infer
             * the stride; otherwise derive it from bytes-per-pixel × width.
             */
            uint32_t bpp_bytes = (resp.fmt == MD64API_GRP_FMT_RGB565) ? 2u : 4u;
            uint32_t pitch = fb.addr ? fb.width * bpp_bytes : 0u;
            /* If the allocation matches the screen exactly, use the real fb pitch
             * (which may be wider due to hardware alignment). */
            if (fb.addr && fb.height > 0 && resp.size_bytes == fb.pitch * fb.height)
                pitch = fb.pitch;
            slot->pitch = pitch;

            resp.handle = handle;
            resp.pitch = pitch;
            video0_resp_set(c, &resp, sizeof(resp));
            return (ssize_t)h->size_bytes; // Return immediately for response commands
        }

        case VIDEOCTL_CMD2_MAP_BUF: {
            // Commands with responses can't be batched
            if (offset > 0) return (ssize_t)total_processed;
            if (h->size_bytes < sizeof(videoctl2_map_buf_t)) return -1;
            videoctl2_map_buf_t resp;
            memcpy(&resp, buf, sizeof(resp));

            video0_buf_t *b = video0_find_buf(c, resp.handle);
            if (!b) {
                resp.user_addr = 0; resp.size_bytes = 0; resp.pitch = 0; resp.fmt = 0;
                video0_resp_set(c, &resp, sizeof(resp));
                break;
            }

            if (!b->user_addr) {
                uint64_t ua = video0_alloc_user_va(b->size_bytes);
                if (paging_map_range(ua, b->phys_base, b->size_bytes, PFLAG_PRESENT | PFLAG_WRITABLE | PFLAG_USER) != 0) {
                    resp.user_addr = 0; resp.size_bytes = 0; resp.pitch = 0; resp.fmt = 0;
                    video0_resp_set(c, &resp, sizeof(resp));
                    break;
                }
                b->user_addr = ua;
            }

            resp.user_addr = b->user_addr;
            resp.size_bytes = b->size_bytes;
            resp.pitch = b->pitch;
            resp.fmt = b->fmt;
            video0_resp_set(c, &resp, sizeof(resp));
            return (ssize_t)h->size_bytes; // Return immediately for response commands
        }

        case VIDEOCTL_CMD2_ENQUEUE: {
            if (h->size_bytes < sizeof(videoctl2_enqueue_t)) return -1;
            const videoctl2_enqueue_t *req = (const videoctl2_enqueue_t*)buf;
            if (!fb.addr) return -1;
            if (!(fb.bpp == 32 || fb.bpp == 16)) return -1;

            uint8_t *dst = (uint8_t*)fb.addr;

            if (req->u.fill.op == VIDEOCTL2_OP_FILL_RECT) {
                uint32_t x=req->u.fill.x, y=req->u.fill.y, w=req->u.fill.w, hh=req->u.fill.h;
                if (x>=fb.width || y>=fb.height) break;
                if (x+w>fb.width) w = fb.width - x;
                if (y+hh>fb.height) hh = fb.height - y;
                if (fb.bpp == 32) {
                    uint32_t color = req->u.fill.argb;
                    for (uint32_t yy=0; yy<hh; yy++) {
                        uint32_t *row = (uint32_t*)(dst + (y+yy)*fb.pitch + x*4);
                        for (uint32_t xx=0; xx<w; xx++) row[xx] = color;
                    }
                } else {
                    uint32_t c32 = req->u.fill.argb;
                    uint16_t c565 = (uint16_t)(((c32>>19)&0x1F)<<11 | ((c32>>10)&0x3F)<<5 | ((c32>>3)&0x1F));
                    for (uint32_t yy=0; yy<hh; yy++) {
                        uint16_t *row = (uint16_t*)(dst + (y+yy)*fb.pitch + x*2);
                        for (uint32_t xx=0; xx<w; xx++) row[xx] = c565;
                    }
                }
                video0_dirty_union(c, x, y, w, hh);
                break;
            }

            if (req->u.blit.op == VIDEOCTL2_OP_BLIT) {
                uint32_t sx=req->u.blit.src_x, sy=req->u.blit.src_y;
                uint32_t dx=req->u.blit.dst_x, dy=req->u.blit.dst_y;
                uint32_t w=req->u.blit.w, hh=req->u.blit.h;
                if (sx>=fb.width || sy>=fb.height || dx>=fb.width || dy>=fb.height) break;
                if (sx+w>fb.width) w = fb.width - sx;
                if (dx+w>fb.width) w = fb.width - dx;
                if (sy+hh>fb.height) hh = fb.height - sy;
                if (dy+hh>fb.height) hh = fb.height - dy;
                uint32_t bpp = (fb.bpp==32)?4:2;
                for (uint32_t yy=0; yy<hh; yy++) {
                    void *srcp = dst + (sy+yy)*fb.pitch + sx*bpp;
                    void *dstp = dst + (dy+yy)*fb.pitch + dx*bpp;
                    memmove(dstp, srcp, w*bpp);
                }
                video0_dirty_union(c, dx, dy, w, hh);
                break;
            }

            if (req->u.blit_buf.op == VIDEOCTL2_OP_BLIT_BUF) {
                video0_buf_t *b = video0_find_buf(c, req->u.blit_buf.handle);
                if (!b) break;
                uint8_t *srcbuf = (uint8_t*)phys_to_virt_kernel(b->phys_base);
                if (!srcbuf) break;

                uint32_t sx=req->u.blit_buf.src_x, sy=req->u.blit_buf.src_y;
                uint32_t dx=req->u.blit_buf.dst_x, dy=req->u.blit_buf.dst_y;
                uint32_t w=req->u.blit_buf.w, hh=req->u.blit_buf.h;
                uint32_t sp=req->u.blit_buf.src_pitch;
                uint32_t sf=req->u.blit_buf.src_fmt;
                if (dx>=fb.width || dy>=fb.height) break;
                if (dx+w>fb.width) w = fb.width - dx;
                if (dy+hh>fb.height) hh = fb.height - dy;

                if (fb.bpp == 32 && sf == MD64API_GRP_FMT_XRGB8888) {
                    for (uint32_t yy = 0; yy < hh; yy++) {
                        uint32_t *srcrow = (uint32_t*)(srcbuf + (sy+yy)*sp + sx*4);
                        uint32_t *dstrow = (uint32_t*)(dst + (dy+yy)*fb.pitch + dx*4);
                        memcpy(dstrow, srcrow, w * 4);
                    }
                } else if (fb.bpp == 16 && sf == MD64API_GRP_FMT_XRGB8888) {
                    /* Convert XRGB8888 source to RGB565 framebuffer. */
                    for (uint32_t yy = 0; yy < hh; yy++) {
                        uint32_t *srcrow = (uint32_t*)(srcbuf + (sy+yy)*sp + sx*4);
                        uint16_t *dstrow = (uint16_t*)(dst + (dy+yy)*fb.pitch + dx*2);
                        for (uint32_t xx = 0; xx < w; xx++) {
                            uint32_t p = srcrow[xx];
                            dstrow[xx] = (uint16_t)(((p>>19)&0x1F)<<11 |
                                                    ((p>>10)&0x3F)<<5  |
                                                    ((p>>3) &0x1F));
                        }
                    }
                } else if (fb.bpp == 16 && sf == MD64API_GRP_FMT_RGB565) {
                    for (uint32_t yy = 0; yy < hh; yy++) {
                        uint16_t *srcrow = (uint16_t*)(srcbuf + (sy+yy)*sp + sx*2);
                        uint16_t *dstrow = (uint16_t*)(dst + (dy+yy)*fb.pitch + dx*2);
                        memcpy(dstrow, srcrow, w * 2);
                    }
                } else if (fb.bpp == 32 && sf == MD64API_GRP_FMT_RGB565) {
                    /* Upconvert RGB565 source to XRGB8888 framebuffer. */
                    for (uint32_t yy = 0; yy < hh; yy++) {
                        uint16_t *srcrow = (uint16_t*)(srcbuf + (sy+yy)*sp + sx*2);
                        uint32_t *dstrow = (uint32_t*)(dst + (dy+yy)*fb.pitch + dx*4);
                        for (uint32_t xx = 0; xx < w; xx++) {
                            uint16_t p = srcrow[xx];
                            uint32_t r = (p >> 11) & 0x1F; r = (r << 3) | (r >> 2);
                            uint32_t g = (p >>  5) & 0x3F; g = (g << 2) | (g >> 4);
                            uint32_t b = (p >>  0) & 0x1F; b = (b << 3) | (b >> 2);
                            dstrow[xx] = (r << 16) | (g << 8) | b;
                        }
                    }
                }
                video0_dirty_union(c, dx, dy, w, hh);
                break;
            }

            break;
        }

        case VIDEOCTL_CMD2_FLUSH: {
            if (!fb.addr) return -1;
            uint32_t x=0,y=0,w=0,hh=0;
            if (h->size_bytes >= sizeof(videoctl2_flush_t)) {
                const videoctl2_flush_t *req = (const videoctl2_flush_t*)buf;
                x=req->x; y=req->y; w=req->w; hh=req->h;
            }
            if (w==0 || hh==0) {
                if (c->dirty_valid) {
                    x=c->dirty_x0; y=c->dirty_y0; w=c->dirty_x1 - c->dirty_x0; hh=c->dirty_y1 - c->dirty_y0;
                }
            }
            if (w && hh) VGA_FlushRect(x,y,w,hh);
            c->dirty_valid = 0;
            break;
        }

        case VIDEOCTL_CMD2_CURSOR_SET:
        case VIDEOCTL_CMD2_CURSOR_MOVE:
        case VIDEOCTL_CMD2_CURSOR_SHOW:
            break;

        case VIDEOCTL_CMD2_MAP_CMDBUF:
        case VIDEOCTL_CMD2_SUBMIT_CMDBUF:
            break;

        default:
            // Unknown command, stop processing
            if (total_processed == 0) return -1;
            return (ssize_t)total_processed;
    }
    
    // Move to next command
    offset += h->size_bytes;
    total_processed += h->size_bytes;
    }
    
    // Return total bytes processed (all commands in the batch)
    return (ssize_t)(total_processed > 0 ? total_processed : count);
}


// ------------------------------------------------------------
// GUI IPC stubs
// ------------------------------------------------------------
// Some kernel components expect these symbols even if the GUI device
// is not built in this devfs implementation.

int devfs_gui_init(void) {
    // No GUI device/server available.
    return 0;
}

uint32_t devfs_gui_server_pid(void) {
    // 0 means: no GUI server claimed.
    return 0;
}








