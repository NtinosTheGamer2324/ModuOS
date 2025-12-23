#include "moduos/drivers/input/input.h"
#include "moduos/drivers/input/ps2/ps2.h" // ps2_init(), keyboard_irq_handler()

#include "moduos/drivers/USB/usb.h"
#include "moduos/drivers/USB/Classes/hid.h"
#include "moduos/drivers/USB/Controllers/uhci.h"
#include "moduos/drivers/USB/Controllers/ohci.h"
#include "moduos/drivers/USB/Controllers/ehci.h"

#include "moduos/kernel/COM/com.h"
#include "moduos/kernel/macros.h"
#include "moduos/kernel/events/events.h"
#include "moduos/drivers/graphics/VGA.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/fs/devfs.h"

#include <stdbool.h>

// USB HID state tracking
static hid_keyboard_report_t last_usb_kbd_report = {0};

// ---------------- Line discipline (POSIX-ish) ----------------

static char g_line_buf[256];
static size_t g_line_idx = 0;

void replace_input_line(const char* new_text) {
    if (!new_text) new_text = "";

    // Erase current line visually
    while (g_line_idx > 0) {
        g_line_idx--;
        VGA_Backspace();
    }

    // Copy + echo
    size_t len = strlen(new_text);
    if (len >= sizeof(g_line_buf)) len = sizeof(g_line_buf) - 1;

    for (size_t i = 0; i < len; i++) {
        g_line_buf[i] = new_text[i];
        VGA_WriteChar(new_text[i]);
    }

    g_line_idx = len;
    g_line_buf[g_line_idx] = 0;
}

// Read a line from $/dev/input/event0, echoing to VGA.
// Arrow keys call shell hooks for history browsing.
char* input(void) {
    /*
     * Ensure interrupts are enabled while we wait for input.
     * Some call paths (or future scheduler changes) may enter with IF=0; without this,
     * blocking reads can appear as "keyboard dead".
     */
    __asm__ volatile("sti" ::: "memory");

    g_line_idx = 0;
    g_line_buf[0] = 0;

    void *h = devfs_open("event0", O_RDONLY);
    if (!h) return g_line_buf;

    for (;;) {
        Event e;
        ssize_t n = devfs_read(h, &e, sizeof(e));
        if (n != (ssize_t)sizeof(e)) continue;
        if (e.type != EVENT_KEY_PRESSED) continue;

        // Shell history keys
        if (e.data.keyboard.keycode == KEY_ARROW_UP) {
            extern void up_arrow_pressed(void);
            up_arrow_pressed();
            continue;
        }
        if (e.data.keyboard.keycode == KEY_ARROW_DOWN) {
            extern void down_arrow_pressed(void);
            down_arrow_pressed();
            continue;
        }

        if (e.data.keyboard.keycode == KEY_BACKSPACE) {
            if (g_line_idx > 0) {
                g_line_idx--;
                g_line_buf[g_line_idx] = 0;
                VGA_Backspace();
            }
            continue;
        }

        if (e.data.keyboard.keycode == KEY_TAB) {
            // Keep old behavior: tab inserts 2 spaces
            if (g_line_idx + 2 < sizeof(g_line_buf)) {
                g_line_buf[g_line_idx++] = ' ';
                g_line_buf[g_line_idx++] = ' ';
                g_line_buf[g_line_idx] = 0;
                VGA_Write("  ");
            }
            continue;
        }

        if (e.data.keyboard.keycode == KEY_ENTER) {
            VGA_WriteChar('\n');
            break;
        }

        char c = e.data.keyboard.ascii;
        if (c && g_line_idx + 1 < sizeof(g_line_buf)) {
            g_line_buf[g_line_idx++] = c;
            g_line_buf[g_line_idx] = 0;
            VGA_WriteChar(c);
        }
    }

    devfs_close(h);
    return g_line_buf;
}

// Initialize all input subsystems
int input_init(void) {
    COM_LOG_INFO(COM1_PORT, "Initializing input subsystem");

    // PS/2
    COM_LOG_INFO(COM1_PORT, "Initializing PS/2 input");
    if (ps2_init() != 0) {
        COM_LOG_WARN(COM1_PORT, "PS/2 did not respond! (This happens on some VMs)");
    } else {
        COM_LOG_OK(COM1_PORT, "PS/2 initialized");

    }

    // USB + HID
    COM_LOG_INFO(COM1_PORT, "Initializing USB input");
    usb_init();
    hid_init();
    COM_LOG_OK(COM1_PORT, "USB input initialized");

    COM_LOG_OK(COM1_PORT, "Input subsystem initialized");
    return 0;
}

// Convert USB HID modifiers to event system modifiers
uint8_t usb_get_event_modifiers(uint8_t hid_mods) {
    uint8_t mods = 0;
    if (hid_mods & (HID_MOD_LEFT_SHIFT | HID_MOD_RIGHT_SHIFT)) mods |= MOD_SHIFT;
    if (hid_mods & (HID_MOD_LEFT_CTRL | HID_MOD_RIGHT_CTRL)) mods |= MOD_CTRL;
    if (hid_mods & (HID_MOD_LEFT_ALT | HID_MOD_RIGHT_ALT)) mods |= MOD_ALT;
    return mods;
}

// Convert USB HID keycode to internal KeyCode enum
KeyCode usb_hid_to_keycode(uint8_t hid_code) {
    switch (hid_code) {
        case HID_KEY_ENTER: return KEY_ENTER;
        case HID_KEY_ESCAPE: return KEY_ESCAPE;
        case HID_KEY_BACKSPACE: return KEY_BACKSPACE;
        case HID_KEY_TAB: return KEY_TAB;
        case HID_KEY_SPACE: return KEY_SPACE;
        case HID_KEY_CAPS_LOCK: return KEY_CAPS_LOCK;
        case HID_KEY_F1: return KEY_F1;
        case HID_KEY_F2: return KEY_F2;
        case HID_KEY_F3: return KEY_F3;
        case HID_KEY_F4: return KEY_F4;
        case HID_KEY_F5: return KEY_F5;
        case HID_KEY_F6: return KEY_F6;
        case HID_KEY_F7: return KEY_F7;
        case HID_KEY_F8: return KEY_F8;
        case HID_KEY_F9: return KEY_F9;
        case HID_KEY_F10: return KEY_F10;
        case HID_KEY_F11: return KEY_F11;
        case HID_KEY_F12: return KEY_F12;
        case HID_KEY_LEFT_ARROW: return KEY_ARROW_LEFT;
        case HID_KEY_RIGHT_ARROW: return KEY_ARROW_RIGHT;
        case HID_KEY_UP_ARROW: return KEY_ARROW_UP;
        case HID_KEY_DOWN_ARROW: return KEY_ARROW_DOWN;
        case HID_KEY_HOME: return KEY_HOME;
        case HID_KEY_END: return KEY_END;
        case HID_KEY_PAGE_UP: return KEY_PAGE_UP;
        case HID_KEY_PAGE_DOWN: return KEY_PAGE_DOWN;
        case HID_KEY_DELETE: return KEY_DELETE;
        case HID_KEY_INSERT: return KEY_INSERT;
        default: return KEY_UNKNOWN;
    }
}

// Process USB keyboard report (called from HID interrupt callback)
void usb_process_keyboard_report(hid_device_t *hid) {
    if (!hid) return;

    hid_keyboard_report_t *report = &hid->report.keyboard;

    // Process new key presses
    for (int j = 0; j < 6; j++) {
        uint8_t key = report->keys[j];
        if (key == 0) continue;

        bool is_new = true;
        for (int k = 0; k < 6; k++) {
            if (last_usb_kbd_report.keys[k] == key) { is_new = false; break; }
        }

        if (is_new) {
            KeyCode keycode = usb_hid_to_keycode(key);
            char c = hid_keycode_to_ascii(key, report->modifiers);

            Event event = event_create_key_pressed(
                keycode,
                key,
                c,
                usb_get_event_modifiers(report->modifiers),
                false
            );

            event_push(&event);
            devfs_input_push_event(&event);
        }
    }

    // Process key releases
    for (int j = 0; j < 6; j++) {
        uint8_t old_key = last_usb_kbd_report.keys[j];
        if (old_key == 0) continue;

        bool still_pressed = false;
        for (int k = 0; k < 6; k++) {
            if (report->keys[k] == old_key) { still_pressed = true; break; }
        }

        if (!still_pressed) {
            KeyCode keycode = usb_hid_to_keycode(old_key);

            Event event = event_create_key_released(
                keycode,
                old_key,
                usb_get_event_modifiers(report->modifiers),
                false
            );

            event_push(&event);
            devfs_input_push_event(&event);
        }
    }

    memcpy(&last_usb_kbd_report, report, sizeof(hid_keyboard_report_t));
}

// Legacy polling function (USB is interrupt-driven)
void usb_input_poll(void) {
    // no-op
}
