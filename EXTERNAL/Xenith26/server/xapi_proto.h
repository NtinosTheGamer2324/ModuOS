#ifndef XAPI_PROTO_H
#define XAPI_PROTO_H

#include <stdint.h>

/*
 * Xenith26 Protocol - X11-like windowing for ModuOS
 * 
 * Communication is via UserFS nodes:
 *   $/user/xapi/gfxapi   - Graphics commands (draw rect, blit, etc)
 *   $/user/xapi/event    - Input events from server to client
 *   $/user/xapi/windows  - Window management
 */

/* ========== Message Types ========== */

typedef enum {
    /* Graphics Drawing Commands */
    XAPI_CMD_RECT = 1,      /* Draw filled rectangle */
    XAPI_CMD_TEXT = 2,      /* Draw text */
    XAPI_CMD_BLIT = 3,      /* Blit bitmap/buffer */
    XAPI_CMD_LINE = 4,      /* Draw line */
    XAPI_CMD_PIXEL = 5,     /* Set pixel */
    XAPI_CMD_TEXT_FNT = 6,  /* Draw text with FNT font */
    XAPI_CMD_COMMIT = 10,   /* Commit/present window buffer */
    
    /* Window Management */
    XAPI_WIN_CREATE = 20,   /* Create window */
    XAPI_WIN_DESTROY = 21,  /* Destroy window */
    XAPI_WIN_MAP = 22,      /* Show window */
    XAPI_WIN_UNMAP = 23,    /* Hide window */
    XAPI_WIN_RAISE = 24,    /* Bring to front */
    XAPI_WIN_LOWER = 25,    /* Send to back */
    XAPI_WIN_MOVE = 26,     /* Move window */
    XAPI_WIN_RESIZE = 27,   /* Resize window */
    XAPI_WIN_SET_TITLE = 28, /* Set window title */
    
    /* Events (Server -> Client) */
    XAPI_EVENT_EXPOSE = 40,     /* Redraw needed */
    XAPI_EVENT_KEY_PRESS = 41,  /* Key pressed */
    XAPI_EVENT_KEY_RELEASE = 42, /* Key released */
    XAPI_EVENT_MOUSE_MOVE = 43,  /* Mouse moved */
    XAPI_EVENT_MOUSE_PRESS = 44, /* Mouse button pressed */
    XAPI_EVENT_MOUSE_RELEASE = 45, /* Mouse button released */
    XAPI_EVENT_FOCUS_IN = 46,    /* Window gained focus */
    XAPI_EVENT_FOCUS_OUT = 47,   /* Window lost focus */
} xapi_msg_type_t;

/* ========== Window Types ========== */

typedef enum {
    XAPI_WIN_TYPE_NORMAL = 1,   /* Regular application window */
    XAPI_WIN_TYPE_DIALOG = 2,   /* Dialog window */
    XAPI_WIN_TYPE_DESKTOP = 3,  /* Desktop/background */
    XAPI_WIN_TYPE_DOCK = 4,     /* Dock/panel */
    XAPI_WIN_TYPE_SPLASH = 5,   /* Splash screen */
    XAPI_WIN_TYPE_MENU = 6,     /* Menu window */
    XAPI_WIN_TYPE_TOOLTIP = 7,  /* Tooltip */
} xapi_win_type_t;

/* ========== Protocol Structures ========== */

/* Message header (all messages start with this) */
typedef struct __attribute__((packed)) {
    uint8_t type;          /* xapi_msg_type_t */
    uint8_t reserved;
    uint16_t size;         /* Total message size including header */
    uint32_t window_id;    /* Target window ID (0 = server) */
    uint32_t sequence;     /* Sequence number for replies */
} xapi_msg_hdr_t;

/* Draw filled rectangle */
typedef struct __attribute__((packed)) {
    xapi_msg_hdr_t hdr;
    int16_t x, y;
    uint16_t w, h;
    uint32_t color;        /* ARGB8888 format */
} xapi_cmd_rect_t;

/* Draw text */
typedef struct __attribute__((packed)) {
    xapi_msg_hdr_t hdr;
    int16_t x, y;
    uint32_t color;        /* ARGB8888 format */
    uint16_t text_len;     /* Length of following text */
    /* char text[text_len] follows */
} xapi_cmd_text_t;

/* Draw text with FNT font */
typedef struct __attribute__((packed)) {
    xapi_msg_hdr_t hdr;
    int16_t x, y;
    uint32_t fg_color;     /* ARGB8888 foreground */
    uint32_t bg_color;     /* ARGB8888 background (0 = transparent) */
    uint8_t scale;         /* Font scale: 1=normal, 2=2x, etc. */
    uint8_t reserved[3];
    uint16_t text_len;     /* Length of following text */
    /* char text[text_len] follows */
} xapi_cmd_text_fnt_t;

/* Blit buffer/bitmap */
typedef struct __attribute__((packed)) {
    xapi_msg_hdr_t hdr;
    int16_t dst_x, dst_y;  /* Destination position */
    int16_t src_x, src_y;  /* Source position in buffer */
    uint16_t w, h;         /* Dimensions */
    uint32_t buffer_id;    /* Shared memory buffer ID */
} xapi_cmd_blit_t;

/* Draw line */
typedef struct __attribute__((packed)) {
    xapi_msg_hdr_t hdr;
    int16_t x1, y1;
    int16_t x2, y2;
    uint32_t color;        /* ARGB8888 format */
} xapi_cmd_line_t;

/* Commit/present window */
typedef struct __attribute__((packed)) {
    xapi_msg_hdr_t hdr;
    int16_t x, y;          /* Damaged region */
    uint16_t w, h;
} xapi_cmd_commit_t;

/* Create window */
typedef struct __attribute__((packed)) {
    xapi_msg_hdr_t hdr;
    uint16_t width;
    uint16_t height;
    uint8_t win_type;      /* xapi_win_type_t */
    uint8_t flags;         /* Reserved */
    uint16_t title_len;    /* Length of title string */
    /* char title[title_len] follows */
} xapi_win_create_t;

/* Window management (move/resize) */
typedef struct __attribute__((packed)) {
    xapi_msg_hdr_t hdr;
    int16_t x, y;
    uint16_t w, h;
} xapi_win_geometry_t;

/* Set window title */
typedef struct __attribute__((packed)) {
    xapi_msg_hdr_t hdr;
    uint16_t title_len;
    /* char title[title_len] follows */
} xapi_win_set_title_t;

/* ========== Event Structures ========== */

/* Expose event (redraw needed) */
typedef struct __attribute__((packed)) {
    xapi_msg_hdr_t hdr;
    int16_t x, y;
    uint16_t w, h;
} xapi_event_expose_t;

/* Key event */
typedef struct __attribute__((packed)) {
    xapi_msg_hdr_t hdr;
    uint16_t keycode;      /* Scancode */
    uint16_t modifiers;    /* Shift, Ctrl, Alt, etc. */
    uint32_t unicode;      /* UTF-32 character */
} xapi_event_key_t;

/* Mouse event */
typedef struct __attribute__((packed)) {
    xapi_msg_hdr_t hdr;
    int16_t x, y;          /* Position relative to window */
    int16_t root_x, root_y; /* Absolute screen position */
    uint8_t buttons;       /* Button state bitmask */
    uint8_t button;        /* Which button changed (for press/release) */
    uint16_t modifiers;    /* Keyboard modifiers */
} xapi_event_mouse_t;

/* Mouse button bits */
#define XAPI_MOUSE_BUTTON1 0x01  /* Left */
#define XAPI_MOUSE_BUTTON2 0x02  /* Right */
#define XAPI_MOUSE_BUTTON3 0x04  /* Middle */

/* Keyboard modifier bits */
#define XAPI_MOD_SHIFT 0x01
#define XAPI_MOD_CTRL  0x02
#define XAPI_MOD_ALT   0x04
#define XAPI_MOD_SUPER 0x08

#endif /* XAPI_PROTO_H */