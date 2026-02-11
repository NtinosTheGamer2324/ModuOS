#include <stdint.h>
#include <stddef.h>

#include "moduos/kernel/sqrm.h"
#include "moduos/kernel/errno.h"

#include "usb_core_api.h"


static const char * const g_usb_deps[] = {
    "uhci",
};

SQRM_DEFINE_MODULE_V2(SQRM_TYPE_USB, "usb", 1, 0, (uint16_t)(sizeof(g_usb_deps)/sizeof(g_usb_deps[0])), g_usb_deps);

// Must match sdk/sqrm_sdk.h controller ABI (subset used here)
typedef struct {
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} __attribute__((packed)) sqrm_usb_setup_packet_t;

typedef struct {
    uint8_t dev_addr;
    uint8_t endpoint;
    uint8_t speed;
    uint8_t xfer_type;
    sqrm_usb_setup_packet_t setup;
    void   *data;
    uint32_t length;
    uint8_t direction_in;
    int32_t status;
    uint32_t actual_length;
} sqrm_usb_transfer_v1_t;

typedef uint32_t sqrm_usb_xfer_handle_t;
#define SQRM_USB_XFER_INVALID_HANDLE 0u

typedef struct {
    uint8_t bus, device, function;
    uint8_t irq_line;
    uint16_t io_base;
} sqrm_uhci_controller_info_v1_t;

typedef void (*sqrm_usb_xfer_cb_v1_t)(sqrm_usb_xfer_handle_t handle, sqrm_usb_transfer_v1_t *xfer, void *user);

typedef struct {
    int (*get_controller_count)(void);
    int (*get_controller_info)(int index, sqrm_uhci_controller_info_v1_t *out);

    // port ops
    int (*get_port_count)(int controller_index);
    uint16_t (*read_portsc)(int controller_index, int port_index);
    int (*reset_port)(int controller_index, int port_index);

    // transfers
    sqrm_usb_xfer_handle_t (*submit)(int controller_index, sqrm_usb_transfer_v1_t *xfer);
    int (*wait)(sqrm_usb_xfer_handle_t handle, uint32_t timeout_ms);
    int (*cancel)(sqrm_usb_xfer_handle_t handle);

    // async completion (IRQ-driven)
    int (*set_callback)(sqrm_usb_xfer_handle_t handle, sqrm_usb_xfer_cb_v1_t cb, void *user);
} sqrm_usbctl_uhci_api_v1_t;

#define REQ_GET_DESCRIPTOR 0x06
#define REQ_SET_ADDRESS    0x05

#define DESC_DEVICE        0x01

static void log_str(const sqrm_kernel_api_t *api, const char *s) {
    if (api && api->com_write_string) api->com_write_string(0x3F8, s);
}

static void log_hex8(const sqrm_kernel_api_t *api, uint8_t v) {
    static const char hex[] = "0123456789abcdef";
    char b[3];
    b[0] = hex[(v >> 4) & 0xF];
    b[1] = hex[v & 0xF];
    b[2] = 0;
    api->com_write_string(0x3F8, b);
}

static void log_hex16(const sqrm_kernel_api_t *api, uint16_t v) {
    static const char hex[] = "0123456789abcdef";
    char b[5];
    b[0] = hex[(v >> 12) & 0xF];
    b[1] = hex[(v >> 8) & 0xF];
    b[2] = hex[(v >> 4) & 0xF];
    b[3] = hex[v & 0xF];
    b[4] = 0;
    api->com_write_string(0x3F8, b);
}

// Debug control for verbose descriptor dumps
#define USB_DEBUG_VERBOSE 0

static void dump_bytes(const sqrm_kernel_api_t *api, const char *prefix, const uint8_t *buf, size_t n) {
#if USB_DEBUG_VERBOSE
    log_str(api, prefix);
    for (size_t i = 0; i < n; i++) {
        log_hex8(api, buf[i]);
        log_str(api, (i + 1 == n) ? "\n" : " ");
    }
#else
    (void)api; (void)prefix; (void)buf; (void)n;
#endif
}

static int uhci_control_in(const sqrm_kernel_api_t *api, const sqrm_usbctl_uhci_api_v1_t *uhci,
                           int ctrl, uint8_t addr, uint8_t speed, uint8_t request, uint16_t value, uint16_t index,
                           void *data, uint16_t len) {
    sqrm_usb_transfer_v1_t x = {0};
    x.dev_addr = addr;
    x.endpoint = 0;
    x.speed = speed;
    x.xfer_type = 1; // CONTROL
    x.setup.bmRequestType = 0x80; // IN, standard, device
    x.setup.bRequest = request;
    x.setup.wValue = value;
    x.setup.wIndex = index;
    x.setup.wLength = len;
    x.data = data;
    x.length = len;
    x.direction_in = 1;

    sqrm_usb_xfer_handle_t h = uhci->submit(ctrl, &x);
    if (h == SQRM_USB_XFER_INVALID_HANDLE) return x.status ? x.status : -EIO;
    int r = uhci->wait(h, 200);
    return r;
}

// --- usbcore service (in-tree modules) ---
static const sqrm_kernel_api_t *g_api;
static const sqrm_usbctl_uhci_api_v1_t *g_uhci;

static usb_device_info_v1_t g_devs[8];
static int g_dev_count;

static int uhci_control_set_address(const sqrm_kernel_api_t *api, const sqrm_usbctl_uhci_api_v1_t *uhci,
                                   int ctrl, uint8_t speed, uint8_t new_addr) {
    sqrm_usb_transfer_v1_t x = {0};
    x.dev_addr = 0;
    x.endpoint = 0;
    x.speed = speed;
    x.xfer_type = 1;
    x.setup.bmRequestType = 0x00; // OUT, standard, device
    x.setup.bRequest = REQ_SET_ADDRESS;
    x.setup.wValue = new_addr;
    x.setup.wIndex = 0;
    x.setup.wLength = 0;
    x.data = NULL;
    x.length = 0;
    x.direction_in = 0;

    sqrm_usb_xfer_handle_t h = uhci->submit(ctrl, &x);
    if (h == SQRM_USB_XFER_INVALID_HANDLE) return x.status ? x.status : -EIO;
    int r = uhci->wait(h, 200);
    if (api->inb) { for (volatile int i = 0; i < 10000; i++) (void)api->inb(0x80); }
    return r;
}

static int usb_enumerate_uhci(const sqrm_kernel_api_t *api, const sqrm_usbctl_uhci_api_v1_t *uhci) {
    g_dev_count = 0;

    int ctrl_count = uhci->get_controller_count ? uhci->get_controller_count() : 0;
    if (ctrl_count <= 0) {
        log_str(api, "[USB] No UHCI controllers\n");
        return 0;
    }

    for (int c = 0; c < ctrl_count; c++) {
        int ports = uhci->get_port_count ? uhci->get_port_count(c) : 0;
        log_str(api, "[USB] UHCI controller ports=");
        log_hex16(api, (uint16_t)ports);
        log_str(api, "\n");

        for (int p = 0; p < ports; p++) {
            uint16_t st = uhci->read_portsc ? uhci->read_portsc(c, p) : 0;
            log_str(api, "[USB] Port ");
            log_hex16(api, (uint16_t)p);
            log_str(api, " PORTSC=");
            log_hex16(api, st);
            log_str(api, "\n");

            // UHCI PORTSC bit0 (CCS) indicates a device is present.
            // Don't attempt reset/enumeration if nothing is connected.
            if ((st & 0x0001) == 0) {
                continue;
            }

            if (uhci->reset_port) uhci->reset_port(c, p);
            // Don't rely on kernel ticks during early bring-up (UEFI/q35 may not have PIT ticks working).
    // Use a small I/O delay loop instead.
    if (api->inb) {
        for (volatile int m = 0; m < 20; m++) {
            for (volatile int i = 0; i < 1000; i++) (void)api->inb(0x80);
        }
    }

            // determine speed from LSDA bit in PORTSC
            // UHCI PORTSC: LSDA is bit 8 (0x0100). Bit 7 (0x0080) is *not* LSDA.
            // Using the wrong bit causes us to mark full-speed devices as low-speed,
            // which makes UHCI set the TD "LS" bit and breaks enumeration on QEMU.
            st = uhci->read_portsc ? uhci->read_portsc(c, p) : st;
            uint8_t speed = (st & 0x0100) ? 1 : 2; // 1=LOW, 2=FULL

            // try descriptor reads on addr 0
            uint8_t dev_desc[18];
            int ok = 0;
            uint32_t backoff_ms[3] = {10, 20, 40};
            for (int attempt = 0; attempt < 3; attempt++) {
                for (int i = 0; i < 18; i++) dev_desc[i] = 0;

                // First read 8 bytes so we learn bMaxPacketSize0
                int r = uhci_control_in(api, uhci, c, 0, speed, REQ_GET_DESCRIPTOR,
                                        (uint16_t)((DESC_DEVICE << 8) | 0), 0,
                                        dev_desc, 8);
                if (r == 0) {
                    ok = 1;
                    break;
                }
                log_str(api, "[USB] GET_DESCRIPTOR failed (attempt)\n");
                if (api->inb) {
                    // backoff without relying on timer ticks
                    for (volatile int m = 0; m < (int)backoff_ms[attempt]; m++) {
                        for (volatile int i = 0; i < 1000; i++) (void)api->inb(0x80);
                    }
                }
            }

            if (!ok) {
                log_str(api, "[USB] Port: no device or descriptor read failed\n");
                continue;
            }

            // Now read full descriptor (18 bytes)
            uint8_t maxp0 = dev_desc[7];
            (void)maxp0;
            for (int i = 0; i < 18; i++) dev_desc[i] = 0;
            (void)uhci_control_in(api, uhci, c, 0, speed, REQ_GET_DESCRIPTOR,
                                  (uint16_t)((DESC_DEVICE << 8) | 0), 0,
                                  dev_desc, 18);

            dump_bytes(api, "[USB] Device desc: ", dev_desc, 18);

            uint16_t vid = (uint16_t)(dev_desc[8] | ((uint16_t)dev_desc[9] << 8));
            uint16_t pid = (uint16_t)(dev_desc[10] | ((uint16_t)dev_desc[11] << 8));

            log_str(api, "[USB] VID:PID=");
            log_hex16(api, vid);
            log_str(api, ":");
            log_hex16(api, pid);
            log_str(api, " class=");
            log_hex8(api, dev_desc[4]);
            log_str(api, " sub=");
            log_hex8(api, dev_desc[5]);
            log_str(api, " proto=");
            log_hex8(api, dev_desc[6]);
            log_str(api, "\n");

            // set address 1 (simple)
            uint8_t new_addr = 1;
            int r = uhci_control_set_address(api, uhci, c, speed, new_addr);
            if (r != 0) {
                log_str(api, "[USB] SET_ADDRESS failed\n");
                continue;
            }

            for (int i = 0; i < 18; i++) dev_desc[i] = 0;
            (void)uhci_control_in(api, uhci, c, 1, speed, REQ_GET_DESCRIPTOR,
                                  (uint16_t)((DESC_DEVICE << 8) | 0), 0,
                                  dev_desc, 18);
            dump_bytes(api, "[USB] Device desc (addr=1): ", dev_desc, 18);

            // Record device
            if (g_dev_count < (int)(sizeof(g_devs)/sizeof(g_devs[0]))) {
                usb_device_info_v1_t *di = &g_devs[g_dev_count++];
                di->controller_index = c;
                di->addr = new_addr;
                di->speed = speed;
                di->vid = vid;
                di->pid = pid;
                di->dev_class = dev_desc[4];
                di->dev_subclass = dev_desc[5];
                di->dev_protocol = dev_desc[6];
            }
        }
    }

    return 0;
}

static int usbcore_get_device_count(void) { return g_dev_count; }
static int usbcore_get_device_info(int idx, usb_device_info_v1_t *out) {
    if (!out) return -EINVAL;
    if (idx < 0 || idx >= g_dev_count) return -EINVAL;
    *out = g_devs[idx];
    return 0;
}

static int usbcore_control_in(int controller_index, uint8_t addr, uint8_t speed,
                              uint8_t bmRequestType, uint8_t request,
                              uint16_t value, uint16_t index,
                              void *data, uint16_t len) {
    if (!g_uhci) return -ENODEV;

    sqrm_usb_transfer_v1_t x = {0};
    x.dev_addr = addr;
    x.endpoint = 0;
    x.speed = speed;
    x.xfer_type = 1; // CONTROL
    x.setup.bmRequestType = bmRequestType;
    x.setup.bRequest = request;
    x.setup.wValue = value;
    x.setup.wIndex = index;
    x.setup.wLength = len;
    x.data = data;
    x.length = len;
    x.direction_in = 1;

    sqrm_usb_xfer_handle_t h = g_uhci->submit(controller_index, &x);
    if (h == SQRM_USB_XFER_INVALID_HANDLE) return x.status ? x.status : -EIO;
    return g_uhci->wait(h, 200);
}

static int usbcore_control_out(int controller_index, uint8_t addr, uint8_t speed,
                               uint8_t bmRequestType, uint8_t request,
                               uint16_t value, uint16_t index,
                               const void *data, uint16_t len) {
    if (!g_uhci) return -ENODEV;

    sqrm_usb_transfer_v1_t x = {0};
    x.dev_addr = addr;
    x.endpoint = 0;
    x.speed = speed;
    x.xfer_type = 1; // CONTROL
    x.setup.bmRequestType = bmRequestType;
    x.setup.bRequest = request;
    x.setup.wValue = value;
    x.setup.wIndex = index;
    x.setup.wLength = len;
    x.data = (void*)data;
    x.length = len;
    x.direction_in = 0;

    sqrm_usb_xfer_handle_t h = g_uhci->submit(controller_index, &x);
    if (h == SQRM_USB_XFER_INVALID_HANDLE) return x.status ? x.status : -EIO;
    return g_uhci->wait(h, 200);
}

static int usbcore_set_address(int controller_index, uint8_t speed, uint8_t new_addr) {
    if (!g_uhci) return -ENODEV;
    return uhci_control_set_address(g_api, g_uhci, controller_index, speed, new_addr);
}

static int usbcore_set_configuration(int controller_index, uint8_t addr, uint8_t speed, uint8_t cfg_value) {
    // Standard SET_CONFIGURATION
    return usbcore_control_out(controller_index, addr, speed, 0x00, USB_REQ_SET_CONFIGURATION,
                               cfg_value, 0, NULL, 0);
}

static int usbcore_interrupt_in(usb_int_in_xfer_v1_t *xfer, uint32_t timeout_ms) {
    if (!xfer) return -EINVAL;
    if (!g_uhci) return -ENODEV;

    sqrm_usb_transfer_v1_t t = {0};
    t.dev_addr = xfer->dev_addr;
    t.endpoint = xfer->endpoint;
    t.speed = xfer->speed;
    t.xfer_type = 3; // INTERRUPT
    t.data = xfer->data;
    t.length = xfer->length;
    t.direction_in = 1;

    sqrm_usb_xfer_handle_t h = g_uhci->submit(xfer->controller_index, &t);
    if (h == SQRM_USB_XFER_INVALID_HANDLE) {
        xfer->status = t.status ? t.status : -EIO;
        return xfer->status;
    }

    int r = g_uhci->wait(h, timeout_ms);
    xfer->status = r;
    xfer->actual_length = t.actual_length;
    return r;
}

typedef struct {
    usb_int_in_xfer_v1_t *xfer;
    usb_int_in_cb_v1_t cb;
    void *user;
    sqrm_usb_transfer_v1_t *t;
} usb_int_async_ctx_t;

static void usbcore_int_done(sqrm_usb_xfer_handle_t handle, sqrm_usb_transfer_v1_t *t, void *user) {
    (void)handle;
    usb_int_async_ctx_t *ctx = (usb_int_async_ctx_t*)user;
    if (!ctx) return;

    ctx->xfer->status = t ? t->status : -EIO;
    ctx->xfer->actual_length = t ? t->actual_length : 0;

    // Invoke user callback
    if (ctx->cb) ctx->cb(ctx->xfer, ctx->user);

    // free transfer struct + ctx
    if (g_api && g_api->kfree) {
        if (ctx->t) g_api->kfree(ctx->t);
        g_api->kfree(ctx);
    }
}

static inline void usb_outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static void usb_com1_write_string_raw(const char *s) {
    if (!s) return;
    while (*s) {
        usb_outb(0x3F8, (uint8_t)*s++);
    }
}

static void usb_dbg_hex64(uint64_t v, char out[17]) {
    static const char *hx = "0123456789abcdef";
    for (int i = 15; i >= 0; i--) { out[i] = hx[v & 0xF]; v >>= 4; }
    out[16] = 0;
}

static int usbcore_interrupt_in_async(usb_int_in_xfer_v1_t *xfer, usb_int_in_cb_v1_t cb, void *user) {
    // Marker: prove we entered this function (MUST be before any early returns)
    usb_outb(0x3F8, 'U');
    usb_com1_write_string_raw("[USB] interrupt_in_async enter\n");

    // Debug: dump args and key globals
    {
        char h[17];
        usb_dbg_hex64((uint64_t)(uintptr_t)xfer, h);
        usb_com1_write_string_raw("[USB]   xfer=0x");
        usb_com1_write_string_raw(h);
        usb_dbg_hex64((uint64_t)(uintptr_t)cb, h);
        usb_com1_write_string_raw(" cb=0x");
        usb_com1_write_string_raw(h);
        usb_dbg_hex64((uint64_t)(uintptr_t)user, h);
        usb_com1_write_string_raw(" user=0x");
        usb_com1_write_string_raw(h);
        usb_com1_write_string_raw("\n");

        usb_dbg_hex64((uint64_t)(uintptr_t)g_uhci, h);
        usb_com1_write_string_raw("[USB]   g_uhci=0x");
        usb_com1_write_string_raw(h);
        usb_dbg_hex64((uint64_t)(uintptr_t)g_api, h);
        usb_com1_write_string_raw(" g_api=0x");
        usb_com1_write_string_raw(h);
        usb_com1_write_string_raw("\n");
    }

    if (!xfer || !cb) return -EINVAL;
    if (!g_uhci || !g_uhci->submit || !g_uhci->set_callback) return -ENODEV;

    if (!g_api || !g_api->kmalloc || !g_api->kfree) {
        usb_com1_write_string_raw("[USB] g_api missing kmalloc/kfree\n");
        return -ENOSYS;
    }

    // Debug: log UHCI service function pointers (catches ABI mismatch/truncation)
    {
        char a[17], b[17];
        usb_dbg_hex64((uint64_t)(uintptr_t)g_uhci->submit, a);
        usb_dbg_hex64((uint64_t)(uintptr_t)g_uhci->set_callback, b);
        usb_com1_write_string_raw("[USB] uhci->submit=0x");
        usb_com1_write_string_raw(a);
        usb_com1_write_string_raw(" set_callback=0x");
        usb_com1_write_string_raw(b);
        usb_com1_write_string_raw("\n");
    }

    usb_int_async_ctx_t *ctx = (usb_int_async_ctx_t*)g_api->kmalloc(sizeof(*ctx));
    if (!ctx) return -ENOMEM;
    ctx->xfer = xfer;
    ctx->cb = cb;
    ctx->user = user;

    sqrm_usb_transfer_v1_t *t = (sqrm_usb_transfer_v1_t*)g_api->kmalloc(sizeof(*t));
    if (!t) {
        g_api->kfree(ctx);
        return -ENOMEM;
    }
    *t = (sqrm_usb_transfer_v1_t){0};
    t->dev_addr = xfer->dev_addr;
    t->endpoint = xfer->endpoint;
    t->speed = xfer->speed;
    t->xfer_type = 3; // INTERRUPT
    t->data = xfer->data;
    t->length = xfer->length;
    t->direction_in = 1;

    ctx->t = t;

    sqrm_usb_xfer_handle_t h = g_uhci->submit(xfer->controller_index, t);
    if (h == SQRM_USB_XFER_INVALID_HANDLE) {
        int err = t->status ? t->status : -EIO;
        g_api->kfree(t);
        g_api->kfree(ctx);
        return err;
    }

    int r = g_uhci->set_callback(h, usbcore_int_done, ctx);
    if (r != 0) {
        // best-effort cancel not implemented
        g_api->kfree(t);
        g_api->kfree(ctx);
        return r;
    }

    return 0;
}

static const usbcore_api_v1_t g_usbcore_api = {
    .get_device_count = usbcore_get_device_count,
    .get_device_info = usbcore_get_device_info,
    .control_in = usbcore_control_in,
    .control_out = usbcore_control_out,
    .set_address = usbcore_set_address,
    .set_configuration = usbcore_set_configuration,
    .interrupt_in = usbcore_interrupt_in,
    .interrupt_in_async = usbcore_interrupt_in_async,
};

static void usb_log_abi(void) {
    // NOTE: Do not call itoa() from SQRM modules; it may not be linked/resolved.
    // Print values as hex using local helpers only.
    char h[17];

    usb_com1_write_string_raw("[USB] sizeof(api)=0x");
    usb_dbg_hex64((uint64_t)sizeof(usbcore_api_v1_t), h);
    usb_com1_write_string_raw(h);

    usb_com1_write_string_raw(" off_async=0x");
    usb_dbg_hex64((uint64_t)offsetof(usbcore_api_v1_t, interrupt_in_async), h);
    usb_com1_write_string_raw(h);

    usb_com1_write_string_raw("\n");
}

int sqrm_module_init(const sqrm_kernel_api_t *api) {
    if (!api || api->abi_version != 1) return -1;
    g_api = api;

    log_str(api, "[USB] usb init (enumerate at init)\n");

    // ABI debug: confirm struct layout and exported function pointers.
    usb_log_abi();
    {
        char h[17];
        usb_dbg_hex64((uint64_t)(uintptr_t)g_usbcore_api.interrupt_in_async, h);
        usb_com1_write_string_raw("[USB] export interrupt_in_async=0x");
        usb_com1_write_string_raw(h);
        usb_com1_write_string_raw("\n");
    }

    const sqrm_usbctl_uhci_api_v1_t *uhci = NULL;
    if (api->sqrm_service_get) {
        size_t sz = 0;
        const void *p = api->sqrm_service_get("usbctl_uhci", &sz);
        if (p) {
            uhci = (const sqrm_usbctl_uhci_api_v1_t*)p;
            g_uhci = uhci;
            log_str(api, "[USB] Bound controller service: usbctl_uhci\n");
        } else {
            log_str(api, "[USB] Missing controller service usbctl_uhci\n");
        }
    }

    if (uhci && uhci->submit && uhci->wait) {
        (void)usb_enumerate_uhci(api, uhci);
    } else {
        log_str(api, "[USB] UHCI controller API incomplete\n");
    }

    if (api->sqrm_service_register) {
        // Keep legacy service name "usb" for compatibility, but export usbcore API.
        int r = api->sqrm_service_register("usb", &g_usbcore_api, sizeof(g_usbcore_api));
        log_str(api, r == 0 ? "[USB] exported service: usb\n" : "[USB] failed to export service: usb\n");
    }

    return 0;
}
