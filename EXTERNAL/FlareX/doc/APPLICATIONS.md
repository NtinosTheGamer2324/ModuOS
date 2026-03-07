# Writing FlareX Applications

## Basic Application Template

```c
#include "../lib/libFlareX.h"
#include "../server/libc.h"

int md_main(long argc, char **argv) {
    /* 1. Connect to display server */
    FlareXDisplay *dpy = FlareXOpenDisplay();
    if (!dpy) {
        printf("ERROR: Could not connect to display\n");
        return 1;
    }
    
    /* 2. Create a window */
    FlareXWindow win = FlareXCreateSimpleWindow(dpy, 100, 100, 400, 300);
    if (!win) {
        printf("ERROR: Could not create window\n");
        FlareXCloseDisplay(dpy);
        return 1;
    }
    
    /* 3. Set window properties */
    FlareXSetWindowTitle(dpy, win, "My Application");
    
    /* 4. Draw content */
    FlareXClearWindow(dpy, win, FlareX_COLOR_WHITE);
    FlareXDrawText(dpy, win, 50, 50, "Hello World!", FlareX_COLOR_BLACK);
    
    /* 5. Show window */
    FlareXMapWindow(dpy, win);
    FlareXFlush(dpy, win);
    
    /* 6. Event loop */
    while (1) {
        /* Handle events, redraw, etc. */
        sleep(1);
    }
    
    /* 7. Cleanup */
    FlareXDestroyWindow(dpy, win);
    FlareXCloseDisplay(dpy);
    
    return 0;
}
```

## Drawing Functions

### Rectangles

```c
/* Fill a rectangle */
FlareXFillRectangle(dpy, win, x, y, width, height, FlareX_RGB(255, 0, 0));

/* Clear entire window */
FlareXClearWindow(dpy, win, FlareX_COLOR_WHITE);
```

### Lines

```c
/* Draw a line */
FlareXDrawLine(dpy, win, x1, y1, x2, y2, FlareX_COLOR_BLACK);
```

### Text

```c
/* Draw text */
FlareXDrawText(dpy, win, x, y, "Hello!", FlareX_COLOR_BLACK);
```

### Colors

Use predefined colors:
```c
FlareX_COLOR_BLACK
FlareX_COLOR_WHITE
FlareX_COLOR_RED
FlareX_COLOR_GREEN
FlareX_COLOR_BLUE
FlareX_COLOR_YELLOW
FlareX_COLOR_CYAN
FlareX_COLOR_MAGENTA
FlareX_COLOR_GRAY
```

Or create custom colors:
```c
FlareXColor mycolor = FlareX_RGB(128, 64, 255);  /* Purple */
FlareXColor semitransparent = FlareX_RGBA(255, 0, 0, 128);  /* Semi-transparent red */
```

## Window Types

```c
/* Normal application window */
FlareXCreateWindow(dpy, x, y, w, h, XAPI_WIN_TYPE_NORMAL);

/* Dialog window */
FlareXCreateWindow(dpy, x, y, w, h, XAPI_WIN_TYPE_DIALOG);

/* Desktop background */
FlareXCreateWindow(dpy, x, y, w, h, XAPI_WIN_TYPE_DESKTOP);

/* Panel/dock */
FlareXCreateWindow(dpy, x, y, w, h, XAPI_WIN_TYPE_DOCK);

/* Splash screen */
FlareXCreateWindow(dpy, x, y, w, h, XAPI_WIN_TYPE_SPLASH);
```

## Event Handling

```c
FlareXEvent event;
while (1) {
    if (FlareXNextEvent(dpy, &event) > 0) {
        switch (event.type) {
            case XAPI_EVENT_EXPOSE:
                /* Window needs redraw */
                draw_my_content(dpy, win);
                FlareXFlush(dpy, win);
                break;
                
            case XAPI_EVENT_KEY_PRESS:
                printf("Key pressed: %u\n", event.keycode);
                break;
                
            case XAPI_EVENT_MOUSE_PRESS:
                printf("Mouse click at %d, %d\n", event.x, event.y);
                break;
        }
    }
}
```

## Best Practices

### 1. Always Flush After Drawing

```c
FlareXFillRectangle(dpy, win, 0, 0, 100, 100, FlareX_COLOR_RED);
FlareXFlush(dpy, win);  /* Don't forget this! */
```

### 2. Handle Expose Events

```c
if (event.type == XAPI_EVENT_EXPOSE) {
    /* Redraw your window content */
    redraw_function(dpy, win);
}
```

### 3. Check Return Values

```c
if (FlareXCreateWindow(...) == 0) {
    printf("Window creation failed!\n");
    return 1;
}
```

### 4. Clean Up Resources

```c
FlareXDestroyWindow(dpy, win);
FlareXCloseDisplay(dpy);
```

## Building Your Application

### Option 1: Using the FlareX Makefile

Add your app to `EXTERNAL/FlareX/Makefile`:

```makefile
EXAMPLES = $(BUILD_DIR)/hello_FlareX.sqr \
           $(BUILD_DIR)/xclock.sqr \
           $(BUILD_DIR)/myapp.sqr

$(BUILD_DIR)/myapp.sqr: $(EXAMPLES_DIR)/myapp.c $(LIBFlareX)
	$(CC) $(CFLAGS) -o $@ $(EXAMPLES_DIR)/myapp.c $(LIBFlareX) $(LDFLAGS)
```

### Option 2: Manual Build

```bash
x86_64-elf-gcc -Wall -O2 -ffreestanding -nostdlib -mcmodel=large \
    -mno-red-zone -I../userland \
    -o myapp.sqr myapp.c libFlareX.a \
    -T ../userland/user.ld
```

## Complete Example: Simple Paint Program

```c
#include "../lib/libFlareX.h"
#include "../server/libc.h"

FlareXColor current_color = FlareX_COLOR_BLACK;

void draw_palette(FlareXDisplay *dpy, FlareXWindow win) {
    FlareXColor colors[] = {
        FlareX_COLOR_BLACK, FlareX_COLOR_RED, FlareX_COLOR_GREEN,
        FlareX_COLOR_BLUE, FlareX_COLOR_YELLOW, FlareX_COLOR_CYAN
    };
    
    for (int i = 0; i < 6; i++) {
        FlareXFillRectangle(dpy, win, i * 30, 0, 28, 28, colors[i]);
    }
}

int md_main(long argc, char **argv) {
    FlareXDisplay *dpy = FlareXOpenDisplay();
    if (!dpy) return 1;
    
    FlareXWindow win = FlareXCreateSimpleWindow(dpy, 100, 100, 640, 480);
    FlareXSetWindowTitle(dpy, win, "Paint");
    
    FlareXClearWindow(dpy, win, FlareX_COLOR_WHITE);
    draw_palette(dpy, win);
    
    FlareXMapWindow(dpy, win);
    FlareXFlush(dpy, win);
    
    FlareXEvent event;
    int drawing = 0;
    
    while (1) {
        if (FlareXNextEvent(dpy, &event) > 0) {
            if (event.type == XAPI_EVENT_MOUSE_PRESS) {
                drawing = 1;
                FlareXFillRectangle(dpy, win, event.x - 2, event.y - 2, 
                               4, 4, current_color);
                FlareXFlush(dpy, win);
            } else if (event.type == XAPI_EVENT_MOUSE_RELEASE) {
                drawing = 0;
            } else if (event.type == XAPI_EVENT_MOUSE_MOVE && drawing) {
                FlareXFillRectangle(dpy, win, event.x - 2, event.y - 2, 
                               4, 4, current_color);
                FlareXFlush(dpy, win);
            }
        }
    }
    
    FlareXDestroyWindow(dpy, win);
    FlareXCloseDisplay(dpy);
    return 0;
}
```

