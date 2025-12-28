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
#include "moduos/fs/fd.h" /* O_RDONLY/O_NONBLOCK */

#include <stdbool.h>

/*
 * Optional hooks for shells: if a kernel shell wants command history, it can
 * provide these symbols. We keep weak no-op defaults so generic input() stays usable.
 */
__attribute__((weak)) void up_arrow_pressed(void) {}
__attribute__((weak)) void down_arrow_pressed(void) {}

// USB HID state tracking
static hid_keyboard_report_t last_usb_kbd_report = {0};

// ---------------- Line discipline (POSIX-ish) ----------------

#define INPUT_LINE_MAX 256
#define VGA_TEXT_WIDTH 80

static char g_line_buf[INPUT_LINE_MAX];
static size_t g_line_len = 0;     /* number of valid chars in buffer */
static size_t g_line_cursor = 0;  /* cursor position within buffer (0..len) */
static size_t g_line_prev_len = 0;/* previous rendered length (for clearing) */
static int g_line_start_row = 0;
static int g_line_start_col = 0;

static void vga_set_cursor_from_line_pos(size_t pos) {
    /* Map (start_row,start_col)+pos into a text cursor position with wrapping. */
    int row = g_line_start_row;
    int col = g_line_start_col + (int)pos;
    if (col >= VGA_TEXT_WIDTH) {
        row += col / VGA_TEXT_WIDTH;
        col = col % VGA_TEXT_WIDTH;
    }
    VGA_SetCursorPosition(row, col);
}

static void render_line(void) {
    /* Re-render full line from the start and restore cursor position. */
    vga_set_cursor_from_line_pos(0);

    VGA_WriteN(g_line_buf, g_line_len);

    /* Clear leftovers from previous render */
    if (g_line_prev_len > g_line_len) {
        for (size_t i = g_line_len; i < g_line_prev_len; i++) {
            VGA_WriteChar(' ');
        }
    }

    g_line_prev_len = g_line_len;
    vga_set_cursor_from_line_pos(g_line_cursor);
}

static void line_set_from_text(const char *new_text) {
    if (!new_text) new_text = "";
    size_t len = strlen(new_text);
    if (len >= INPUT_LINE_MAX) len = INPUT_LINE_MAX - 1;

    memcpy(g_line_buf, new_text, len);
    g_line_len = len;
    g_line_buf[g_line_len] = 0;
    g_line_cursor = g_line_len;
    render_line();
}

void replace_input_line(const char* new_text) {
    line_set_from_text(new_text);
}

// Drain $/dev/input/event0 (nonblocking). This prevents keystrokes from being replayed
// by other consumers when one program reads from kbd0 (raw chars).
static void input_flush_events(void) {
    void *eh = devfs_open_path("input/event0", O_RDONLY | O_NONBLOCK);
    if (!eh) {
        // fallback for legacy flat DEVFS (should not normally happen)
        eh = devfs_open("event0", O_RDONLY | O_NONBLOCK);
    }
    if (!eh) return;

    for (;;) {
        Event e;
        ssize_t n = devfs_read(eh, &e, sizeof(e));
        if (n != (ssize_t)sizeof(e)) break;
    }

    devfs_close(eh);
}

// Read a line from $/dev/input/kbd0, echoing to VGA (libc-like behavior).
// NOTE: This intentionally does NOT implement history arrows; those are structured events.
// NOTE: Multi-TTY is not implemented. This function uses a single global line buffer and
// writes directly to the global VGA console. Supporting multiple TTYs would require per-tty
// line discipline + a virtual console layer + input focus switching.
char* input(void) {
    /*
     * Ensure interrupts are enabled while we wait for input.
     * Some call paths may enter with IF=0; without this, blocking reads can appear as "keyboard dead".
     */
    __asm__ volatile("sti" ::: "memory");

    /* record where the prompt left the cursor so we can do in-line editing */
    VGA_GetCursorPosition(&g_line_start_row, &g_line_start_col);

    g_line_len = 0;
    g_line_cursor = 0;
    g_line_prev_len = 0;
    g_line_buf[0] = 0;

    void *h = devfs_open_path("input/kbd0", O_RDONLY);
    if (!h) {
        // fallback for legacy flat DEVFS
        h = devfs_open("kbd0", O_RDONLY);
    }
    if (!h) return g_line_buf;

    for (;;) {
        char c;
        ssize_t n = devfs_read(h, &c, 1);
        if (n != 1) continue;

        if (c == '\r') continue;

        /* ANSI/VT100 escape sequences from kbd0 (e.g. arrows: ESC [ A/B). */
        if ((unsigned char)c == 0x1b) {
            char b1 = 0, b2 = 0;
            if (devfs_read(h, &b1, 1) == 1 && devfs_read(h, &b2, 1) == 1) {
                if (b1 == '[') {
                    if (b2 == 'A') { up_arrow_pressed(); continue; }
                    if (b2 == 'B') { down_arrow_pressed(); continue; }
                    if (b2 == 'C') { /* Right */ if (g_line_cursor < g_line_len) { g_line_cursor++; render_line(); } continue; }
                    if (b2 == 'D') { /* Left */  if (g_line_cursor > 0) { g_line_cursor--; render_line(); } continue; }
                    if (b2 == 'H') { /* Home */  if (g_line_cursor != 0) { g_line_cursor = 0; render_line(); } continue; }
                    if (b2 == 'F') { /* End */   if (g_line_cursor != g_line_len) { g_line_cursor = g_line_len; render_line(); } continue; }

                    /* Tilde sequences: ESC [ <n> ~ */
                    if (b2 >= '0' && b2 <= '9') {
                        char b3 = 0;
                        if (devfs_read(h, &b3, 1) == 1 && b3 == '~') {
                            if (b2 == '3') {
                                /* Delete key: delete char at cursor */
                                if (g_line_cursor < g_line_len) {
                                    memmove(&g_line_buf[g_line_cursor],
                                            &g_line_buf[g_line_cursor + 1],
                                            g_line_len - g_line_cursor - 1);
                                    g_line_len--;
                                    g_line_buf[g_line_len] = 0;
                                    render_line();
                                }
                                continue;
                            }
                            /* Insert/PgUp/PgDn/etc: ignore for now */
                        }
                    }
                }
                /* Unknown escape: ignore. */
            }
            continue;
        }

        if (c == '\n') {
            g_line_buf[g_line_len] = 0;
            VGA_WriteChar('\n');
            break;
        }

        if ((c == '\b' || c == 127)) {
            if (g_line_cursor > 0) {
                /* delete char left of cursor */
                memmove(&g_line_buf[g_line_cursor - 1],
                        &g_line_buf[g_line_cursor],
                        g_line_len - g_line_cursor);
                g_line_len--;
                g_line_cursor--;
                g_line_buf[g_line_len] = 0;
                render_line();
            }
            continue;
        }

        if (c >= 32 && c < 127) {
            if (g_line_len + 1 < INPUT_LINE_MAX) {
                /* insert at cursor */
                memmove(&g_line_buf[g_line_cursor + 1],
                        &g_line_buf[g_line_cursor],
                        g_line_len - g_line_cursor);
                g_line_buf[g_line_cursor] = c;
                g_line_len++;
                g_line_cursor++;
                g_line_buf[g_line_len] = 0;
                render_line();
            }
            continue;
        }
    }

    devfs_close(h);

    // Prevent the same typing from being replayed later by event0 consumers.
    input_flush_events();

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
    //COM_LOG_INFO(COM1_PORT, "Initializing USB input");
    //usb_init();
    //hid_init();
    //COM_LOG_OK(COM1_PORT, "USB input initialized");

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
