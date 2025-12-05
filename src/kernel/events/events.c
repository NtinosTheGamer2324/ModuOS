#include "moduos/kernel/events/events.h"
#include <stddef.h>
#include "moduos/kernel/memory/string.h"

// Global event queue
EventQueue g_event_queue;
static uint8_t g_modifier_state = MOD_NONE;

// Current modifier state
static volatile uint8_t current_modifiers = MOD_NONE;

void event_init(void) {
    memset(&g_event_queue, 0, sizeof(EventQueue));
    g_event_queue.read_index = 0;
    g_event_queue.write_index = 0;
    g_event_queue.count = 0;
    current_modifiers = MOD_NONE;
}

bool event_push(Event *event) {
    if (!event) return false;
    
    // Check if queue is full
    if (g_event_queue.count >= EVENT_QUEUE_SIZE) {
        return false; // Queue full, drop event
    }
    
    // Add event to queue
    g_event_queue.events[g_event_queue.write_index] = *event;
    g_event_queue.write_index = (g_event_queue.write_index + 1) % EVENT_QUEUE_SIZE;
    g_event_queue.count++;
    
    return true;
}

bool event_poll(Event *out_event) {
    if (!out_event) return false;
    
    // Check if queue is empty
    if (g_event_queue.count == 0) {
        return false;
    }
    
    // Get event from queue
    *out_event = g_event_queue.events[g_event_queue.read_index];
    g_event_queue.read_index = (g_event_queue.read_index + 1) % EVENT_QUEUE_SIZE;
    g_event_queue.count--;
    
    return true;
}

Event event_wait(void) {
    Event event;
    
    // Wait until an event is available
    while (!event_poll(&event)) {
        asm volatile("hlt"); // Sleep until interrupt
    }
    
    return event;
}

bool event_pending(void) {
    return g_event_queue.count > 0;
}

void event_clear(void) {
    g_event_queue.read_index = 0;
    g_event_queue.write_index = 0;
    g_event_queue.count = 0;
}

Event event_create_key_pressed(KeyCode keycode, uint8_t scancode, char ascii, uint8_t modifiers, bool extended) {
    Event event;
    event.type = EVENT_KEY_PRESSED;
    event.timestamp = 0; // TODO: Add system tick counter
    event.data.keyboard.keycode = keycode;
    event.data.keyboard.scancode = scancode;
    event.data.keyboard.ascii = ascii;
    event.data.keyboard.modifiers = modifiers;
    event.data.keyboard.is_extended = extended;
    return event;
}

Event event_create_key_released(KeyCode keycode, uint8_t scancode, uint8_t modifiers, bool extended) {
    Event event;
    event.type = EVENT_KEY_RELEASED;
    event.timestamp = 0; // TODO: Add system tick counter
    event.data.keyboard.keycode = keycode;
    event.data.keyboard.scancode = scancode;
    event.data.keyboard.ascii = 0;
    event.data.keyboard.modifiers = modifiers;
    event.data.keyboard.is_extended = extended;
    return event;
}

Event event_create_char_input(char c) {
    Event event;
    event.type = EVENT_CHAR_INPUT;
    event.timestamp = 0;
    event.data.keyboard.keycode = KEY_UNKNOWN;
    event.data.keyboard.scancode = 0;
    event.data.keyboard.ascii = c;
    event.data.keyboard.modifiers = current_modifiers;
    event.data.keyboard.is_extended = false;
    return event;
}

void event_update_modifiers(KeyCode keycode, bool pressed) {
    switch (keycode) {
        case KEY_LEFT_SHIFT:
        case KEY_RIGHT_SHIFT:
            if (pressed) {
                g_modifier_state |= MOD_SHIFT;
            } else {
                g_modifier_state &= ~MOD_SHIFT;
            }
            break;
            
        case KEY_LEFT_CTRL:
        case KEY_RIGHT_CTRL:
            if (pressed) {
                g_modifier_state |= MOD_CTRL;
            } else {
                g_modifier_state &= ~MOD_CTRL;
            }
            break;
            
        case KEY_LEFT_ALT:
        case KEY_RIGHT_ALT:
            if (pressed) {
                g_modifier_state |= MOD_ALT;
            } else {
                g_modifier_state &= ~MOD_ALT;
            }
            break;
            
        case KEY_CAPS_LOCK:
            if (pressed) {
                g_modifier_state ^= MOD_CAPS;  // Toggle
            }
            break;
            
        case KEY_NUM_LOCK:
            if (pressed) {
                g_modifier_state ^= MOD_NUM;  // Toggle
            }
            break;
            
        default:
            break;
    }
}

uint8_t event_get_modifiers(void) {
    return g_modifier_state;
}

KeyCode scancode_to_keycode(uint8_t scancode, bool extended) {
    if (extended) {
        // Extended scancodes (0xE0 prefix)
        switch (scancode) {
            case 0x75: return KEY_ARROW_UP;
            case 0x72: return KEY_ARROW_DOWN;
            case 0x6B: return KEY_ARROW_LEFT;
            case 0x74: return KEY_ARROW_RIGHT;
            case 0x7D: return KEY_PAGE_UP;
            case 0x7A: return KEY_PAGE_DOWN;
            case 0x6C: return KEY_HOME;
            case 0x69: return KEY_END;
            case 0x70: return KEY_INSERT;
            case 0x71: return KEY_DELETE;
            case 0x14: return KEY_RIGHT_CTRL;
            case 0x11: return KEY_RIGHT_ALT;
            default: return KEY_UNKNOWN;
        }
    } else {
        // Standard scancodes (PS/2 Set 2)
        switch (scancode) {
            case 0x76: return KEY_ESCAPE;
            case 0x05: return KEY_F1;
            case 0x06: return KEY_F2;
            case 0x04: return KEY_F3;
            case 0x0C: return KEY_F4;
            case 0x03: return KEY_F5;
            case 0x0B: return KEY_F6;
            case 0x83: return KEY_F7;
            case 0x0A: return KEY_F8;
            case 0x01: return KEY_F9;
            case 0x09: return KEY_F10;
            case 0x78: return KEY_F11;
            case 0x07: return KEY_F12;
            case 0x66: return KEY_BACKSPACE;
            case 0x0D: return KEY_TAB;
            case 0x5A: return KEY_ENTER;
            case 0x12: return KEY_LEFT_SHIFT;
            case 0x59: return KEY_RIGHT_SHIFT;
            case 0x14: return KEY_LEFT_CTRL;
            case 0x11: return KEY_LEFT_ALT;
            case 0x58: return KEY_CAPS_LOCK;
            case 0x77: return KEY_NUM_LOCK;
            case 0x7E: return KEY_SCROLL_LOCK;
            case 0x29: return KEY_SPACE;
            default: return KEY_UNKNOWN;
        }
    }
}