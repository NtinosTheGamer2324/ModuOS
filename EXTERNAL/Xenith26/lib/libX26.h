#ifndef LIBX26_H
#define LIBX26_H

/*
 * libX26 - Client library for Xenith26 windowing system
 * 
 * This is the equivalent of Xlib for X11.
 * Applications use this library to create windows and draw graphics.
 */

#include <stdint.h>
#include "../server/xapi_proto.h"

/* Opaque handle types */
typedef struct X26Display X26Display;
typedef uint32_t X26Window;

/* Color format: ARGB8888 */
typedef uint32_t X26Color;

/* RGB color helper */
#define X26_RGB(r, g, b) (0xFF000000 | ((r) << 16) | ((g) << 8) | (b))
#define X26_RGBA(r, g, b, a) (((a) << 24) | ((r) << 16) | ((g) << 8) | (b))

/* Common colors */
#define X26_COLOR_BLACK   0xFF000000
#define X26_COLOR_WHITE   0xFFFFFFFF
#define X26_COLOR_RED     0xFFFF0000
#define X26_COLOR_GREEN   0xFF00FF00
#define X26_COLOR_BLUE    0xFF0000FF
#define X26_COLOR_YELLOW  0xFFFFFF00
#define X26_COLOR_CYAN    0xFF00FFFF
#define X26_COLOR_MAGENTA 0xFFFF00FF
#define X26_COLOR_GRAY    0xFF808080

/* ========== Display Connection ========== */

/**
 * Open connection to the display server
 * Returns NULL on failure
 */
X26Display *X26OpenDisplay(void);

/**
 * Close connection to display server
 */
void X26CloseDisplay(X26Display *dpy);

/**
 * Get display dimensions
 */
void X26DisplaySize(X26Display *dpy, int *width, int *height);

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
X26Window X26CreateWindow(X26Display *dpy, int x, int y, 
                          int width, int height, xapi_win_type_t type);

/**
 * Create a simple window with default type
 */
X26Window X26CreateSimpleWindow(X26Display *dpy, int x, int y, 
                                int width, int height);

/**
 * Map (show) a window
 */
int X26MapWindow(X26Display *dpy, X26Window win);

/**
 * Unmap (hide) a window
 */
int X26UnmapWindow(X26Display *dpy, X26Window win);

/**
 * Destroy a window
 */
int X26DestroyWindow(X26Display *dpy, X26Window win);

/**
 * Set window title
 */
int X26SetWindowTitle(X26Display *dpy, X26Window win, const char *title);

/**
 * Move window
 */
int X26MoveWindow(X26Display *dpy, X26Window win, int x, int y);

/**
 * Resize window
 */
int X26ResizeWindow(X26Display *dpy, X26Window win, int width, int height);

/**
 * Raise window to front
 */
int X26RaiseWindow(X26Display *dpy, X26Window win);

/* ========== Drawing Functions ========== */

/**
 * Fill rectangle
 */
int X26FillRectangle(X26Display *dpy, X26Window win, 
                     int x, int y, int width, int height, X26Color color);

/**
 * Draw line
 */
int X26DrawLine(X26Display *dpy, X26Window win, 
                int x1, int y1, int x2, int y2, X26Color color);

/**
 * Draw text
 */
int X26DrawText(X26Display *dpy, X26Window win, 
                int x, int y, const char *text, X26Color color);

/**
 * Set pixel
 */
int X26DrawPoint(X26Display *dpy, X26Window win, 
                 int x, int y, X26Color color);

/**
 * Clear window to color
 */
int X26ClearWindow(X26Display *dpy, X26Window win, X26Color color);

/**
 * Flush drawing commands and update window on screen
 */
int X26Flush(X26Display *dpy, X26Window win);

/* ========== Event Handling ========== */

typedef struct {
    uint16_t type;         /* xapi_msg_type_t */
    X26Window window;
    
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
} X26Event;

/**
 * Wait for next event
 * Returns 1 if event received, 0 on timeout, -1 on error
 */
int X26NextEvent(X26Display *dpy, X26Event *event);

/**
 * Check if events are pending
 */
int X26Pending(X26Display *dpy);

/* ========== Helper Functions ========== */

/**
 * Simple message loop helper
 * Calls draw_func whenever window needs redrawing
 */
void X26EventLoop(X26Display *dpy, X26Window win, 
                  void (*draw_func)(X26Display*, X26Window, void*),
                  void *userdata);

#endif /* LIBX26_H */
