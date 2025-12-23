#include "moduos/fs/devfs.h"
#include "moduos/kernel/md64api_grp.h"
#include "moduos/kernel/memory/memory.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/COM/com.h"
#include "moduos/drivers/graphics/VGA.h"
#include "moduos/drivers/graphics/framebuffer.h"
#include "moduos/kernel/interrupts/irq_lock.h"
#include "moduos/kernel/interrupts/hlt_wait.h"

#define DEVFS_MAX_DEVICES 32

typedef struct {
    const devfs_device_ops_t *ops;
    void *ctx;
    int in_use;
} devfs_device_t;

typedef struct {
    devfs_device_t *dev;
    int flags;
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
static int g_inited = 0;

static void devfs_init_once(void) {
    if (g_inited) return;
    memset(g_devices, 0, sizeof(g_devices));
    g_inited = 1;
}

int devfs_register(const devfs_device_ops_t *ops, void *ctx) {
    devfs_init_once();
    if (!ops || !ops->name || !ops->name[0] || !ops->read) return -1;

    // no duplicates
    for (int i = 0; i < DEVFS_MAX_DEVICES; i++) {
        if (g_devices[i].in_use && g_devices[i].ops && strcmp(g_devices[i].ops->name, ops->name) == 0) {
            return -2;
        }
    }

    for (int i = 0; i < DEVFS_MAX_DEVICES; i++) {
        if (!g_devices[i].in_use) {
            g_devices[i].ops = ops;
            g_devices[i].ctx = ctx;
            g_devices[i].in_use = 1;
            return 0;
        }
    }

    return -3;
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

void* devfs_open(const char *name, int flags) {
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
    h->dev = d;
    h->flags = flags;
    return h;
}

ssize_t devfs_read(void *handle, void *buf, size_t count) {
    devfs_handle_t *h = (devfs_handle_t*)handle;
    if (!h || !h->dev || !h->dev->ops || !h->dev->ops->read) return -1;
    return h->dev->ops->read(h->dev->ctx, buf, count);
}

ssize_t devfs_write(void *handle, const void *buf, size_t count) {
    devfs_handle_t *h = (devfs_handle_t*)handle;
    if (!h || !h->dev || !h->dev->ops || !h->dev->ops->write) return -1;
    return h->dev->ops->write(h->dev->ctx, buf, count);
}

int devfs_close(void *handle) {
    devfs_handle_t *h = (devfs_handle_t*)handle;
    if (!h) return -1;
    if (h->dev && h->dev->ops && h->dev->ops->close) {
        h->dev->ops->close(h->dev->ctx);
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
    int r1 = devfs_register(&g_dev_kbd0_ops, &g_kbd0);
    int r2 = devfs_register(&g_dev_evt0_ops, &g_evt0);
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
        info.fb_addr = (uint64_t)(uintptr_t)fb.addr;
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
    devfs_init_once();
    int r = devfs_register(&g_dev_video0_ops, NULL);
    if (r == 0) {
        com_write_string(COM1_PORT, "[DEVFS] Registered graphics devices: $/dev/graphics/video0\n");
    }
    return r;
}

void devfs_input_push_event(const Event *e) {
    if (!e) return;

    // publish structured event
    evt_stream_push(&g_evt0, e);

    // publish ASCII into kbd stream (very linux-like: $/dev/input/event* vs $/dev/tty)
    if (e->type == EVENT_KEY_PRESSED) {
        char c = e->data.keyboard.ascii;

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
