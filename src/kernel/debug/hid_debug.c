#include "moduos/kernel/sqrm.h"
#include "moduos/kernel/COM/com.h"
#include "moduos/arch/AMD64/interrupts/timer.h"
#include <stdint.h>
#include <stddef.h>

// Keep this file tiny and opt-in.
#ifndef HID_DEBUG_POLL
#define HID_DEBUG_POLL 0
#endif

#if HID_DEBUG_POLL

// Must match the service struct exported by modules/hid_sqrm.c
typedef struct {
    int (*get_keyboard_present)(void);
    int (*get_mouse_present)(void);
    int (*poll_keyboard_boot)(uint8_t out_report[8]);
    int (*poll_mouse_boot)(uint8_t out_report[4]);
} sqrm_hid_api_v1_t;

static int memeq(const uint8_t *a, const uint8_t *b, size_t n) {
    for (size_t i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

static void dump_hex(const uint8_t *b, size_t n) {
    static const char h[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        char out[3];
        out[0] = h[(b[i] >> 4) & 0xF];
        out[1] = h[b[i] & 0xF];
        out[2] = 0;
        com_write_string(COM1_PORT, out);
        if (i + 1 != n) com_write_string(COM1_PORT, " ");
    }
}

void hid_debug_poll_early(void) {
    size_t sz = 0;
    const void *p = sqrm_service_get_kernel("hid", &sz);
    if (!p || sz < sizeof(sqrm_hid_api_v1_t)) {
        com_write_string(COM1_PORT, "[HID-DBG] hid service not available\n");
        return;
    }

    const sqrm_hid_api_v1_t *hid = (const sqrm_hid_api_v1_t*)p;

    int kbd = hid->get_keyboard_present ? hid->get_keyboard_present() : 0;
    int mse = hid->get_mouse_present ? hid->get_mouse_present() : 0;

    com_write_string(COM1_PORT, "[HID-DBG] present: kbd=");
    com_write_string(COM1_PORT, kbd ? "1" : "0");
    com_write_string(COM1_PORT, " mouse=");
    com_write_string(COM1_PORT, mse ? "1" : "0");
    com_write_string(COM1_PORT, "\n");

    uint8_t last_kbd[8] = {0};
    uint8_t last_mse[4] = {0};

    uint64_t start = get_system_ticks();
    uint64_t end = start + ms_to_ticks(10000); // 10 seconds

    while (get_system_ticks() < end) {
        if (kbd && hid->poll_keyboard_boot) {
            uint8_t rep[8];
            int r = hid->poll_keyboard_boot(rep);
            if (r > 0 && !memeq(rep, last_kbd, 8)) {
                for (int i = 0; i < 8; i++) last_kbd[i] = rep[i];
                com_write_string(COM1_PORT, "[HID-DBG] kbd: ");
                dump_hex(rep, 8);
                com_write_string(COM1_PORT, "\n");
            }
        }

        if (mse && hid->poll_mouse_boot) {
            uint8_t rep[4];
            int r = hid->poll_mouse_boot(rep);
            if (r > 0 && !memeq(rep, last_mse, 4)) {
                for (int i = 0; i < 4; i++) last_mse[i] = rep[i];
                com_write_string(COM1_PORT, "[HID-DBG] mouse: ");
                dump_hex(rep, 4);
                com_write_string(COM1_PORT, "\n");
            }
        }
    }

    com_write_string(COM1_PORT, "[HID-DBG] done\n");
}

#else
void hid_debug_poll_early(void) { }
#endif
