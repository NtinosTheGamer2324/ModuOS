#ifndef KERNEL_DRIVER_PS2_H
#define KERNEL_DRIVER_PS2_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Shared input buffer (used by both PS/2 and USB input)
#define INPUT_BUFFER_SIZE 256
extern char input_buffer[INPUT_BUFFER_SIZE];
extern size_t input_index;
extern bool input_ready;

// Initialize PS/2 keyboard controller and keyboard device
// Returns 0 on success, -1 on failure (e.g. timeout)
int ps2_init(void);

// Print current scancode set
void print_scancode_set(void);

// Get current PS/2 scancode set
uint8_t ps2_get_scancode_set(void);

// IRQ handler function called on keyboard interrupt
void keyboard_irq_handler(void);

// Get input from keyboard (blocks until Enter is pressed)
char* input(void);

// Convert scancode to ASCII character
char scancode_to_char(uint8_t scancode);

// Convert extended scancode to ASCII character
char scancode_to_char_extended(uint8_t scancode);

// Replace the current input line with new text (for history navigation)
void replace_input_line(const char* new_text);

#endif // KERNEL_DRIVER_PS2_H
