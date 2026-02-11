#include <stdint.h>
#include <stddef.h> // offsetof
#include <stddef.h>

#include "moduos/kernel/sqrm.h"
#include "moduos/kernel/errno.h"

/*
 * hid_sqrm.c
 *
 * Skeleton HID module (keyboard/mouse/etc.).
 * This typically depends on USB core (or could be generic for PS/2 etc.).
 */

static const char * const g_hid_deps[] = {
    "usb",
};

SQRM_DEFINE_MODULE_V2(SQRM_TYPE_HID, "hid", 1, 0, (uint16_t)(sizeof(g_hid_deps)/sizeof(g_hid_deps[0])), g_hid_deps);

#include "usb_core_api.h"
#include "moduos/drivers/input/input.h" // usb_hid_to_keycode, hid_keycode_to_ascii, usb_get_event_modifiers
#include "moduos/kernel/events/events.h"
#include "moduos/kernel/memory/string.h" // memset

typedef struct {
    int (*get_keyboard_present)(void);
    int (*get_mouse_present)(void);
    // Poll functions: return 1 if new report read, 0 if no data, negative errno on error.
    int (*poll_keyboard_boot)(uint8_t out_report[8]);
    int (*poll_mouse_boot)(uint8_t out_report[4]);
} sqrm_hid_api_v1_t;

static const sqrm_kernel_api_t *g_api;
static const usbcore_api_v1_t *g_usb;
static size_t g_usb_api_size = 0;

static uint8_t g_last_kbd_report[8];

// Double-buffered async transfers so we never mutate the active xfer from its own callback.
static uint8_t g_kbd_report_buf_a[8];
static uint8_t g_kbd_report_buf_b[8];
static usb_int_in_xfer_v1_t g_kbd_xfer_a;
static usb_int_in_xfer_v1_t g_kbd_xfer_b;
static usb_int_in_xfer_v1_t *g_kbd_active_xfer = NULL;

static int g_kbd_present;
static int g_mouse_present;

// Selected HID endpoints
static usb_device_info_v1_t g_kbd_dev;
static uint8_t g_kbd_ep;
static uint8_t g_kbd_mps;

static usb_device_info_v1_t g_mouse_dev;
static uint8_t g_mouse_ep;
static uint8_t g_mouse_mps;

static int hid_get_keyboard_present(void) { return g_kbd_present; }
static int hid_get_mouse_present(void) { return g_mouse_present; }

// Poll API removed: HID is interrupt-driven only.
static int hid_poll_keyboard_boot(uint8_t out_report[8]) {
    (void)out_report;
    return -ENOSYS;
}

static int hid_poll_mouse_boot(uint8_t out_report[4]) {
    (void)out_report;
    return -ENOSYS;
}

static const sqrm_hid_api_v1_t g_hid_api = {
    .get_keyboard_present = hid_get_keyboard_present,
    .get_mouse_present = hid_get_mouse_present,
    .poll_keyboard_boot = hid_poll_keyboard_boot,
    .poll_mouse_boot = hid_poll_mouse_boot,
};

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }

static void hid_log(const char *s) {
    if (g_api && g_api->com_write_string) g_api->com_write_string(0x3F8, s);
}

static int report_contains_key(const uint8_t rep[8], uint8_t key) {
    for (int i = 2; i < 8; i++) if (rep[i] == key) return 1;
    return 0;
}

static void hid_kbd_submit_async(usb_int_in_xfer_v1_t *xfer);

static void hid_kbd_int_done(usb_int_in_xfer_v1_t *xfer, void *user) {
    (void)user;
    if (!xfer) return;

    // Mark inactive before we schedule the next one.
    if (g_kbd_active_xfer == xfer) g_kbd_active_xfer = NULL;

    if (xfer->status == 0 && xfer->actual_length == 8) {
        uint8_t *rep = (uint8_t*)xfer->data;
        uint8_t mods = rep[0];

        if (g_api && g_api->input_push_event) {
            // Pressed keys
            for (int i = 2; i < 8; i++) {
                uint8_t key = rep[i];
                if (key == 0) continue;
                if (!report_contains_key(g_last_kbd_report, key)) {
                    KeyCode kc = usb_hid_to_keycode(key);
                    char ascii = hid_keycode_to_ascii(key, mods);
                    Event e = event_create_key_pressed(kc, key, ascii, usb_get_event_modifiers(mods), false);
                    g_api->input_push_event(&e);
                }
            }

            // Released keys
            for (int i = 2; i < 8; i++) {
                uint8_t key = g_last_kbd_report[i];
                if (key == 0) continue;
                if (!report_contains_key(rep, key)) {
                    KeyCode kc = usb_hid_to_keycode(key);
                    Event e = event_create_key_released(kc, key, usb_get_event_modifiers(mods), false);
                    g_api->input_push_event(&e);
                }
            }
        }

        memcpy(g_last_kbd_report, rep, 8);
    }

    // Re-arm using the *other* transfer object.
    usb_int_in_xfer_v1_t *next = (xfer == &g_kbd_xfer_a) ? &g_kbd_xfer_b : &g_kbd_xfer_a;
    hid_kbd_submit_async(next);
}

static int hid_is_canonical_ptr(uintptr_t p) {
    // Canonical x86_64 addresses have bits 48..63 all 0 or all 1.
    uint64_t top = ((uint64_t)p) >> 48;
    return (top == 0x0000u || top == 0xFFFFu);
}

static void hid_kbd_submit_async(usb_int_in_xfer_v1_t *xfer) {
    if (!g_kbd_present || !g_usb) return;
    if (!xfer) return;

    // ABI safety: ensure the exported usbcore_api_v1_t is large enough to include interrupt_in_async.
    size_t need = offsetof(usbcore_api_v1_t, interrupt_in_async) + sizeof(g_usb->interrupt_in_async);
    if (g_usb_api_size < need) {
        hid_log("[HID] usb API too small for interrupt_in_async; refusing\n");
        return;
    }

    uintptr_t fn = (uintptr_t)g_usb->interrupt_in_async;
    if (fn == 0 || !hid_is_canonical_ptr(fn) || fn < 0x100000u) {
        hid_log("[HID] interrupt_in_async pointer invalid; refusing\n");
        return;
    }

    // Debug: log function pointer
    if (g_api && g_api->com_write_string) {
        char h[17];
        static const char *hx = "0123456789abcdef";
        uint64_t v = (uint64_t)fn;
        for (int i = 15; i >= 0; i--) { h[i] = hx[v & 0xF]; v >>= 4; }
        h[16] = 0;
        g_api->com_write_string(0x3F8, "[HID] interrupt_in_async=0x");
        g_api->com_write_string(0x3F8, h);
        g_api->com_write_string(0x3F8, "\n");

        // Dump first 16 bytes at function pointer.
        g_api->com_write_string(0x3F8, "[HID] fn bytes: ");
        volatile const uint8_t *p = (volatile const uint8_t*)fn;
        for (int i = 0; i < 16; i++) {
            uint8_t b = p[i];
            char hh[3];
            hh[0] = hx[(b >> 4) & 0xF];
            hh[1] = hx[b & 0xF];
            hh[2] = 0;
            g_api->com_write_string(0x3F8, hh);
            g_api->com_write_string(0x3F8, (i == 15) ? "\n" : " ");
        }
    }

    // Do not touch the currently active xfer (reentrancy safety).
    if (g_kbd_active_xfer) return;

    // Debug: confirm we're calling the module's own memset (from sqrmlibc)
    {
        static const char *hx = "0123456789abcdef";
        char h[17];
        uint64_t v = (uint64_t)(uintptr_t)&memset;
        for (int i = 15; i >= 0; i--) { h[i] = hx[v & 0xF]; v >>= 4; }
        h[16] = 0;
        g_api->com_write_string(0x3F8, "[HID] &memset=0x");
        g_api->com_write_string(0x3F8, h);
        g_api->com_write_string(0x3F8, "\n");
    }
    hid_log("[HID] before memset\n");
    memset(xfer, 0, sizeof(*xfer));
    hid_log("[HID] after memset\n");
    xfer->controller_index = g_kbd_dev.controller_index;
    xfer->dev_addr = g_kbd_dev.addr;
    xfer->endpoint = g_kbd_ep;
    xfer->speed = g_kbd_dev.speed;
    xfer->ep_mps = g_kbd_mps; // boot keyboards are typically <= 8

    if (xfer == &g_kbd_xfer_a) xfer->data = g_kbd_report_buf_a;
    else xfer->data = g_kbd_report_buf_b;
    xfer->length = 8;

    g_kbd_active_xfer = xfer;
    hid_log("[HID] calling interrupt_in_async...\n");
    (void)g_usb->interrupt_in_async(xfer, hid_kbd_int_done, NULL);
    hid_log("[HID] returned from interrupt_in_async\n");
}

static int hid_try_bind_device(int dev_idx) {
    usb_device_info_v1_t di;
    if (g_usb->get_device_info(dev_idx, &di) != 0) return -EINVAL;

    // Fetch configuration descriptor header (first 9 bytes)
    uint8_t cfg_hdr[9];
    if (g_usb->control_in(di.controller_index, di.addr, di.speed,
                          0x80, USB_REQ_GET_DESCRIPTOR,
                          (uint16_t)((USB_DESC_CONFIGURATION << 8) | 0), 0,
                          cfg_hdr, sizeof(cfg_hdr)) != 0) {
        return -EIO;
    }

    uint16_t total_len = rd16(&cfg_hdr[2]);
    if (total_len < 9 || total_len > 512) return -EINVAL;

    uint8_t *cfg = (uint8_t*)g_api->kmalloc(total_len);
    if (!cfg) return -ENOMEM;

    int rc = g_usb->control_in(di.controller_index, di.addr, di.speed,
                               0x80, USB_REQ_GET_DESCRIPTOR,
                               (uint16_t)((USB_DESC_CONFIGURATION << 8) | 0), 0,
                               cfg, total_len);
    if (rc != 0) {
        g_api->kfree(cfg);
        return rc;
    }

    uint8_t cfg_value = cfg[5];

    // Parse descriptors
    uint8_t cur_iface = 0xFF;
    uint8_t cur_class = 0, cur_sub = 0, cur_proto = 0;

    for (uint16_t off = 0; off + 2 <= total_len; ) {
        uint8_t len = cfg[off];
        uint8_t type = cfg[off + 1];
        if (len == 0) break;
        if (off + len > total_len) break;

        if (type == USB_DESC_INTERFACE && len >= 9) {
            cur_iface = cfg[off + 2];
            cur_class = cfg[off + 5];
            cur_sub   = cfg[off + 6];
            cur_proto = cfg[off + 7];
        } else if (type == USB_DESC_ENDPOINT && len >= 7) {
            if (cur_class == USB_CLASS_HID) {
                uint8_t ep_addr = cfg[off + 2];
                uint8_t ep_attr = cfg[off + 3];
                uint16_t mps = rd16(&cfg[off + 4]);
                // uint8_t interval = cfg[off + 6];

                // interrupt IN only
                if ((ep_addr & 0x80) && ((ep_attr & 0x03) == 0x03)) {
                    uint8_t ep_num = (uint8_t)(ep_addr & 0x0F);

                    // Boot Interface Subclass = 1
                    if (cur_sub == 1 && cur_proto == 1 && !g_kbd_present) {
                        // keyboard
                        (void)g_usb->set_configuration(di.controller_index, di.addr, di.speed, cfg_value);
                        // SET_PROTOCOL (boot) to interface
                        (void)g_usb->control_out(di.controller_index, di.addr, di.speed,
                                                 0x21, HID_REQ_SET_PROTOCOL,
                                                 HID_PROTOCOL_BOOT, cur_iface,
                                                 NULL, 0);
                        // SET_IDLE (0)
                        (void)g_usb->control_out(di.controller_index, di.addr, di.speed,
                                                 0x21, HID_REQ_SET_IDLE,
                                                 0, cur_iface,
                                                 NULL, 0);

                        g_kbd_dev = di;
                        g_kbd_ep = ep_num;
                        g_kbd_mps = (uint8_t)(mps & 0xFF);
                        g_kbd_present = 1;
                        hid_log("[HID] Bound USB boot keyboard\n");
                    } else if (cur_sub == 1 && cur_proto == 2 && !g_mouse_present) {
                        // mouse
                        (void)g_usb->set_configuration(di.controller_index, di.addr, di.speed, cfg_value);
                        (void)g_usb->control_out(di.controller_index, di.addr, di.speed,
                                                 0x21, HID_REQ_SET_PROTOCOL,
                                                 HID_PROTOCOL_BOOT, cur_iface,
                                                 NULL, 0);
                        (void)g_usb->control_out(di.controller_index, di.addr, di.speed,
                                                 0x21, HID_REQ_SET_IDLE,
                                                 0, cur_iface,
                                                 NULL, 0);

                        g_mouse_dev = di;
                        g_mouse_ep = ep_num;
                        g_mouse_mps = (uint8_t)(mps & 0xFF);
                        g_mouse_present = 1;
                        hid_log("[HID] Bound USB boot mouse\n");
                    }
                }
            }
        }

        off = (uint16_t)(off + len);
    }

    g_api->kfree(cfg);
    return 0;
}

int sqrm_module_init(const sqrm_kernel_api_t *api) {
    if (!api || api->abi_version != 1) return -1;
    g_api = api;

    // bind usbcore
    if (api->sqrm_service_get) {
        size_t sz = 0;
        const void *p = api->sqrm_service_get("usb", &sz);
        if (p) {
            g_usb = (const usbcore_api_v1_t*)p;
            g_usb_api_size = sz;

            // ABI debug
            if (g_api && g_api->com_write_string) {
                char h[17];
                static const char *hx = "0123456789abcdef";
                uint64_t v;

                // ABI debug: log what we received and how we interpret it.
                {
                    char hptr[17];
                    static const char *hx2 = "0123456789abcdef";
                    uint64_t pv = (uint64_t)(uintptr_t)p;
                    for (int i = 15; i >= 0; i--) { hptr[i] = hx2[pv & 0xF]; pv >>= 4; }
                    hptr[16] = 0;
                    g_api->com_write_string(0x3F8, "[HID] usb api ptr=0x");
                    g_api->com_write_string(0x3F8, hptr);
                    g_api->com_write_string(0x3F8, "\n");
                }

                g_api->com_write_string(0x3F8, "[HID] usb api sz=0x");
                {
                    char hsz[17];
                    uint64_t vv = (uint64_t)sz;
                    for (int i = 15; i >= 0; i--) { hsz[i] = hx[vv & 0xF]; vv >>= 4; }
                    hsz[16] = 0;
                    g_api->com_write_string(0x3F8, hsz);
                }

                g_api->com_write_string(0x3F8, " sizeof=0x");
                {
                    char hsz2[17];
                    uint64_t vv = (uint64_t)sizeof(usbcore_api_v1_t);
                    for (int i = 15; i >= 0; i--) { hsz2[i] = hx[vv & 0xF]; vv >>= 4; }
                    hsz2[16] = 0;
                    g_api->com_write_string(0x3F8, hsz2);
                }

                size_t off = offsetof(usbcore_api_v1_t, interrupt_in_async);
                g_api->com_write_string(0x3F8, " off_async=0x");
                {
                    char hoff[17];
                    uint64_t vv = (uint64_t)off;
                    for (int i = 15; i >= 0; i--) { hoff[i] = hx[vv & 0xF]; vv >>= 4; }
                    hoff[16] = 0;
                    g_api->com_write_string(0x3F8, hoff);
                }
                g_api->com_write_string(0x3F8, "\n");

                // direct read via struct field
                {
                    uint64_t fnp2 = (uint64_t)(uintptr_t)g_usb->interrupt_in_async;
                    v = fnp2;
                    for (int i = 15; i >= 0; i--) { h[i] = hx[v & 0xF]; v >>= 4; }
                    h[16] = 0;
                    g_api->com_write_string(0x3F8, "[HID] async ptr(field)=0x");
                    g_api->com_write_string(0x3F8, h);
                    g_api->com_write_string(0x3F8, "\n");
                }

                // manual read of pointer at that offset
                const uint8_t *bp = (const uint8_t*)p;
                uint64_t fnp = *(const uint64_t*)(bp + off);
                v = fnp;
                for (int i = 15; i >= 0; i--) { h[i] = hx[v & 0xF]; v >>= 4; }
                h[16] = 0;
                g_api->com_write_string(0x3F8, "[HID] async ptr@off=0x");
                g_api->com_write_string(0x3F8, h);
                g_api->com_write_string(0x3F8, "\n");
            }
        }
    }

    hid_log("[HID] HID module init\n");

    if (g_usb) {
        int n = g_usb->get_device_count();
        for (int i = 0; i < n; i++) {
            (void)hid_try_bind_device(i);
        }
    } else {
        hid_log("[HID] Missing usbcore service\n");
    }

    // Start interrupt-driven keyboard input (no polling)
    if (g_kbd_present && g_usb && g_usb->interrupt_in_async) {
        hid_log("[HID] Starting interrupt-driven keyboard\n");
        hid_kbd_submit_async(&g_kbd_xfer_a);
    } else if (g_kbd_present) {
        hid_log("[HID] Keyboard present but interrupt_in_async unavailable\n");
    }

    if (api->sqrm_service_register) {
        int r = api->sqrm_service_register("hid", &g_hid_api, sizeof(g_hid_api));
        hid_log(r == 0 ? "[HID] exported service: hid\n" : "[HID] failed to export service: hid\n");
    }

    return 0;
}
