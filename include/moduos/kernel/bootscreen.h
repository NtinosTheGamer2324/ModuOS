#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Root directory for bootscreen images on the boot filesystem.
#define BOOTSCREEN_DIR "/ModuOS/shared/boot/bimg/"

// Determine bootscreen BMP filename (basename only) based on SMBIOS vendor.
// Returns pointer to a static string (never NULL).
const char *bootscreen_pick_bmp_basename(void *mb2);

// Show an early boot splash using a built-in (burn-in) image.
// This does not require the boot filesystem to be mounted.
// Returns 0 on success, <0 on failure.
int bootscreen_show_early(void);

// Try to load and display the bootscreen BMP (centered) if in graphics mode.
// Returns 0 on success, <0 on failure.
int bootscreen_show(void *mb2);

// Bootsplash overlay: when enabled, redraw will paint the cached logo on top of the framebuffer.
void bootscreen_overlay_set_enabled(int enabled);
void bootscreen_overlay_redraw(void);

#ifdef __cplusplus
}
#endif
