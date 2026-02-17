# Writing Xenith26 Applications

## Basic Application Template

```c
#include "../lib/libX26.h"
#include "../server/libc.h"

int md_main(long argc, char **argv) {
    /* 1. Connect to display server */
    X26Display *dpy = X26OpenDisplay();
    if (!dpy) {
        printf("ERROR: Could not connect to display\n");
        return 1;
    }
    
    /* 2. Create a window */
    X26Window win = X26CreateSimpleWindow(dpy, 100, 100, 400, 300);
    if (!win) {
        printf("ERROR: Could not create window\n");
        X26CloseDisplay(dpy);
        return 1;
    }
    
    /* 3. Set window properties */
    X26SetWindowTitle(dpy, win, "My Application");
    
    /* 4. Draw content */
    X26ClearWindow(dpy, win, X26_COLOR_WHITE);
    X26DrawText(dpy, win, 50, 50, "Hello World!", X26_COLOR_BLACK);
    
    /* 5. Show window */
    X26MapWindow(dpy, win);
    X26Flush(dpy, win);
    
    /* 6. Event loop */
    while (1) {
        /* Handle events, redraw, etc. */
        sleep(1);
    }
    
    /* 7. Cleanup */
    X26DestroyWindow(dpy, win);
    X26CloseDisplay(dpy);
    
    return 0;
}
```

## Drawing Functions

### Rectangles

```c
/* Fill a rectangle */
X26FillRectangle(dpy, win, x, y, width, height, X26_RGB(255, 0, 0));

/* Clear entire window */
X26ClearWindow(dpy, win, X26_COLOR_WHITE);
```

### Lines

```c
/* Draw a line */
X26DrawLine(dpy, win, x1, y1, x2, y2, X26_COLOR_BLACK);
```

### Text

```c
/* Draw text */
X26DrawText(dpy, win, x, y, "Hello!", X26_COLOR_BLACK);
```

### Colors

Use predefined colors:
```c
X26_COLOR_BLACK
X26_COLOR_WHITE
X26_COLOR_RED
X26_COLOR_GREEN
X26_COLOR_BLUE
X26_COLOR_YELLOW
X26_COLOR_CYAN
X26_COLOR_MAGENTA
X26_COLOR_GRAY
```

Or create custom colors:
```c
X26Color mycolor = X26_RGB(128, 64, 255);  /* Purple */
X26Color semitransparent = X26_RGBA(255, 0, 0, 128);  /* Semi-transparent red */
```

## Window Types

```c
/* Normal application window */
X26CreateWindow(dpy, x, y, w, h, XAPI_WIN_TYPE_NORMAL);

/* Dialog window */
X26CreateWindow(dpy, x, y, w, h, XAPI_WIN_TYPE_DIALOG);

/* Desktop background */
X26CreateWindow(dpy, x, y, w, h, XAPI_WIN_TYPE_DESKTOP);

/* Panel/dock */
X26CreateWindow(dpy, x, y, w, h, XAPI_WIN_TYPE_DOCK);

/* Splash screen */
X26CreateWindow(dpy, x, y, w, h, XAPI_WIN_TYPE_SPLASH);
```

## Event Handling

```c
X26Event event;
while (1) {
    if (X26NextEvent(dpy, &event) > 0) {
        switch (event.type) {
            case XAPI_EVENT_EXPOSE:
                /* Window needs redraw */
                draw_my_content(dpy, win);
                X26Flush(dpy, win);
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
X26FillRectangle(dpy, win, 0, 0, 100, 100, X26_COLOR_RED);
X26Flush(dpy, win);  /* Don't forget this! */
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
if (X26CreateWindow(...) == 0) {
    printf("Window creation failed!\n");
    return 1;
}
```

### 4. Clean Up Resources

```c
X26DestroyWindow(dpy, win);
X26CloseDisplay(dpy);
```

## Building Your Application

### Option 1: Using the Xenith26 Makefile

Add your app to `EXTERNAL/Xenith26/Makefile`:

```makefile
EXAMPLES = $(BUILD_DIR)/hello_x26.sqr \
           $(BUILD_DIR)/xclock.sqr \
           $(BUILD_DIR)/myapp.sqr

$(BUILD_DIR)/myapp.sqr: $(EXAMPLES_DIR)/myapp.c $(LIBX26)
	$(CC) $(CFLAGS) -o $@ $(EXAMPLES_DIR)/myapp.c $(LIBX26) $(LDFLAGS)
```

### Option 2: Manual Build

```bash
x86_64-elf-gcc -Wall -O2 -ffreestanding -nostdlib -mcmodel=large \
    -mno-red-zone -I../userland \
    -o myapp.sqr myapp.c libX26.a \
    -T ../userland/user.ld
```

## Complete Example: Simple Paint Program

```c
#include "../lib/libX26.h"
#include "../server/libc.h"

X26Color current_color = X26_COLOR_BLACK;

void draw_palette(X26Display *dpy, X26Window win) {
    X26Color colors[] = {
        X26_COLOR_BLACK, X26_COLOR_RED, X26_COLOR_GREEN,
        X26_COLOR_BLUE, X26_COLOR_YELLOW, X26_COLOR_CYAN
    };
    
    for (int i = 0; i < 6; i++) {
        X26FillRectangle(dpy, win, i * 30, 0, 28, 28, colors[i]);
    }
}

int md_main(long argc, char **argv) {
    X26Display *dpy = X26OpenDisplay();
    if (!dpy) return 1;
    
    X26Window win = X26CreateSimpleWindow(dpy, 100, 100, 640, 480);
    X26SetWindowTitle(dpy, win, "Paint");
    
    X26ClearWindow(dpy, win, X26_COLOR_WHITE);
    draw_palette(dpy, win);
    
    X26MapWindow(dpy, win);
    X26Flush(dpy, win);
    
    X26Event event;
    int drawing = 0;
    
    while (1) {
        if (X26NextEvent(dpy, &event) > 0) {
            if (event.type == XAPI_EVENT_MOUSE_PRESS) {
                drawing = 1;
                X26FillRectangle(dpy, win, event.x - 2, event.y - 2, 
                               4, 4, current_color);
                X26Flush(dpy, win);
            } else if (event.type == XAPI_EVENT_MOUSE_RELEASE) {
                drawing = 0;
            } else if (event.type == XAPI_EVENT_MOUSE_MOVE && drawing) {
                X26FillRectangle(dpy, win, event.x - 2, event.y - 2, 
                               4, 4, current_color);
                X26Flush(dpy, win);
            }
        }
    }
    
    X26DestroyWindow(dpy, win);
    X26CloseDisplay(dpy);
    return 0;
}
```
