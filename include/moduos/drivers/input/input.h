#ifndef MODUOS_DRIVERS_INPUT_H
#define MODUOS_DRIVERS_INPUT_H

#include <stdint.h>
#include "moduos/kernel/events/events.h"

// Initialize the input subsystem
// This will initialize all available input devices (PS/2, USB, etc.)
int input_init(void);

// Get a line of input from the keyboard (blocks until Enter is pressed)
char* input(void);

// IRQ handler - called by the active keyboard driver
void keyboard_irq_handler(void);

// Forward declaration for HID
struct hid_device;

// Process USB keyboard report (called from HID driver)
void usb_process_keyboard_report(struct hid_device *hid);

// Legacy polling function (now a stub - USB is interrupt-driven)
void usb_input_poll(void);

// USB HID helper functions
KeyCode usb_hid_to_keycode(uint8_t hid_code);
uint8_t usb_get_event_modifiers(uint8_t hid_mods);
void usb_handle_key_press(uint8_t hid_key, uint8_t modifiers);

#endif // MODUOS_DRIVERS_INPUT_H
