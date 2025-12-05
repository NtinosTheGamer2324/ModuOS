#pragma once

#include <stdint.h>
#include <stdbool.h>

// Event types
typedef enum {
    EVENT_NONE = 0,
    EVENT_KEY_PRESSED,
    EVENT_KEY_RELEASED,
    EVENT_CHAR_INPUT,
    EVENT_MOUSE_MOVE,      // For future expansion
    EVENT_MOUSE_BUTTON,    // For future expansion
} EventType;

// Key codes for special keys
typedef enum {
    KEY_UNKNOWN = 0,
    KEY_ESCAPE,
    KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6,
    KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
    KEY_BACKSPACE,
    KEY_TAB,
    KEY_ENTER,
    KEY_LEFT_SHIFT,
    KEY_RIGHT_SHIFT,
    KEY_LEFT_CTRL,
    KEY_RIGHT_CTRL,
    KEY_LEFT_ALT,
    KEY_RIGHT_ALT,
    KEY_CAPS_LOCK,
    KEY_NUM_LOCK,
    KEY_SCROLL_LOCK,
    KEY_SPACE,
    KEY_INSERT,
    KEY_DELETE,
    KEY_HOME,
    KEY_END,
    KEY_PAGE_UP,
    KEY_PAGE_DOWN,
    KEY_ARROW_UP,
    KEY_ARROW_DOWN,
    KEY_ARROW_LEFT,
    KEY_ARROW_RIGHT,
    // Add more as needed
} KeyCode;

// Modifier flags
typedef enum {
    MOD_NONE = 0,
    MOD_SHIFT = (1 << 0),
    MOD_CTRL = (1 << 1),
    MOD_ALT = (1 << 2),
    MOD_CAPS = (1 << 3),
    MOD_NUM = (1 << 4),
} ModifierFlags;

// Keyboard event data
typedef struct {
    KeyCode keycode;        // Virtual key code
    uint8_t scancode;       // Raw scancode
    char ascii;             // ASCII character (0 if not printable)
    uint8_t modifiers;      // Modifier flags (shift, ctrl, alt, etc.)
    bool is_extended;       // Was this an extended scancode?
} KeyboardEventData;

// Mouse event data (for future use)
typedef struct {
    int16_t x;
    int16_t y;
    int16_t delta_x;
    int16_t delta_y;
    uint8_t buttons;        // Bit flags for buttons
} MouseEventData;

// Generic event structure
typedef struct {
    EventType type;
    uint64_t timestamp;     // Optional: system tick count
    union {
        KeyboardEventData keyboard;
        MouseEventData mouse;
    } data;
} Event;

// Event queue configuration
#define EVENT_QUEUE_SIZE 64

// Event queue structure
typedef struct {
    Event events[EVENT_QUEUE_SIZE];
    volatile uint32_t read_index;
    volatile uint32_t write_index;
    volatile uint32_t count;
} EventQueue;

// Global event queue
extern EventQueue g_event_queue;

// Initialize the event system
void event_init(void);

// Push an event to the queue (called by drivers/IRQ handlers)
// Returns true on success, false if queue is full
bool event_push(Event *event);

// Get the next event from the queue (non-blocking)
// Returns true if event was retrieved, false if queue is empty
bool event_poll(Event *out_event);

// Wait for the next event (blocking)
// Returns the next event when available
Event event_wait(void);

// Check if there are pending events
bool event_pending(void);

// Clear all pending events
void event_clear(void);

// Helper functions to create specific events
Event event_create_key_pressed(KeyCode keycode, uint8_t scancode, char ascii, uint8_t modifiers, bool extended);
Event event_create_key_released(KeyCode keycode, uint8_t scancode, uint8_t modifiers, bool extended);
Event event_create_char_input(char c);

// Get current modifier state
uint8_t event_get_modifiers(void);
void event_update_modifiers(KeyCode keycode, bool pressed);

// Helper to convert scancode to keycode
KeyCode scancode_to_keycode(uint8_t scancode, bool extended);