#ifndef LIBFlareX_H
#define LIBFlareX_H

/*
 * libFlareX - Client library for FlareX windowing system
 * 
 * This is the equivalent of Xlib for X11.
 * Applications use this library to create windows and draw graphics.
 */

#include <stdint.h>
#include "../server/libc.h"
#include "../server/xapi_proto.h"

/* Opaque handle types */
typedef struct FlareXDisplay FlareXDisplay;
typedef uint32_t FlareXWindow;

/* Color format: ARGB8888 */
typedef uint32_t FlareXColor;

/* RGB color helper */
#define FlareX_RGB(r, g, b) (0xFF000000 | ((r) << 16) | ((g) << 8) | (b))
#define FlareX_RGBA(r, g, b, a) (((a) << 24) | ((r) << 16) | ((g) << 8) | (b))

/* Common colors */
#define FlareX_COLOR_BLACK   0xFF000000
#define FlareX_COLOR_WHITE   0xFFFFFFFF
#define FlareX_COLOR_RED     0xFFFF0000
#define FlareX_COLOR_GREEN   0xFF00FF00
#define FlareX_COLOR_BLUE    0xFF0000FF
#define FlareX_COLOR_YELLOW  0xFFFFFF00
#define FlareX_COLOR_CYAN    0xFF00FFFF
#define FlareX_COLOR_MAGENTA 0xFFFF00FF
#define FlareX_COLOR_GRAY    0xFF808080

/* ========== Display Connection ========== */

/**
 * Open connection to the display server
 * Returns NULL on failure
 */
FlareXDisplay *FlareXOpenDisplay(void);

/**
 * Close connection to display server
 */
void FlareXCloseDisplay(FlareXDisplay *dpy);

/**
 * Get display dimensions
 */
void FlareXDisplaySize(FlareXDisplay *dpy, int *width, int *height);

/* ========== Window Management ========== */

/**
 * Create a window
 * 
 * @param dpy - Display connection
 * @param x, y - Position on screen
 * @param width, height - Window dimensions
 * @param type - Window type (XAPI_WIN_TYPE_*)
 * @return Window handle, or 0 on failure
 */
FlareXWindow FlareXCreateWindow(FlareXDisplay *dpy, int x, int y, 
                          int width, int height, xapi_win_type_t type);

/**
 * Create a simple window with default type
 */
FlareXWindow FlareXCreateSimpleWindow(FlareXDisplay *dpy, int x, int y, 
                                int width, int height);

/**
 * Map (show) a window
 */
int FlareXMapWindow(FlareXDisplay *dpy, FlareXWindow win);

/**
 * Unmap (hide) a window
 */
int FlareXUnmapWindow(FlareXDisplay *dpy, FlareXWindow win);

/**
 * Destroy a window
 */
int FlareXDestroyWindow(FlareXDisplay *dpy, FlareXWindow win);

/**
 * Set window title
 */
int FlareXSetWindowTitle(FlareXDisplay *dpy, FlareXWindow win, const char *title);

/**
 * Move window
 */
int FlareXMoveWindow(FlareXDisplay *dpy, FlareXWindow win, int x, int y);

/**
 * Resize window
 */
int FlareXResizeWindow(FlareXDisplay *dpy, FlareXWindow win, int width, int height);

/**
 * Raise window to front
 */
int FlareXRaiseWindow(FlareXDisplay *dpy, FlareXWindow win);

/* ========== Drawing Functions ========== */

/**
 * Fill rectangle
 */
int FlareXFillRectangle(FlareXDisplay *dpy, FlareXWindow win, 
                     int x, int y, int width, int height, FlareXColor color);

/**
 * Draw line
 */
int FlareXDrawLine(FlareXDisplay *dpy, FlareXWindow win, 
                int x1, int y1, int x2, int y2, FlareXColor color);

/**
 * Draw text
 */
int FlareXDrawText(FlareXDisplay *dpy, FlareXWindow win, 
                int x, int y, const char *text, FlareXColor color);

/**
 * Set pixel
 */
int FlareXDrawPoint(FlareXDisplay *dpy, FlareXWindow win, 
                 int x, int y, FlareXColor color);

/**
 * Clear window to color
 */
int FlareXClearWindow(FlareXDisplay *dpy, FlareXWindow win, FlareXColor color);

/**
 * Flush drawing commands and update window on screen
 */
int FlareXFlush(FlareXDisplay *dpy, FlareXWindow win);

/* ========== Event Handling ========== */

typedef struct {
    uint16_t type;         /* xapi_msg_type_t */
    FlareXWindow window;
    
    /* Key events */
    uint16_t keycode;
    uint32_t unicode;
    
    /* Mouse events */
    int16_t x, y;
    int16_t root_x, root_y;
    uint8_t buttons;
    uint8_t button;
    
    /* Modifiers */
    uint16_t modifiers;
} FlareXEvent;

/**
 * Wait for next event
 * Returns 1 if event received, 0 on timeout, -1 on error
 */
int FlareXNextEvent(FlareXDisplay *dpy, FlareXEvent *event);

/**
 * Check if events are pending
 */
int FlareXPending(FlareXDisplay *dpy);

/* ========== Helper Functions ========== */

/**
 * Simple message loop helper
 * Calls draw_func whenever window needs redrawing
 */
void FlareXEventLoop(FlareXDisplay *dpy, FlareXWindow win, 
                  void (*draw_func)(FlareXDisplay*, FlareXWindow, void*),
                  void *userdata);

#endif /* LIBFlareX_H */

