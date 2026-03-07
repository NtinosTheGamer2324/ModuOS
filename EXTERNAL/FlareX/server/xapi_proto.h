#ifndef XAPI_PROTO_H
#define XAPI_PROTO_H

#include <stdint.h>

/*
 * MD64API GRP types (md64api_grp_mode_t, md64api_grp_format_t,
 * md64api_grp_video_info_t) are provided by libc.h -> md64api_grp.h.
 * Do not redefine them here.
 */
#ifndef MD64API_GRP_DEFAULT_DEVICE
#define MD64API_GRP_DEFAULT_DEVICE "$/dev/graphics/video0"
#endif

/*
 * FlareX Protocol - X11-like windowing for ModuOS
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

/* Set single pixel */
typedef struct __attribute__((packed)) {
    xapi_msg_hdr_t hdr;
    int16_t x, y;
    uint32_t color;        /* ARGB8888 format */
} xapi_cmd_pixel_t;

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

/* ========== Window Manager Protocol ========== */

/* Window properties (like ICCCM/EWMH) */
typedef enum {
    XAPI_PROP_WM_NAME = 100,        /* Window title */
    XAPI_PROP_WM_CLASS = 101,       /* Application class */
    XAPI_PROP_WM_PROTOCOLS = 102,   /* Supported protocols */
    XAPI_PROP_WM_STATE = 103,       /* Window state */
    XAPI_PROP_WM_HINTS = 104,       /* Window hints */
    XAPI_PROP_WM_NORMAL_HINTS = 105, /* Size hints */
    XAPI_PROP_WM_TRANSIENT_FOR = 106, /* Parent window */
    XAPI_PROP_WM_PID = 107,         /* Process ID */
    XAPI_PROP_WM_DESKTOP = 108,     /* Virtual desktop number */
} xapi_property_t;

/* Window states */
typedef enum {
    XAPI_STATE_NORMAL = 0,
    XAPI_STATE_MINIMIZED = 1,
    XAPI_STATE_MAXIMIZED = 2,
    XAPI_STATE_FULLSCREEN = 3,
    XAPI_STATE_HIDDEN = 4,
} xapi_window_state_t;

/* Window hints */
typedef struct __attribute__((packed)) {
    uint32_t flags;
    int32_t min_width, min_height;
    int32_t max_width, max_height;
    int32_t base_width, base_height;
    int32_t width_inc, height_inc;
    uint8_t decorations;       /* 1 = show decorations, 0 = borderless */
    uint8_t input_focus;       /* 1 = wants input focus */
    uint8_t urgency;           /* 1 = urgent (flashing taskbar) */
} xapi_window_hints_t;

/* Hint flags */
#define XAPI_HINT_MIN_SIZE  0x01
#define XAPI_HINT_MAX_SIZE  0x02
#define XAPI_HINT_BASE_SIZE 0x04
#define XAPI_HINT_RESIZE_INC 0x08
#define XAPI_HINT_DECORATIONS 0x10
#define XAPI_HINT_INPUT 0x20
#define XAPI_HINT_URGENCY 0x40

/* Set window property */
typedef struct __attribute__((packed)) {
    xapi_msg_hdr_t hdr;
    uint16_t property;        /* xapi_property_t */
    uint16_t data_len;
    /* data follows */
} xapi_win_set_property_t;

/* Get window property */
typedef struct __attribute__((packed)) {
    xapi_msg_hdr_t hdr;
    uint16_t property;        /* xapi_property_t */
} xapi_win_get_property_t;

/* Client message (for WM communication) */
typedef struct __attribute__((packed)) {
    xapi_msg_hdr_t hdr;
    uint32_t message_type;
    uint32_t data[5];         /* Message-specific data */
} xapi_client_message_t;

/* Common client messages */
#define XAPI_MSG_WM_CHANGE_STATE 0x01  /* Change window state */
#define XAPI_MSG_WM_TAKE_FOCUS   0x02  /* Request focus */
#define XAPI_MSG_WM_DELETE_WINDOW 0x03 /* Request close */
#define XAPI_MSG_WM_PING         0x04  /* Ping (for hang detection) */

/* Additional window management commands */
#define XAPI_WIN_SET_PROPERTY    50    /* Set window property */
#define XAPI_WIN_GET_PROPERTY    51    /* Get window property */
#define XAPI_WIN_CLIENT_MESSAGE  52    /* Send client message */
#define XAPI_WIN_CONFIGURE       53    /* Configure window (WM -> client) */

/* ========== Shared Memory Pixmaps (MIT-SHM style) ========== */

/* Create shared memory segment for fast blitting */
typedef struct __attribute__((packed)) {
    xapi_msg_hdr_t hdr;
    uint32_t shm_id;          /* Shared memory ID (like SysV IPC key) */
    uint32_t size;            /* Size in bytes */
    uint16_t width;           /* Pixmap width */
    uint16_t height;          /* Pixmap height */
    uint8_t  format;          /* XRGB8888, RGB565, etc. */
} xapi_shm_create_t;

/* Attach to shared memory */
typedef struct __attribute__((packed)) {
    xapi_msg_hdr_t hdr;
    uint32_t shm_id;
    uint32_t pixmap_id;       /* Returned pixmap handle */
} xapi_shm_attach_t;

/* Blit from shared memory pixmap */
typedef struct __attribute__((packed)) {
    xapi_msg_hdr_t hdr;
    uint32_t pixmap_id;
    int16_t src_x, src_y;
    int16_t dst_x, dst_y;
    uint16_t width, height;
} xapi_shm_blit_t;

/* Detach shared memory */
typedef struct __attribute__((packed)) {
    xapi_msg_hdr_t hdr;
    uint32_t pixmap_id;
} xapi_shm_detach_t;

#define XAPI_SHM_CREATE   60
#define XAPI_SHM_ATTACH   61
#define XAPI_SHM_BLIT     62
#define XAPI_SHM_DETACH   63

/* Pixmap formats */
#define XAPI_FORMAT_XRGB8888  0
#define XAPI_FORMAT_RGB565    1
#define XAPI_FORMAT_ARGB8888  2  /* With alpha */

/* ========== Compositor Features ========== */

/* Window stacking operations */
typedef struct __attribute__((packed)) {
    xapi_msg_hdr_t hdr;
    uint8_t operation;        /* XAPI_STACK_* */
    uint32_t sibling_id;      /* For ABOVE/BELOW operations */
} xapi_win_restack_t;

#define XAPI_STACK_RAISE       0  /* Raise to top */
#define XAPI_STACK_LOWER       1  /* Lower to bottom */
#define XAPI_STACK_ABOVE       2  /* Place above sibling */
#define XAPI_STACK_BELOW       3  /* Place below sibling */

#define XAPI_WIN_RESTACK       70

/* Window decorations (drawn by compositor) */
typedef struct __attribute__((packed)) {
    uint8_t title_bar;        /* Show title bar */
    uint8_t border;           /* Show border */
    uint8_t resize_handles;   /* Show resize grips */
    uint8_t close_button;     /* Show close button */
    uint8_t maximize_button;  /* Show maximize button */
    uint8_t minimize_button;  /* Show minimize button */
    uint32_t title_bg_color;  /* Title bar background */
    uint32_t title_fg_color;  /* Title bar text color */
    uint32_t border_color;    /* Border color */
    uint8_t border_width;     /* Border width in pixels */
    uint8_t shadow;           /* Drop shadow enabled */
    uint8_t shadow_size;      /* Shadow blur radius */
} xapi_decoration_style_t;

/* Set window decorations */
typedef struct __attribute__((packed)) {
    xapi_msg_hdr_t hdr;
    xapi_decoration_style_t style;
} xapi_win_set_decorations_t;

#define XAPI_WIN_SET_DECORATIONS  71

/* Window opacity/transparency */
typedef struct __attribute__((packed)) {
    xapi_msg_hdr_t hdr;
    uint8_t opacity;          /* 0=transparent, 255=opaque */
} xapi_win_set_opacity_t;

#define XAPI_WIN_SET_OPACITY  72

/* Damage tracking - notify server of changed region */
typedef struct __attribute__((packed)) {
    xapi_msg_hdr_t hdr;
    int16_t x, y;
    uint16_t width, height;
} xapi_win_damage_t;

#define XAPI_WIN_DAMAGE  73

/* ========== Clipboard/Selection Support ========== */

/* Selection types (like X11 CLIPBOARD, PRIMARY, SECONDARY) */
#define XAPI_SELECTION_CLIPBOARD  0  /* Standard clipboard (Ctrl+C/V) */
#define XAPI_SELECTION_PRIMARY    1  /* Primary selection (middle-click paste) */
#define XAPI_SELECTION_SECONDARY  2  /* Secondary selection */

/* Data formats */
#define XAPI_FORMAT_TEXT_PLAIN   0
#define XAPI_FORMAT_TEXT_UTF8    1
#define XAPI_FORMAT_IMAGE_PNG    2
#define XAPI_FORMAT_IMAGE_BMP    3
#define XAPI_FORMAT_CUSTOM       255

/* Set selection owner */
typedef struct __attribute__((packed)) {
    xapi_msg_hdr_t hdr;
    uint8_t selection;        /* XAPI_SELECTION_* */
    uint32_t timestamp;       /* For conflict resolution */
} xapi_selection_own_t;

/* Request selection data */
typedef struct __attribute__((packed)) {
    xapi_msg_hdr_t hdr;
    uint8_t selection;        /* XAPI_SELECTION_* */
    uint8_t format;           /* Requested format */
} xapi_selection_request_t;

/* Send selection data */
typedef struct __attribute__((packed)) {
    xapi_msg_hdr_t hdr;
    uint8_t selection;
    uint8_t format;
    uint32_t data_len;
    /* data follows */
} xapi_selection_data_t;

/* Selection notify event (sent when data is ready) */
typedef struct __attribute__((packed)) {
    xapi_msg_hdr_t hdr;
    uint8_t selection;
    uint8_t format;
    uint32_t data_len;
} xapi_event_selection_t;

#define XAPI_SELECTION_OWN      80
#define XAPI_SELECTION_REQUEST  81
#define XAPI_SELECTION_DATA     82
#define XAPI_EVENT_SELECTION    83

/* ========== Drag and Drop Protocol ========== */

/* Drag operations */
#define XAPI_DRAG_NONE   0
#define XAPI_DRAG_COPY   1
#define XAPI_DRAG_MOVE   2
#define XAPI_DRAG_LINK   3

/* Begin drag operation */
typedef struct __attribute__((packed)) {
    xapi_msg_hdr_t hdr;
    uint8_t format;           /* Data format being dragged */
    uint8_t allowed_ops;      /* Bitmask of XAPI_DRAG_* */
    uint32_t data_len;
    /* Preview data/metadata follows */
} xapi_drag_begin_t;

/* Drag motion event (sent to window under cursor) */
typedef struct __attribute__((packed)) {
    xapi_msg_hdr_t hdr;
    int16_t x, y;             /* Position in window */
    uint8_t format;
    uint8_t allowed_ops;
} xapi_event_drag_motion_t;

/* Drag drop event (sent when mouse released) */
typedef struct __attribute__((packed)) {
    xapi_msg_hdr_t hdr;
    int16_t x, y;
    uint8_t format;
    uint8_t operation;        /* Chosen XAPI_DRAG_* operation */
} xapi_event_drag_drop_t;

/* Drag leave event (cursor left window) */
typedef struct __attribute__((packed)) {
    xapi_msg_hdr_t hdr;
} xapi_event_drag_leave_t;

/* Accept/reject drop */
typedef struct __attribute__((packed)) {
    xapi_msg_hdr_t hdr;
    uint8_t accept;           /* 1=accept, 0=reject */
    uint8_t operation;        /* Chosen operation */
} xapi_drag_status_t;

/* Get drag data */
typedef struct __attribute__((packed)) {
    xapi_msg_hdr_t hdr;
    uint8_t format;
} xapi_drag_get_data_t;

#define XAPI_DRAG_BEGIN        90
#define XAPI_EVENT_DRAG_MOTION 91
#define XAPI_EVENT_DRAG_DROP   92
#define XAPI_EVENT_DRAG_LEAVE  93
#define XAPI_DRAG_STATUS       94
#define XAPI_DRAG_GET_DATA     95

/* ========== External Compositor Protocol ========== */

/* Register as the compositor (exclusive - only one allowed) */
typedef struct __attribute__((packed)) {
    xapi_msg_hdr_t hdr;
    uint32_t compositor_pid;
} xapi_compositor_register_t;

/* Unregister compositor */
typedef struct __attribute__((packed)) {
    xapi_msg_hdr_t hdr;
} xapi_compositor_unregister_t;

/* Compositor damage event (server -> compositor) */
typedef struct __attribute__((packed)) {
    xapi_msg_hdr_t hdr;
    uint32_t window_id;
    int16_t x, y;
    uint16_t width, height;
} xapi_event_compositor_damage_t;

/* Compositor window list (for external compositor to know what to render) */
typedef struct __attribute__((packed)) {
    xapi_msg_hdr_t hdr;
    uint32_t window_count;
    /* Array of window_info follows */
} xapi_compositor_window_list_t;

typedef struct __attribute__((packed)) {
    uint32_t window_id;
    int16_t x, y;
    uint16_t width, height;
    int16_t z_order;
    uint8_t mapped;
    uint8_t opacity;
    uint32_t gfx_handle;      /* GPU buffer handle for blitting */
} xapi_window_info_t;

/* Request window list */
typedef struct __attribute__((packed)) {
    xapi_msg_hdr_t hdr;
} xapi_compositor_get_windows_t;

/* Notify server that compositor has rendered a frame */
typedef struct __attribute__((packed)) {
    xapi_msg_hdr_t hdr;
    uint32_t frame_number;
} xapi_compositor_frame_done_t;

#define XAPI_COMPOSITOR_REGISTER     100
#define XAPI_COMPOSITOR_UNREGISTER   101
#define XAPI_COMPOSITOR_GET_WINDOWS  102
#define XAPI_COMPOSITOR_FRAME_DONE   103
#define XAPI_EVENT_COMPOSITOR_DAMAGE 104

/* Configure notify (sent by WM when window moves/resizes) */
typedef struct __attribute__((packed)) {
    xapi_msg_hdr_t hdr;
    int16_t x, y;
    uint16_t w, h;
    uint8_t send_event;       /* 1 if sent by WM, 0 if real */
} xapi_event_configure_t;

#endif /* XAPI_PROTO_H */
