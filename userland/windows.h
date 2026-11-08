#pragma once
/*
 * windows.h - dead-simple window API for ModuOS (links against windows.sqrl)
 *
 * Hides: mdw mailbox, shm mapping, key/mouse rings, damage, resize remap,
 * font loading. Colors are 0xAARRGGBB (use WIN_RGB). (0,0) = top-left.
 */
#include <stdint.h>
#include <stddef.h>

typedef struct win win_t;

#define WIN_RGB(r, g, b)  (0xFF000000u | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
#define WIN_BLACK  WIN_RGB(0, 0, 0)
#define WIN_WHITE  WIN_RGB(255, 255, 255)
#define WIN_RED    WIN_RGB(220, 50, 50)
#define WIN_GREEN  WIN_RGB(50, 200, 90)
#define WIN_BLUE   WIN_RGB(60, 110, 230)
#define WIN_YELLOW WIN_RGB(240, 220, 60)
#define WIN_GRAY   WIN_RGB(128, 128, 128)

typedef enum {
    WIN_EV_KEY = 1,      /* ascii (0 if non-printable), keycode (raw KeyCode) */
    WIN_EV_MOUSE_MOVE,   /* x, y, buttons */
    WIN_EV_MOUSE_DOWN,   /* x, y, buttons, button = which one changed */
    WIN_EV_MOUSE_UP,
    WIN_EV_MOUSE_ENTER,
    WIN_EV_MOUSE_LEAVE,
    WIN_EV_RESIZE        /* w, h = new content size (buffer already remapped) */
} win_event_type_t;

#define WIN_BTN_LEFT   0x1u
#define WIN_BTN_RIGHT  0x2u
#define WIN_BTN_MIDDLE 0x4u

typedef struct {
    int      type;       /* win_event_type_t */
    int      x, y;       /* mouse, content space */
    unsigned buttons;    /* mouse button mask after event */
    unsigned button;     /* DOWN/UP: button that changed */
    int      ascii;      /* KEY */
    int      keycode;    /* KEY */
    int      w, h;       /* RESIZE */
} win_event_t;

/* ---- lifecycle ---- */
win_t *win_open(const char *title, int w, int h);   /* NULL on failure */
win_t *win_open_flags(const char *title, int w, int h, unsigned wm_flags);
void   win_close(win_t *w);                          /* marks dead + destroys */
int    win_alive(win_t *w);                          /* 0 after close / mdw gone */

/* ---- events (non-blocking; returns 1 if *ev filled) ---- */
int    win_poll(win_t *w, win_event_t *ev);

/* ---- info ---- */
int       win_width(win_t *w);
int       win_height(win_t *w);
uint32_t *win_pixels(win_t *w);                      /* raw 0xAARRGGBB buffer, w*h */

/* ---- drawing (all clipped; nothing visible until win_present) ---- */
void win_clear(win_t *w, uint32_t color);
void win_pixel(win_t *w, int x, int y, uint32_t color);
void win_line(win_t *w, int x0, int y0, int x1, int y1, uint32_t color);
void win_rect(win_t *w, int x, int y, int rw, int rh, uint32_t color);       /* outline */
void win_fill_rect(win_t *w, int x, int y, int rw, int rh, uint32_t color);
void win_circle(win_t *w, int cx, int cy, int r, uint32_t color);            /* outline */
void win_fill_circle(win_t *w, int cx, int cy, int r, uint32_t color);
void win_text(win_t *w, int x, int y, const char *s, uint32_t color);
void win_text_scaled(win_t *w, int x, int y, const char *s, uint32_t color, int scale);
int  win_text_width(win_t *w, const char *s);
int  win_text_height(win_t *w);

/* ---- present ---- */
void win_present(win_t *w);                          /* damage whole window, then yield() */
void win_present_rect(win_t *w, int x, int y, int rw, int rh);

/* ---- extras ---- */
void win_set_title_cursor(win_t *w, int wm_cursor_id);  /* WM_CURSOR_* */
void win_minimize(win_t *w);
void win_maximize(win_t *w);
void win_restore(win_t *w);
void win_fullscreen(win_t *w);
void win_resize(win_t *w, int nw, int nh);
void win_sleep_frame(void);                          /* just yield() */
uint64_t win_time_ms(void);