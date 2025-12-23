#include "moduos/kernel/io/io.h"
#include "moduos/drivers/input/ps2/ports.h"
#include "moduos/arch/AMD64/interrupts/pic.h"
#include "moduos/drivers/input/ps2/scancodes.h"
#include <stdbool.h>
#include "moduos/drivers/graphics/VGA.h"
#include <stddef.h>
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/COM/com.h"
#include "moduos/fs/devfs.h"
#include "moduos/kernel/events/events.h"
#include "moduos/drivers/input/ps2/ps2.h"


#define TIMEOUT_LIMIT 1000000


// Wait until output buffer has data or timeout
static int ps2_wait_output(uint8_t *data) {
    int timeout = 0;
    while (!(inb(PS2_COMMAND_PORT) & PS2_STATUS_OUTPUT_BUFFER) && timeout < TIMEOUT_LIMIT)
        timeout++;
    if (timeout == TIMEOUT_LIMIT) return -1;
    *data = inb(PS2_DATA_PORT);
    return 0;
}

// Wait until input buffer is free or timeout
static int ps2_wait_input(void) {
    int timeout = 0;
    while ((inb(PS2_COMMAND_PORT) & PS2_STATUS_INPUT_BUFFER) && timeout < TIMEOUT_LIMIT)
        timeout++;
    return (timeout == TIMEOUT_LIMIT) ? -1 : 0;
}

static bool is_running_in_vm() {
    uint32_t eax, ebx, ecx, edx;
    char hv_vendor[13] = {0};

    // Check if hypervisor present (bit 31 of ECX in CPUID leaf 1)
    eax = 1;
    __asm__ volatile("cpuid"
                     : "=c"(ecx)
                     : "a"(eax)
                     : "ebx", "edx");

    if (!((ecx >> 31) & 1)) {
        return false; // Not running in a VM
    }

    // Get hypervisor vendor string (CPUID leaf 0x40000000)
    eax = 0x40000000;
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(eax));

    memcpy(&hv_vendor[0], &ebx, 4);
    memcpy(&hv_vendor[4], &ecx, 4);
    memcpy(&hv_vendor[8], &edx, 4);
    hv_vendor[12] = '\0';

    // Known VM vendors
    if (strcmp(hv_vendor, "KVMKVMKVM") == 0 ||    // KVM/QEMU
        strcmp(hv_vendor, "VBoxVBoxVBox") == 0 || // VirtualBox
        strcmp(hv_vendor, "QEMU") == 0 ||         // plain QEMU
        strcmp(hv_vendor, "VMwareVMware") == 0) { // VMware
        return true;
    }

    return false;
}

int ps2_init() {
    uint8_t config;
    uint8_t response;
    bool vm = is_running_in_vm(); // detect VM

    // Initialize event system
    event_init();

    // --- Controller init ---

    if (ps2_wait_input() < 0 && !vm) return -1;
    outb(PS2_COMMAND_PORT, PS2_CMD_DISABLE_PORT1);

    if (ps2_wait_input() < 0 && !vm) return -1;
    outb(PS2_COMMAND_PORT, PS2_CMD_READ_CONFIG);

    if (ps2_wait_output(&config) < 0 && !vm) config = 0;

    /* PS/2 controller config byte:
     *  bit0 = IRQ1 enable (port1/keyboard)
     *  bit1 = IRQ12 enable (port2/mouse)
     *  bit6 = translation
     */
    config |= (1 << 0);    // Enable IRQ1 (keyboard)
    config &= ~(1 << 1);   // Disable IRQ12 (mouse)
    config &= ~(1 << 6);   // Disable translation

    if (ps2_wait_input() < 0 && !vm) return -1;
    outb(PS2_COMMAND_PORT, PS2_CMD_WRITE_CONFIG);

    if (ps2_wait_input() < 0 && !vm) return -1;
    outb(PS2_DATA_PORT, config);

    // Flush pending output
    while (inb(PS2_COMMAND_PORT) & PS2_STATUS_OUTPUT_BUFFER) {
        (void)inb(PS2_DATA_PORT);
    }

    if (ps2_wait_input() < 0 && !vm) return -1;
    outb(PS2_COMMAND_PORT, PS2_CMD_ENABLE_PORT1);

    // --- Device presence check ---

    // Flush again
    while (inb(PS2_COMMAND_PORT) & PS2_STATUS_OUTPUT_BUFFER) {
        (void)inb(PS2_DATA_PORT);
    }

    if (ps2_wait_input() >= 0 || vm) {
        outb(PS2_DATA_PORT, 0xFF); // Reset keyboard

        if (ps2_wait_output(&response) == 0) {
            if (response == 0xFA) {
                if (ps2_wait_output(&response) == 0 && response == 0xAA) {
                    /* ok */
                }
                /*
                 * IMPORTANT (QEMU/strict PS/2): explicitly enable keyboard scanning.
                 * Some emulators won't send scancodes/IRQ1 until 0xF4 is issued.
                 */
                if (ps2_wait_input() >= 0 || vm) {
                    outb(PS2_DATA_PORT, 0xF4); // Enable scanning
                    if (ps2_wait_output(&response) == 0) {
                        if (response != 0xFA && !vm) return -1;
                    } else if (!vm) {
                        return -1;
                    }
                }
                return 0;
            } else if (response == 0xAA) {
                return 0; // self-test only
            } else if (vm) {
                return 0; // unexpected byte in VM → tolerate
            }
            return -1; // unexpected byte on real hardware
        }

        // No response → tolerate in VM
        if (vm) return 0;
    }

    return 0; // success by default
}

// -------------------- KEYBOARD STATE & BUFFER --------------------

static bool extended = false;
static bool break_code = false;
static bool shifted = false;

/* Legacy line-discipline code removed.
 * Use input() from src/drivers/input/input.c (reads from $/dev/input/event0).
 */

#if 0
// Made non-static so USB input can share the same buffer
char input_buffer[INPUT_BUFFER_SIZE];
size_t input_index = 0;
bool input_ready = false;

char* input() {
    input_index = 0;
    input_ready = false;

    while (!input_ready) {
        asm volatile("hlt");  // Sleep until interrupt fires
    }

    input_buffer[input_index] = '\0';  // Null terminate
    return input_buffer;
}

// Helper function to replace the current input line with history
void replace_input_line(const char* new_text) {
    // Clear current input buffer visually
    while (input_index > 0) {
        input_index--;
        VGA_Backspace();
    }
    
    // Write new text
    size_t len = strlen(new_text);
    if (len >= INPUT_BUFFER_SIZE) {
        len = INPUT_BUFFER_SIZE - 1;
    }
    
    for (size_t i = 0; i < len; i++) {
        input_buffer[input_index++] = new_text[i];
        VGA_WriteChar(new_text[i]);
    }
}
#endif

char scancode_to_char_extended(uint8_t scancode) {
    if (scancode >= 256) return 0;
    return shifted ? shifted_extended_scancode_map[scancode] : extended_scancode_map[scancode];
}

char scancode_to_char(uint8_t scancode) {
    if (scancode >= 128) return 0;
    return shifted ? shifted_scancode_map[scancode] : scancode_map[scancode];
}

void keyboard_irq_handler() {
    uint8_t scancode = inb(PS2_DATA_PORT);

    // Handle extended scancode prefix (0xE0)
    if (scancode == 0xE0) {
        extended = true;
        pic_send_eoi(1);
        return;
    }

    // Handle break code prefix (0xF0)
    if (scancode == 0xF0) {
        break_code = true;
        pic_send_eoi(1);
        return;
    }

    // --- Create and push keyboard events ---
    
    KeyCode keycode = scancode_to_keycode(scancode, extended);
    uint8_t modifiers = event_get_modifiers();
    
    // Create event based on whether it's a make or break code
    Event event;
    if (!break_code) {
        // Key pressed
        char c = extended ? scancode_to_char_extended(scancode) : scancode_to_char(scancode);
        event = event_create_key_pressed(keycode, scancode, c, modifiers, extended);
        
        // Update modifier state
        event_update_modifiers(keycode, true);
    } else {
        // Key released
        event = event_create_key_released(keycode, scancode, modifiers, extended);
        
        // Update modifier state
        event_update_modifiers(keycode, false);
    }
    
    // Push event to queue
    event_push(&event);

    // Mirror to DEVFS input devices ($/dev/input/event0 and kbd0)
    devfs_input_push_event(&event);

    // --- Special Key Handling (only on make codes) ---

    // Navigation keys are delivered as normal KEY events to $/dev/input/event0.
    // Shell history browsing is handled by the input() line discipline.

    // SHIFT (left = 0x12, right = 0x59)
    if (!extended && (scancode == 0x12 || scancode == 0x59)) {
        shifted = !break_code;
        break_code = false;
        extended = false;
        pic_send_eoi(1);
        return;
    }

    // Ignore break codes after being flagged
    if (break_code) {
        break_code = false;
        extended = false;
        pic_send_eoi(1);
        return;
    }

    // Convert to char (if possible)
    char c = extended ? scancode_to_char_extended(scancode) : scancode_to_char(scancode);

    // Character/line editing is handled in input subsystem (via DEVFS).
    (void)c;
    extended = false;
    pic_send_eoi(1);
}

// --------- New function to get current PS/2 scancode set ---------
uint8_t ps2_get_scancode_set(void) {
    uint8_t response;

    // Wait for input buffer empty before sending command
    if (ps2_wait_input() < 0) return 0;

    // Send 0xF0 to keyboard (Get/Set scancode set)
    outb(PS2_DATA_PORT, 0xF0);

    // Wait for ACK (0xFA)
    if (ps2_wait_output(&response) < 0 || response != 0xFA) return 0;

    // Wait input buffer empty before sending parameter 0 (query)
    if (ps2_wait_input() < 0) return 0;

    // Send 0x00 to query current set
    outb(PS2_DATA_PORT, 0x00);

    // Wait for response (scancode set number)
    if (ps2_wait_output(&response) < 0) return 0;

    return response;
}

// --------- Helper to print the scancode set ---------
void print_scancode_set(void) {
    uint8_t set = ps2_get_scancode_set();

    char buf[32];
    const char* msg = "Current PS/2 Scancode Set: ";

    VGA_Write(msg);

    // Convert number to string (simple)
    buf[0] = '0' + (set % 10);
    buf[1] = '\n';
    buf[2] = '\0';

    VGA_Write(buf);
}