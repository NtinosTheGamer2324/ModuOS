#ifndef KERNEL_VGA_H
#define KERNEL_VGA_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "moduos/drivers/graphics/framebuffer.h"

// VGA framebuffer mode
framebuffer_mode_t VGA_GetFrameBufferMode(void);

// Set framebuffer for GRAPHICS mode (VBE/future). No-op in TEXT mode.
void VGA_SetFrameBuffer(const framebuffer_t *fb);

// Get current framebuffer descriptor. Returns 0 on success, -1 if not in graphics mode.
int VGA_GetFrameBuffer(framebuffer_t *out);

// Clear framebuffer for GRAPHICS mode. No-op in TEXT mode.
void VGA_ClearFrameBuffer(uint32_t color);

/* Basic VGA output functions */
void VGA_WriteChar(char c);
void VGA_Write(const char* str);
void VGA_WriteN(const char *buf, size_t count);
void VGA_Clear();
void VGA_Backspace();
void VGA_Writef(const char* fmt, ...);

/*
 * Write text at a fixed text-cell position without affecting the current cursor.
 * row/col are in character cells (not pixels). Works in both TEXT and GRAPHICS mode.
 */
void VGA_WriteTextAtPosition(int row, int col, const char *str);

/* Scrolling functions */
void VGA_ScrollUp(int lines);      // Scroll up to view previous content
void VGA_ScrollDown(int lines);    // Scroll down to view newer content
void VGA_ScrollReset(void);        // Reset scroll to bottom (current content)
int VGA_GetScrollOffset(void);     // Get current scroll offset
void VGA_EnableScrolling(bool enable);
void VGA_ShowCursor(void);
void VGA_HideCursor(void);

/* Cursor position (TEXT mode only; in GRAPHICS mode this maps to fb_console cursor) */
void VGA_GetCursorPosition(int *row, int *col);
void VGA_SetCursorPosition(int row, int col);

/* Text color control */
void VGA_SetTextColor(uint8_t fg, uint8_t bg);
uint8_t VGA_GetTextColor(void); /* returns (bg<<4)|fg */
void VGA_ResetTextColor(void);

// When enabled in GRAPHICS mode, all VGA_Write* output becomes a no-op.
// Useful for keeping a bootscreen visible during early boot while still logging to COM.
void VGA_SetSplashLock(bool enabled);

#endif // KERNEL_VGA_H