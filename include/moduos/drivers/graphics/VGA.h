#ifndef KERNEL_VGA_H
#define KERNEL_VGA_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Basic VGA output functions */
void VGA_WriteChar(char c);
void VGA_Write(const char* str);
void VGA_WriteN(const char *buf, size_t count);
void VGA_Clear();
void VGA_Backspace();
void VGA_Writef(const char* fmt, ...);

/* Scrolling functions */
void VGA_ScrollUp(int lines);      // Scroll up to view previous content
void VGA_ScrollDown(int lines);    // Scroll down to view newer content
void VGA_ScrollReset(void);        // Reset scroll to bottom (current content)
int VGA_GetScrollOffset(void);     // Get current scroll offset
void VGA_EnableScrolling(bool enable);
void VGA_ShowCursor(void);
void VGA_HideCursor(void);

#endif // KERNEL_VGA_H