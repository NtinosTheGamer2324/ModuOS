#include "moduos/drivers/input/input.h"
#include "moduos/drivers/input/ps2/ps2.h"
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
#include <stdbool.h>

// Input device tracking
static bool ps2_available = false;
static bool usb_available = false;

// USB HID state tracking
static hid_keyboard_report_t last_usb_kbd_report = {0};
static bool usb_shift_pressed = false;

// Initialize all input subsystems
int input_init(void) {
    COM_LOG_INFO(COM1_PORT, "Initializing input subsystem");
    
    // Try PS/2 first
    COM_LOG_INFO(COM1_PORT, "Initializing PS/2 input");
    if (ps2_init() != 0) {
        COM_LOG_WARN(COM1_PORT, "PS/2 did not respond! (This happens on some VMs)");
        ps2_available = false;
    } else {
        COM_LOG_OK(COM1_PORT, "PS/2 initialized");
        ps2_available = true;
    }
    
    // Initialize USB input
    COM_LOG_INFO(COM1_PORT, "Initializing USB input");
    
    // Initialize USB subsystem (this will automatically probe and initialize controllers)
    usb_init();
    
    // Initialize HID driver
    hid_init();
    
    usb_available = true;
    COM_LOG_OK(COM1_PORT, "USB input initialized");
    
    COM_LOG_OK(COM1_PORT, "Input subsystem initialized");
    return 0;
}

// Process USB keyboard report (called from HID interrupt callback)
void usb_process_keyboard_report(hid_device_t *hid) {
    if (!hid || !usb_available) return;
    
    hid_keyboard_report_t *report = &hid->report.keyboard;
    
    // Track shift state
    usb_shift_pressed = (report->modifiers & (HID_MOD_LEFT_SHIFT | HID_MOD_RIGHT_SHIFT)) != 0;
    
    // Process new key presses
    for (int j = 0; j < 6; j++) {
        uint8_t key = report->keys[j];
        if (key == 0) continue;
        
        // Check if this is a new key press
        bool is_new = true;
        for (int k = 0; k < 6; k++) {
            if (last_usb_kbd_report.keys[k] == key) {
                is_new = false;
                break;
            }
        }
        
        if (is_new) {
            // Create keyboard event
            KeyCode keycode = usb_hid_to_keycode(key);
            char c = hid_keycode_to_ascii(key, report->modifiers);
            
            Event event = event_create_key_pressed(
                keycode, 
                key,  // Use HID code as scancode
                c, 
                usb_get_event_modifiers(report->modifiers),
                false  // Not extended
            );
            
            event_push(&event);
            
            // Also feed to input buffer for compatibility
            usb_handle_key_press(key, report->modifiers);
        }
    }
    
    // Process key releases
    for (int j = 0; j < 6; j++) {
        uint8_t old_key = last_usb_kbd_report.keys[j];
        if (old_key == 0) continue;
        
        // Check if this key was released
        bool still_pressed = false;
        for (int k = 0; k < 6; k++) {
            if (report->keys[k] == old_key) {
                still_pressed = true;
                break;
            }
        }
        
        if (!still_pressed) {
            // Create key release event
            KeyCode keycode = usb_hid_to_keycode(old_key);
            
            Event event = event_create_key_released(
                keycode,
                old_key,
                usb_get_event_modifiers(report->modifiers),
                false
            );
            
            event_push(&event);
        }
    }
    
    // Save current report
    memcpy(&last_usb_kbd_report, report, sizeof(hid_keyboard_report_t));
}

// Legacy polling function (now just a stub for compatibility)
void usb_input_poll(void) {
    // No longer needed - USB input is now interrupt-driven!
    // This function is kept for API compatibility but does nothing.
}

// Convert USB HID keycode to internal KeyCode enum
KeyCode usb_hid_to_keycode(uint8_t hid_code) {
    // Letters and numbers are not in KeyCode enum, return KEY_UNKNOWN for them
    // (they're handled as ASCII characters instead)
    
    // Special keys
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

// Convert USB HID modifiers to event system modifiers
uint8_t usb_get_event_modifiers(uint8_t hid_mods) {
    uint8_t mods = 0;
    
    if (hid_mods & (HID_MOD_LEFT_SHIFT | HID_MOD_RIGHT_SHIFT))
        mods |= MOD_SHIFT;
    if (hid_mods & (HID_MOD_LEFT_CTRL | HID_MOD_RIGHT_CTRL))
        mods |= MOD_CTRL;
    if (hid_mods & (HID_MOD_LEFT_ALT | HID_MOD_RIGHT_ALT))
        mods |= MOD_ALT;
    // Note: MOD_SUPER doesn't exist in ModifierFlags, skip GUI keys
    
    return mods;
}

// Handle USB key press for input buffer (compatibility with PS/2 input)
void usb_handle_key_press(uint8_t hid_key, uint8_t modifiers) {
    // Use shared input buffer from ps2.h
    char c = hid_keycode_to_ascii(hid_key, modifiers);
    
    // Handle special keys
    if (hid_key == HID_KEY_UP_ARROW) {
        extern void up_arrow_pressed(void);
        up_arrow_pressed();
        return;
    }
    
    if (hid_key == HID_KEY_DOWN_ARROW) {
        extern void down_arrow_pressed(void);
        down_arrow_pressed();
        return;
    }
    
    if (hid_key == HID_KEY_TAB) {
        if (input_index < 256 - 1) {
            input_buffer[input_index++] = ' ';
            input_buffer[input_index++] = ' ';
            VGA_Write("  ");
        }
        return;
    }
    
    if (hid_key == HID_KEY_BACKSPACE) {
        if (input_index > 0) {
            input_index--;
            input_buffer[input_index] = '\0';
            VGA_Backspace();
        }
        return;
    }
    
    if (hid_key == HID_KEY_ENTER) {
        VGA_WriteChar('\n');
        input_ready = true;
        return;
    }
    
    // Regular character input
    if (c) {
        if (input_index < 256 - 1) {
            input_buffer[input_index++] = c;
            VGA_WriteChar(c);
        }
    }
}
