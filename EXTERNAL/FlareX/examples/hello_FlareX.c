/*
 * hello_FlareX.c - Simple "Hello World" window using libFlareX
 * 
 * This is the equivalent of a basic Xlib hello world program.
 */

#include "../lib/libFlareX.h"
#include "../server/libc.h"

void draw_window(FlareXDisplay *dpy, FlareXWindow win, void *userdata) {
    (void)userdata;
    
    /* Clear window to white */
    FlareXClearWindow(dpy, win, FlareX_COLOR_WHITE);
    
    /* Draw colored rectangles */
    FlareXFillRectangle(dpy, win, 10, 10, 100, 50, FlareX_COLOR_RED);
    FlareXFillRectangle(dpy, win, 120, 10, 100, 50, FlareX_COLOR_GREEN);
    FlareXFillRectangle(dpy, win, 230, 10, 100, 50, FlareX_COLOR_BLUE);
    
    /* Draw some text */
    FlareXDrawText(dpy, win, 50, 80, "Hello, FlareX!", FlareX_COLOR_BLACK);
    FlareXDrawText(dpy, win, 50, 100, "This is an X11-like windowing system", FlareX_COLOR_GRAY);
    
    /* Draw a border */
    FlareXDrawLine(dpy, win, 0, 0, 399, 0, FlareX_COLOR_BLACK);
    FlareXDrawLine(dpy, win, 399, 0, 399, 299, FlareX_COLOR_BLACK);
    FlareXDrawLine(dpy, win, 399, 299, 0, 299, FlareX_COLOR_BLACK);
    FlareXDrawLine(dpy, win, 0, 299, 0, 0, FlareX_COLOR_BLACK);
}

int md_main(long argc, char **argv) {
    (void)argc;
    (void)argv;
    
    printf("Hello FlareX - Simple windowing example\n");
    
    /* Connect to display server */
    FlareXDisplay *dpy = FlareXOpenDisplay();
    if (!dpy) {
        printf("ERROR: Could not connect to display server\n");
        printf("Make sure FlareXd is running!\n");
        return 1;
    }
    
    int width, height;
    FlareXDisplaySize(dpy, &width, &height);
    printf("Connected to display: %dx%d\n", width, height);
    
    /* Create window */
    FlareXWindow win = FlareXCreateSimpleWindow(dpy, 100, 100, 400, 300);
    if (!win) {
        printf("ERROR: Could not create window\n");
        FlareXCloseDisplay(dpy);
        return 1;
    }
    
    printf("Created window ID: %u\n", win);
    
    /* Set window title */
    FlareXSetWindowTitle(dpy, win, "Hello FlareX");
    
    /* Draw initial content */
    draw_window(dpy, win, NULL);
    
    /* Show window */
    FlareXMapWindow(dpy, win);
    FlareXFlush(dpy, win);
    
    printf("Window mapped and visible!\n");
    printf("Press Ctrl+C to exit...\n");
    
    /* Simple event loop */
    FlareXEventLoop(dpy, win, draw_window, NULL);
    
    /* Cleanup */
    FlareXDestroyWindow(dpy, win);
    FlareXCloseDisplay(dpy);
    
    return 0;
}

