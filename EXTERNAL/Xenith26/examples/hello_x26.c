/*
 * hello_x26.c - Simple "Hello World" window using libX26
 * 
 * This is the equivalent of a basic Xlib hello world program.
 */

#include "../lib/libX26.h"
#include "../server/libc.h"

void draw_window(X26Display *dpy, X26Window win, void *userdata) {
    (void)userdata;
    
    /* Clear window to white */
    X26ClearWindow(dpy, win, X26_COLOR_WHITE);
    
    /* Draw colored rectangles */
    X26FillRectangle(dpy, win, 10, 10, 100, 50, X26_COLOR_RED);
    X26FillRectangle(dpy, win, 120, 10, 100, 50, X26_COLOR_GREEN);
    X26FillRectangle(dpy, win, 230, 10, 100, 50, X26_COLOR_BLUE);
    
    /* Draw some text */
    X26DrawText(dpy, win, 50, 80, "Hello, Xenith26!", X26_COLOR_BLACK);
    X26DrawText(dpy, win, 50, 100, "This is an X11-like windowing system", X26_COLOR_GRAY);
    
    /* Draw a border */
    X26DrawLine(dpy, win, 0, 0, 399, 0, X26_COLOR_BLACK);
    X26DrawLine(dpy, win, 399, 0, 399, 299, X26_COLOR_BLACK);
    X26DrawLine(dpy, win, 399, 299, 0, 299, X26_COLOR_BLACK);
    X26DrawLine(dpy, win, 0, 299, 0, 0, X26_COLOR_BLACK);
}

int md_main(long argc, char **argv) {
    (void)argc;
    (void)argv;
    
    printf("Hello X26 - Simple windowing example\n");
    
    /* Connect to display server */
    X26Display *dpy = X26OpenDisplay();
    if (!dpy) {
        printf("ERROR: Could not connect to display server\n");
        printf("Make sure xenith26d is running!\n");
        return 1;
    }
    
    int width, height;
    X26DisplaySize(dpy, &width, &height);
    printf("Connected to display: %dx%d\n", width, height);
    
    /* Create window */
    X26Window win = X26CreateSimpleWindow(dpy, 100, 100, 400, 300);
    if (!win) {
        printf("ERROR: Could not create window\n");
        X26CloseDisplay(dpy);
        return 1;
    }
    
    printf("Created window ID: %u\n", win);
    
    /* Set window title */
    X26SetWindowTitle(dpy, win, "Hello Xenith26");
    
    /* Draw initial content */
    draw_window(dpy, win, NULL);
    
    /* Show window */
    X26MapWindow(dpy, win);
    X26Flush(dpy, win);
    
    printf("Window mapped and visible!\n");
    printf("Press Ctrl+C to exit...\n");
    
    /* Simple event loop */
    X26EventLoop(dpy, win, draw_window, NULL);
    
    /* Cleanup */
    X26DestroyWindow(dpy, win);
    X26CloseDisplay(dpy);
    
    return 0;
}
