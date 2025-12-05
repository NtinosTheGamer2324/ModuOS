#include "moduos/kernel/events/events.h"
#include "moduos/drivers/graphics/VGA.h"
#include "moduos/drivers/ps2/ps2.h"
#include "moduos/kernel/shell/zenith4.h"
#include "moduos/kernel/events/input.h"
#include <stddef.h>
#include <stdbool.h>
#include "moduos/kernel/memory/string.h"

#define INPUT_BUFFER_SIZE 256

// Shell-specific input function that handles both regular input and arrow keys
char* shell_input(void) {
    static char input_buffer[INPUT_BUFFER_SIZE];
    size_t input_index = 0;
    bool input_ready = false;
    
    input_buffer[0] = '\0';
    
    // Clear event queue to start fresh
    event_clear();
    
    while (!input_ready) {
        Event event = event_wait(); // Wait for keyboard event
        
        if (event.type == EVENT_KEY_PRESSED) {
            // Handle arrow keys for history navigation
            if (event.data.keyboard.keycode == KEY_ARROW_UP) {
                const char* cmd = get_history_prev();
                if (cmd) {
                    // Clear current line visually
                    while (input_index > 0) {
                        input_index--;
                        VGA_Backspace();
                    }
                    
                    // Write history command
                    size_t len = strlen(cmd);
                    if (len >= INPUT_BUFFER_SIZE) {
                        len = INPUT_BUFFER_SIZE - 1;
                    }
                    
                    for (size_t i = 0; i < len; i++) {
                        input_buffer[input_index++] = cmd[i];
                        VGA_WriteChar(cmd[i]);
                    }
                }
                continue;
            }
            
            if (event.data.keyboard.keycode == KEY_ARROW_DOWN) {
                const char* cmd = get_history_next();
                if (cmd) {
                    // Clear current line visually
                    while (input_index > 0) {
                        input_index--;
                        VGA_Backspace();
                    }
                    
                    // Write history command (might be empty string to clear)
                    size_t len = strlen(cmd);
                    if (len >= INPUT_BUFFER_SIZE) {
                        len = INPUT_BUFFER_SIZE - 1;
                    }
                    
                    for (size_t i = 0; i < len; i++) {
                        input_buffer[input_index++] = cmd[i];
                        VGA_WriteChar(cmd[i]);
                    }
                }
                continue;
            }
            
            // Handle backspace
            if (event.data.keyboard.keycode == KEY_BACKSPACE) {
                if (input_index > 0) {
                    input_index--;
                    input_buffer[input_index] = '\0';
                    VGA_Backspace(); // only remove what we wrote
                }
                continue;
            }
            
            // Handle enter
            if (event.data.keyboard.keycode == KEY_ENTER) {
                VGA_WriteChar('\n');
                input_buffer[input_index] = '\0';
                input_ready = true;
                continue;
            }
            
            // Handle tab (convert to spaces)
            if (event.data.keyboard.keycode == KEY_TAB) {
                if (input_index < INPUT_BUFFER_SIZE - 2) {
                    input_buffer[input_index++] = ' ';
                    input_buffer[input_index++] = ' ';
                    VGA_Write("  ");
                }
                continue;
            }
            
            // Handle printable characters (record only — PS/2 already echoed)
            if (event.data.keyboard.ascii && input_index < INPUT_BUFFER_SIZE - 1) {
                input_buffer[input_index++] = event.data.keyboard.ascii;
                // Removed VGA_WriteChar() to avoid double print
            }
        }
    }
    
    return input_buffer;
}
