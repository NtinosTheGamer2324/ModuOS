/*
 * xclock.c - Simple analog clock using libFlareX
 * 
 * Demonstrates animation and drawing primitives
 */

#include "../lib/libFlareX.h"
#include "../server/libc.h"

#define CLOCK_SIZE 200
#define CENTER_X (CLOCK_SIZE / 2)
#define CENTER_Y (CLOCK_SIZE / 2)
#define RADIUS 90

void draw_clock(FlareXDisplay *dpy, FlareXWindow win, void *userdata) {
    (void)userdata;
    
    /* Clear to white */
    FlareXClearWindow(dpy, win, FlareX_COLOR_WHITE);
    
    /* Draw clock face circle */
    /* Note: We don't have circle drawing yet, so draw with rectangles */
    for (int angle = 0; angle < 360; angle += 5) {
        float rad = angle * 3.14159f / 180.0f;
        int x = CENTER_X + (int)(RADIUS * cosf(rad));
        int y = CENTER_Y + (int)(RADIUS * sinf(rad));
        FlareXFillRectangle(dpy, win, x - 2, y - 2, 4, 4, FlareX_COLOR_BLACK);
    }
    
    /* Draw hour markers */
    for (int hour = 0; hour < 12; hour++) {
        float angle = (hour * 30 - 90) * 3.14159f / 180.0f;
        int x1 = CENTER_X + (int)(RADIUS * 0.8f * cosf(angle));
        int y1 = CENTER_Y + (int)(RADIUS * 0.8f * sinf(angle));
        int x2 = CENTER_X + (int)(RADIUS * 0.9f * cosf(angle));
        int y2 = CENTER_Y + (int)(RADIUS * 0.9f * sinf(angle));
        FlareXDrawLine(dpy, win, x1, y1, x2, y2, FlareX_COLOR_BLACK);
    }
    
    /* TODO: Get actual time and draw hands */
    /* For now, draw static hands */
    
    /* Hour hand (pointing to 3) */
    FlareXDrawLine(dpy, win, CENTER_X, CENTER_Y, 
                CENTER_X + 40, CENTER_Y, FlareX_RGB(50, 50, 50));
    
    /* Minute hand (pointing to 12) */
    FlareXDrawLine(dpy, win, CENTER_X, CENTER_Y, 
                CENTER_X, CENTER_Y - 60, FlareX_RGB(100, 100, 100));
    
    /* Second hand (pointing to 6) - red */
    FlareXDrawLine(dpy, win, CENTER_X, CENTER_Y, 
                CENTER_X, CENTER_Y + 70, FlareX_COLOR_RED);
    
    /* Center dot */
    FlareXFillRectangle(dpy, win, CENTER_X - 3, CENTER_Y - 3, 6, 6, FlareX_COLOR_BLACK);
    
    /* Title */
    FlareXDrawText(dpy, win, CENTER_X - 30, 20, "Xenith Clock", FlareX_COLOR_BLACK);
}

int md_main(long argc, char **argv) {
    (void)argc;
    (void)argv;
    
    printf("XClock - Analog clock demo\n");
    
    FlareXDisplay *dpy = FlareXOpenDisplay();
    if (!dpy) {
        printf("ERROR: Could not connect to display\n");
        return 1;
    }
    
    FlareXWindow win = FlareXCreateSimpleWindow(dpy, 200, 200, CLOCK_SIZE, CLOCK_SIZE);
    if (!win) {
        printf("ERROR: Could not create window\n");
        FlareXCloseDisplay(dpy);
        return 1;
    }
    
    FlareXSetWindowTitle(dpy, win, "Clock");
    
    draw_clock(dpy, win, NULL);
    FlareXMapWindow(dpy, win);
    FlareXFlush(dpy, win);
    
    printf("Clock running...\n");
    
    /* Event loop with periodic redraws */
    while (1) {
        draw_clock(dpy, win, NULL);
        FlareXFlush(dpy, win);
        sleep(1);  /* Redraw every second */
    }
    
    FlareXDestroyWindow(dpy, win);
    FlareXCloseDisplay(dpy);
    
    return 0;
}

