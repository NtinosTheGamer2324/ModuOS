#pragma once

#include <stdint.h>

// Implemented by PS/2 mouse driver (or future USB mouse).
int32_t devfs_mouse_get_x(void);
int32_t devfs_mouse_get_y(void);
uint8_t devfs_mouse_get_buttons(void);

// Returns accumulated wheel delta since last call and resets it to 0.
int32_t devfs_mouse_take_wheel(void);
