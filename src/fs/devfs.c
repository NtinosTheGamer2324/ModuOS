#include "moduos/fs/devfs.h"
#include "moduos/kernel/md64api_grp.h"
#include "moduos/kernel/memory/memory.h"
#include "moduos/kernel/memory/paging.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/COM/com.h"
#include "moduos/drivers/graphics/VGA.h"
#include "moduos/drivers/graphics/framebuffer.h"
#include "moduos/kernel/interrupts/irq_lock.h"
#include "moduos/kernel/interrupts/hlt_wait.h"

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

    if (owner.kind == DEVFS_OWNER_SQRM) {
        if (devfs_is_reserved_for_sqrm(path)) return -4;
    }

    devfs_node_t *cur = g_root;
    const char *p = path;
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

        // replace in-place
        existing->ops = ops;
        existing->ctx = ctx;
        existing->owner = owner;
        return 0;
    }

    devfs_node_t *ndev = devfs_new_node(DEVFS_NODE_DEV, last, cur);
    if (!ndev) return -12;
    ndev->ops = ops;
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
        return n->ops->read(n->ctx, buf, count);
    }

    devfs_device_t *d = h->u.legacy;
    if (!d || !d->ops || !d->ops->read) return -1;
    return d->ops->read(d->ctx, buf, count);
}

ssize_t devfs_write(void *handle, const void *buf, size_t count) {
    devfs_handle_t *h = (devfs_handle_t*)handle;
    if (!h) return -1;

    if (h->is_tree) {
        devfs_node_t *n = h->u.node;
        if (!n || n->type != DEVFS_NODE_DEV || !n->ops || !n->ops->write) return -1;
        return n->ops->write(n->ctx, buf, count);
    }

    devfs_device_t *d = h->u.legacy;
    if (!d || !d->ops || !d->ops->write) return -1;
    return d->ops->write(d->ctx, buf, count);
}

int devfs_close(void *handle) {
    devfs_handle_t *h = (devfs_handle_t*)handle;
    if (!h) return -1;

    if (h->is_tree) {
        devfs_node_t *n = h->u.node;
        if (n && n->ops && n->ops->close) {
            n->ops->close(n->ctx);
        }
    } else {
        devfs_device_t *d = h->u.legacy;
        if (d && d->ops && d->ops->close) {
            d->ops->close(d->ctx);
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
    .read = dev_video0_read,
    .write = NULL,
    .close = NULL,
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

        /*
         * Avoid double-newline: many keymaps already set ascii='\n' for Enter.
         * If we also inject '\n' on KEY_ENTER, userland line reads will see an
         * extra empty line ("skips password" symptom).
         */
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
