#include "libc.h"
#include "string.h"
#include "userfs.h"
#include "nodes.h"
#include "../lib/lib_fnt.h"
#include "xapi_proto.h"
#include "NodGL.h"
#include "gfx2d.h"

/* Display server state */
typedef struct {
    NodGL_Device device;        /* NodGL device handle */
    NodGL_Context context;      /* NodGL context handle */
    uint32_t width;
    uint32_t height;
    NodGL_FeatureLevel feature_level;
} xserver_display_t;

/* Shared memory pixmap */
typedef struct {
    uint32_t pixmap_id;
    uint32_t shm_id;
    void *addr;               /* Mapped address */
    uint32_t size;
    uint16_t width, height;
    uint8_t format;
    uint32_t owner_pid;
} xserver_pixmap_t;

typedef struct {
    uint32_t id;
    uint32_t owner_pid;
    int16_t x, y;
    uint16_t width, height;
    uint8_t type;               /* xapi_win_type_t */
    uint8_t mapped;             /* Visible? */
    uint8_t state;              /* xapi_window_state_t */
    uint8_t focused;            /* Has keyboard focus? */
    uint8_t opacity;            /* 0-255, for compositing */
    int16_t z_order;            /* Stacking order (higher = on top) */
    char title[128];
    char wm_class[64];          /* Application class name */
    uint32_t *buffer;           /* Window contents */
    xapi_window_hints_t hints;  /* Window manager hints */
    xapi_decoration_style_t decorations; /* Decoration style */
    uint32_t protocols;         /* Supported WM protocols bitmask */
    uint32_t transient_for;     /* Parent window ID (for dialogs) */
    uint32_t desktop;           /* Virtual desktop number */
} xserver_window_t;

/* Forward declarations */
static void update_drag_motion(void);
static void end_drag_drop(void);
static void composite_window(xserver_window_t *win, fnt_font_t *font);

#define MAX_WINDOWS 32
#define MAX_PIXMAPS 64
static xserver_window_t g_windows[MAX_WINDOWS];
static xserver_pixmap_t g_pixmaps[MAX_PIXMAPS];
static uint32_t g_next_window_id = 1;
static uint32_t g_next_pixmap_id = 1;
static int16_t g_next_z_order = 0;     /* Z-order counter */
static xserver_display_t g_display;
static uint32_t g_focused_window = 0;  /* Currently focused window ID */
static int g_input_fd = -1;            /* Input device fd */
static uint16_t g_modifiers = 0;       /* Current keyboard modifiers */
static int16_t g_mouse_x = 0;          /* Mouse position */
static int16_t g_mouse_y = 0;
static uint8_t g_mouse_buttons = 0;    /* Mouse button state */
static uint8_t g_compositor_dirty = 1; /* Needs recomposite */

/* NodGL buffer handles for windows (actual GPU textures) */
typedef struct {
    uint32_t window_id;
    uint32_t gfx_handle;      /* NodGL buffer handle */
    void *mapped_addr;        /* CPU-accessible address */
    uint32_t pitch;
} window_buffer_t;

static window_buffer_t g_window_buffers[MAX_WINDOWS];

/* Clipboard/Selection state */
typedef struct {
    uint32_t owner_window;
    uint8_t format;
    uint32_t data_len;
    uint8_t *data;
    uint32_t timestamp;
} selection_t;

static selection_t g_selections[3]; /* CLIPBOARD, PRIMARY, SECONDARY */

/* Drag and drop state */
typedef struct {
    uint8_t active;
    uint32_t source_window;
    uint8_t format;
    uint8_t allowed_ops;
    uint32_t data_len;
    uint8_t *data;
    uint32_t current_target;  /* Window under cursor */
} drag_state_t;

static drag_state_t g_drag_state;

/* External compositor state */
static uint32_t g_compositor_pid = 0;     /* PID of registered compositor */
static uint8_t g_external_compositor = 0; /* 1 if external compositor is active */

/* Send event to window's owner process via UserFS event node */
static void send_event_to_window(xserver_window_t *win, const void *event_data, size_t event_size) {
    if (!win || !event_data) return;
    
    /* TODO: Queue event for the window's owner process
     * For now, we'll implement a simple ring buffer per-window in future iteration
     * This is a stub that shows the structure */
    (void)event_size;
}

/* Process input events from $/dev/input/event0 */
static void process_input_events(void) {
    if (g_input_fd < 0) return;
    
    struct input_event {
        uint16_t type;
        uint16_t code;
        uint32_t value;
    } ev;
    
    while (read(g_input_fd, &ev, sizeof(ev)) == sizeof(ev)) {
        xserver_window_t *focused = NULL;
        
        /* Find focused window */
        if (g_focused_window != 0) {
            for (int i = 0; i < MAX_WINDOWS; i++) {
                if (g_windows[i].id == g_focused_window) {
                    focused = &g_windows[i];
                    break;
                }
            }
        }
        
        /* Scancode definitions (Linux input event codes) */
        #define KEY_LSHIFT  42
        #define KEY_RSHIFT  54
        #define KEY_LCTRL   29
        #define KEY_RCTRL   97
        #define KEY_LALT    56
        #define KEY_RALT    100
        #define KEY_ESC     1
        
        /* Event types from ModuOS input system */
        switch (ev.type) {
            case 1: /* EV_KEY - Keyboard event */
            {
                /* Update modifiers using named constants instead of magic numbers */
                if (ev.code == KEY_LSHIFT || ev.code == KEY_RSHIFT) {
                    if (ev.value) g_modifiers |= XAPI_MOD_SHIFT;
                    else g_modifiers &= ~XAPI_MOD_SHIFT;
                } else if (ev.code == KEY_LCTRL || ev.code == KEY_RCTRL) {
                    if (ev.value) g_modifiers |= XAPI_MOD_CTRL;
                    else g_modifiers &= ~XAPI_MOD_CTRL;
                } else if (ev.code == KEY_LALT || ev.code == KEY_RALT) {
                    if (ev.value) g_modifiers |= XAPI_MOD_ALT;
                    else g_modifiers &= ~XAPI_MOD_ALT;
                }
                
                /* Send key event to focused window */
                if (focused) {
                    xapi_event_key_t key_ev;
                    memset(&key_ev, 0, sizeof(key_ev));
                    key_ev.hdr.type = ev.value ? XAPI_EVENT_KEY_PRESS : XAPI_EVENT_KEY_RELEASE;
                    key_ev.hdr.size = sizeof(key_ev);
                    key_ev.hdr.window_id = focused->id;
                    key_ev.keycode = ev.code;
                    key_ev.modifiers = g_modifiers;
                    key_ev.unicode = 0; /* TODO: Scancode to Unicode translation */
                    
                    send_event_to_window(focused, &key_ev, sizeof(key_ev));
                }
                break;
            }
            
            case 2: /* EV_REL - Mouse relative movement */
            {
                if (ev.code == 0) { /* REL_X */
                    g_mouse_x += (int16_t)ev.value;
                    if (g_mouse_x < 0) g_mouse_x = 0;
                    if (g_mouse_x >= (int16_t)g_display.width) g_mouse_x = g_display.width - 1;
                } else if (ev.code == 1) { /* REL_Y */
                    g_mouse_y += (int16_t)ev.value;
                    if (g_mouse_y < 0) g_mouse_y = 0;
                    if (g_mouse_y >= (int16_t)g_display.height) g_mouse_y = g_display.height - 1;
                }
                
                /* Update drag state if dragging */
                if (g_drag_state.active) {
                    update_drag_motion();
                }
                
                /* Find window under cursor and send mouse move event */
                for (int i = MAX_WINDOWS - 1; i >= 0; i--) {
                    xserver_window_t *win = &g_windows[i];
                    if (!win->id || !win->mapped) continue;
                    
                    if (g_mouse_x >= win->x && g_mouse_x < win->x + win->width &&
                        g_mouse_y >= win->y && g_mouse_y < win->y + win->height) {
                        
                        xapi_event_mouse_t mouse_ev;
                        memset(&mouse_ev, 0, sizeof(mouse_ev));
                        mouse_ev.hdr.type = XAPI_EVENT_MOUSE_MOVE;
                        mouse_ev.hdr.size = sizeof(mouse_ev);
                        mouse_ev.hdr.window_id = win->id;
                        mouse_ev.x = g_mouse_x - win->x;
                        mouse_ev.y = g_mouse_y - win->y;
                        mouse_ev.root_x = g_mouse_x;
                        mouse_ev.root_y = g_mouse_y;
                        mouse_ev.buttons = g_mouse_buttons;
                        mouse_ev.modifiers = g_modifiers;
                        
                        send_event_to_window(win, &mouse_ev, sizeof(mouse_ev));
                        break;
                    }
                }
                break;
            }
            
            case 4: /* EV_MSC - Mouse button event */
            {
                uint8_t button_bit = 0;
                if (ev.code == 0x110) button_bit = XAPI_MOUSE_BUTTON1;      /* BTN_LEFT */
                else if (ev.code == 0x111) button_bit = XAPI_MOUSE_BUTTON2; /* BTN_RIGHT */
                else if (ev.code == 0x112) button_bit = XAPI_MOUSE_BUTTON3; /* BTN_MIDDLE */
                
                if (button_bit) {
                    if (ev.value) g_mouse_buttons |= button_bit;
                    else {
                        g_mouse_buttons &= ~button_bit;
                        
                        /* End drag on button release */
                        if (g_drag_state.active) {
                            end_drag_drop();
                            break;
                        }
                    }
                    
                    /* Find window under cursor */
                    for (int i = MAX_WINDOWS - 1; i >= 0; i--) {
                        xserver_window_t *win = &g_windows[i];
                        if (!win->id || !win->mapped) continue;
                        
                        if (g_mouse_x >= win->x && g_mouse_x < win->x + win->width &&
                            g_mouse_y >= win->y && g_mouse_y < win->y + win->height) {
                            
                            /* Set focus on click */
                            if (ev.value && g_focused_window != win->id) {
                                if (g_focused_window != 0) {
                                    /* Send focus out to old window */
                                    xapi_msg_hdr_t focus_out;
                                    memset(&focus_out, 0, sizeof(focus_out));
                                    focus_out.type = XAPI_EVENT_FOCUS_OUT;
                                    focus_out.size = sizeof(focus_out);
                                    focus_out.window_id = g_focused_window;
                                    /* TODO: Send to old focused window */
                                }
                                
                                g_focused_window = win->id;
                                win->focused = 1;
                                
                                /* Send focus in to new window */
                                xapi_msg_hdr_t focus_in;
                                memset(&focus_in, 0, sizeof(focus_in));
                                focus_in.type = XAPI_EVENT_FOCUS_IN;
                                focus_in.size = sizeof(focus_in);
                                focus_in.window_id = win->id;
                                send_event_to_window(win, &focus_in, sizeof(focus_in));
                            }
                            
                            xapi_event_mouse_t mouse_ev;
                            memset(&mouse_ev, 0, sizeof(mouse_ev));
                            mouse_ev.hdr.type = ev.value ? XAPI_EVENT_MOUSE_PRESS : XAPI_EVENT_MOUSE_RELEASE;
                            mouse_ev.hdr.size = sizeof(mouse_ev);
                            mouse_ev.hdr.window_id = win->id;
                            mouse_ev.x = g_mouse_x - win->x;
                            mouse_ev.y = g_mouse_y - win->y;
                            mouse_ev.root_x = g_mouse_x;
                            mouse_ev.root_y = g_mouse_y;
                            mouse_ev.buttons = g_mouse_buttons;
                            mouse_ev.button = button_bit;
                            mouse_ev.modifiers = g_modifiers;
                            
                            send_event_to_window(win, &mouse_ev, sizeof(mouse_ev));
                            break;
                        }
                    }
                }
                break;
            }
        }
    }
}

/* Initialize NodGL display */
static int init_display() {
    printf("[FlareXd] Initializing NodGL graphics...\n");
    
    /* Create NodGL device and context */
    int result = NodGL_CreateDevice(
        NodGL_FEATURE_LEVEL_1_0,
        &g_display.device,
        &g_display.context,
        &g_display.feature_level
    );
    
    if (result != NodGL_OK) {
        printf("[FlareXd] ERROR: NodGL_CreateDevice failed: %s\n", 
               NodGL_GetErrorString(result));
        return -1;
    }
    
    /* Get device capabilities */
    NodGL_DeviceCaps caps;
    if (NodGL_GetDeviceCaps(g_display.device, &caps) != NodGL_OK) {
        printf("[FlareXd] ERROR: Could not get device caps\n");
        NodGL_ReleaseDevice(g_display.device);
        return -1;
    }
    
    g_display.width = caps.screen_width;
    g_display.height = caps.screen_height;
    
    printf("[FlareXd] Display initialized: %ux%u\n", g_display.width, g_display.height);
    printf("[FlareXd] Adapter: %s\n", caps.adapter_name);
    printf("[FlareXd] Feature level: 0x%x\n", g_display.feature_level);
    printf("[FlareXd] Hardware accel: %s\n", 
           (caps.capabilities & NodGL_CAP_HARDWARE_ACCEL) ? "YES" : "NO");
    
    /* Clear screen to dark blue using NodGL (INSTANT!) */
    NodGL_ClearContext(g_display.context, NodGL_CLEAR_COLOR, 0xFF1A1A2E, 1.0f, 0);
    NodGL_PresentContext(g_display.context, 0);
    
    return 0;
}

/* Find free window slot */
static xserver_window_t *alloc_window() {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (g_windows[i].id == 0) {
            memset(&g_windows[i], 0, sizeof(xserver_window_t));
            g_windows[i].id = g_next_window_id++;
            return &g_windows[i];
        }
    }
    return NULL;
}

/* Allocate GPU buffer for window using gfx2d */
static int allocate_window_buffer(xserver_window_t *win) {
    /* Find free buffer slot */
    window_buffer_t *buf = NULL;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (g_window_buffers[i].window_id == 0) {
            buf = &g_window_buffers[i];
            break;
        }
    }
    
    if (!buf) return -1;
    
    /* Allocate GPU buffer via gfx2d */
    gfx2d_t gfx;
    int fd = open("$/dev/graphics/video0", O_RDWR, 0);
    if (fd < 0) return -1;
    
    gfx.fd = fd;
    gfx.cmdbuf = NULL;
    gfx.cmdbuf_size = 0;
    gfx.cmdbuf_used = 0;
    gfx.cmd_count = 0;
    
    uint32_t size = win->width * win->height * 4;
    uint32_t handle = 0;
    uint32_t pitch = 0;
    
    int rc = gfx2d_alloc_buf(&gfx, size, MD64API_GRP_FMT_XRGB8888, &handle, &pitch);
    if (rc != 0) {
        close(fd);
        return -1;
    }
    
    /* Map the buffer for CPU access */
    void *addr = NULL;
    uint32_t map_size = 0;
    uint32_t map_pitch = 0;
    uint32_t fmt = 0;
    
    rc = gfx2d_map_buf(&gfx, handle, &addr, &map_size, &map_pitch, &fmt);
    close(fd);
    
    if (rc != 0) return -1;
    
    /* Store buffer info */
    buf->window_id = win->id;
    buf->gfx_handle = handle;
    buf->mapped_addr = addr;
    buf->pitch = pitch;
    
    /* Clear to white */
    if (addr) {
        memset(addr, 0xFF, size);
    }
    
    /* Update window buffer pointer */
    win->buffer = (uint32_t *)addr;
    
    return 0;
}

/* Find window by ID */
static xserver_window_t *find_window(uint32_t id) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (g_windows[i].id == id) {
            return &g_windows[i];
        }
    }
    return NULL;
}

/* Composite window using NodGL (HARDWARE ACCELERATED!) */
/* Draw text using FNT font - OPTIMIZED: Batch horizontal runs instead of 1x1 pixels */
static void draw_text_fnt(fnt_font_t *font, const char *text, int x, int y, uint32_t color) {
    if (!font || !text) return;
    
    int cursor_x = x;
    for (const char *p = text; *p; p++) {
        uint8_t ch = (uint8_t)*p;
        
        /* Get glyph data from FNT font */
        const fnt_glyph_t *glyph = fnt_get_glyph(font, ch);
        if (!glyph) continue;
        
        /* Render glyph - batch consecutive pixels on same scanline */
        for (int gy = 0; gy < glyph->bitmap_height; gy++) {
            int run_start = -1;
            int run_len = 0;
            
            for (int gx = 0; gx <= glyph->bitmap_width; gx++) {
                int is_set = 0;
                
                if (gx < glyph->bitmap_width && glyph->bitmap) {
                    /* Check if pixel is set in glyph bitmap */
                    int byte_idx = (gy * glyph->bitmap_width + gx) / 8;
                    int bit_idx = (gy * glyph->bitmap_width + gx) % 8;
                    is_set = (glyph->bitmap[byte_idx] & (1 << (7 - bit_idx))) != 0;
                }
                
                if (is_set) {
                    if (run_start == -1) {
                        run_start = gx;
                        run_len = 1;
                    } else {
                        run_len++;
                    }
                } else if (run_start != -1) {
                    /* End of run - draw it */
                    NodGL_FillRectContext(g_display.context,
                                        cursor_x + run_start,
                                        y + gy,
                                        run_len, 1, color);
                    run_start = -1;
                    run_len = 0;
                }
            }
        }
        
        cursor_x += glyph->width;
    }
}

/* HARDWARE ACCELERATED compositor using gfx2d_blit_buf! */
static void composite_window(xserver_window_t *win, fnt_font_t *font) {
    if (!win->mapped || !win->buffer) return;

    /* If external compositor is active, let it handle rendering */
    if (g_external_compositor) {
        return;
    }

    /* Find GPU buffer for this window */
    window_buffer_t *buf = NULL;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (g_window_buffers[i].window_id == win->id) {
            buf = &g_window_buffers[i];
            break;
        }
    }
    
    if (!buf || buf->gfx_handle == 0) {
        /* No GPU buffer - skip (shouldn't happen if allocated properly) */
        return;
    }
    
    /* ONE GPU blit call for the entire window! */
    gfx2d_t gfx;
    int fd = open("$/dev/graphics/video0", O_RDWR, 0);
    if (fd < 0) return;
    
    gfx.fd = fd;
    gfx.cmdbuf = NULL;
    gfx.cmdbuf_size = 0;
    gfx.cmdbuf_used = 0;
    gfx.cmd_count = 0;
    
    /* Blit window buffer to screen in ONE call (not W*H calls!) */
    gfx2d_blit_buf(&gfx, buf->gfx_handle,
                   0, 0,                    /* src x,y */
                   win->x, win->y,          /* dst x,y */
                   win->width, win->height, /* size */
                   buf->pitch,              /* pitch */
                   MD64API_GRP_FMT_XRGB8888);
    
    close(fd);
    
    /* Draw decorations if enabled (title bar using FNT font!) */
    if (win->decorations.title_bar && font) {
        int title_height = 24;
        
        /* Draw title bar background */
        NodGL_FillRectContext(g_display.context, 
                            win->x, win->y - title_height,
                            win->width, title_height,
                            win->decorations.title_bg_color);
        
        /* Render window title using FNT font */
        draw_text_fnt(font, win->title, 
                     win->x + 8, win->y - title_height + 4,
                     win->decorations.title_fg_color);
    }
    
    /* Draw border if enabled */
    if (win->decorations.border && win->decorations.border_width > 0) {
        uint8_t bw = win->decorations.border_width;
        uint32_t bc = win->decorations.border_color;
        
        /* Draw border as 4 rects (top, bottom, left, right) */
        NodGL_FillRectContext(g_display.context, win->x - bw, win->y - bw, 
                            win->width + bw*2, bw, bc);
        NodGL_FillRectContext(g_display.context, win->x - bw, win->y + win->height, 
                            win->width + bw*2, bw, bc);
        NodGL_FillRectContext(g_display.context, win->x - bw, win->y, 
                            bw, win->height, bc);
        NodGL_FillRectContext(g_display.context, win->x + win->width, win->y, 
                            bw, win->height, bc);
    }
}

/* Handle window create */
static void handle_create_window(const xapi_win_create_t *msg, uint32_t client_pid) {
    printf("[FlareXd] CREATE_WINDOW from PID %u: %ux%u\n", 
           client_pid, msg->width, msg->height);
    
    /* SECURITY: Validate window dimensions to prevent malloc bombs */
    if (msg->width == 0 || msg->height == 0) {
        printf("[FlareXd] ERROR: Invalid window size (%ux%u)\n", msg->width, msg->height);
        return;
    }
    
    if (msg->width > 4096 || msg->height > 4096) {
        printf("[FlareXd] ERROR: Window too large (%ux%u), max is 4096x4096\n", 
               msg->width, msg->height);
        return;
    }
    
    /* Check if allocation would be too large (prevent OOM) */
    size_t buf_size = (size_t)msg->width * (size_t)msg->height * 4;
    if (buf_size > 64 * 1024 * 1024) { /* 64MB max per window */
        printf("[FlareXd] ERROR: Window buffer would be %zu MB (max 64MB)\n", 
               buf_size / (1024*1024));
        return;
    }
    
    xserver_window_t *win = alloc_window();
    if (!win) {
        printf("[FlareXd] ERROR: No free window slots (max %d)\n", MAX_WINDOWS);
        return;
    }
    
    win->owner_pid = client_pid;
    win->width = msg->width;
    win->height = msg->height;
    win->type = msg->win_type;
    win->x = 100;  /* Default position */
    win->y = 100;
    win->mapped = 0;
    
    /* Try GPU buffer first (FAST!), fallback to malloc if it fails */
    if (allocate_window_buffer(win) != 0) {
        printf("[FlareXd] GPU allocation failed, using malloc fallback\n");
        win->buffer = (uint32_t *)malloc(buf_size);
        if (!win->buffer) {
            printf("[FlareXd] ERROR: Could not allocate window buffer\n");
            win->id = 0;
            return;
        }
    }
    
    /* Clear to white. */
    for (uint32_t i = 0; i < (uint32_t)msg->width * msg->height; i++)
        win->buffer[i] = 0xFFFFFFFF;

    printf("[FlareXd] Created window ID=%u\n", win->id);

    /*
     * Echo the assigned ID back through the windows node so that
     * FlareXCreateWindow() can read it without a separate reply channel.
     * We reuse xapi_msg_hdr_t with window_id set; the client only inspects
     * that field.
     */
    int wfd = open(NODE_DEV_WINDOWS, O_WRONLY, 0);
    if (wfd >= 0) {
        xapi_msg_hdr_t reply;
        memset(&reply, 0, sizeof(reply));
        reply.type      = XAPI_WIN_CREATE; /* same type so client recognises it */
        reply.size      = sizeof(reply);
        reply.window_id = win->id;
        (void)write(wfd, &reply, sizeof(reply));
        close(wfd);
    }
}

/* Handle window map */
static void handle_map_window(const xapi_win_geometry_t *msg) {
    xserver_window_t *win = find_window(msg->hdr.window_id);
    if (!win) {
        printf("[FlareXd] ERROR: Window %u not found\n", msg->hdr.window_id);
        return;
    }
    
    win->x = msg->x;
    win->y = msg->y;
    win->mapped = 1;
    
    printf("[FlareXd] Mapped window %u at (%d, %d)\n", 
           win->id, win->x, win->y);
    
    /* Note: font will be NULL here since we're not in main loop context
     * Compositing will happen on next commit */
}

/* Handle draw rectangle (still uses window buffer for now) */
static void handle_draw_rect(const xapi_cmd_rect_t *msg) {
    xserver_window_t *win = find_window(msg->hdr.window_id);
    if (!win || !win->buffer) return;

    int x0 = msg->x, y0 = msg->y;
    int x1 = x0 + (int)msg->w;
    int y1 = y0 + (int)msg->h;

    /* Clip to window bounds. */
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)win->width)  x1 = (int)win->width;
    if (y1 > (int)win->height) y1 = (int)win->height;

    /* Draw to window buffer (will be composited on commit) */
    for (int y = y0; y < y1; y++) {
        uint32_t *row = win->buffer + y * win->width;
        for (int x = x0; x < x1; x++)
            row[x] = msg->color;
    }
}

/* Handle commit (present window) using NodGL */
static void handle_commit(const xapi_cmd_commit_t *msg) {
    xserver_window_t *win = find_window(msg->hdr.window_id);
    if (!win) return;

    /* Redraw background */
    NodGL_ClearContext(g_display.context, NodGL_CLEAR_COLOR, 0xFF1A1A2E, 1.0f, 0);
    
    /* Composite all windows in order (using sys_font loaded at line 1583) */
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (g_windows[i].id != 0 && g_windows[i].mapped) {
            composite_window(&g_windows[i], NULL);
        }
    }

    /* Present to screen (INSTANT GPU flip!) */
    NodGL_PresentContext(g_display.context, 0);
}

/* Draw line into a window buffer using Bresenham's algorithm. */
static void handle_draw_line(const xapi_cmd_line_t *msg) {
    xserver_window_t *win = find_window(msg->hdr.window_id);
    if (!win || !win->buffer) return;

    int x0 = msg->x1, y0 = msg->y1;
    int x1 = msg->x2, y1 = msg->y2;
    int dx = x1 - x0; if (dx < 0) dx = -dx;
    int dy = y1 - y0; if (dy < 0) dy = -dy;
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    for (;;) {
        if (x0 >= 0 && y0 >= 0 && x0 < (int)win->width && y0 < (int)win->height)
            win->buffer[y0 * win->width + x0] = msg->color;
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

/* Set a single pixel inside a window buffer. */
static void handle_draw_pixel(const xapi_cmd_pixel_t *msg) {
    xserver_window_t *win = find_window(msg->hdr.window_id);
    if (!win || !win->buffer) return;
    if (msg->x < 0 || msg->y < 0) return;
    if ((uint16_t)msg->x >= win->width || (uint16_t)msg->y >= win->height) return;
    win->buffer[(uint16_t)msg->y * win->width + (uint16_t)msg->x] = msg->color;
}

/* Render a single glyph into the window buffer. */
static void render_glyph(xserver_window_t *win, fnt_glyph_t *g,
                          int x, int y, uint32_t fg, uint32_t bg, uint8_t scale) {
    if (scale == 0) scale = 1;
    for (int gy = 0; gy < g->bitmap_height; gy++) {
        for (int gx = 0; gx < g->bitmap_width; gx++) {
            int bit = fnt_get_pixel(g, gx, gy);
            uint32_t color = bit ? fg : bg;
            if (!bit && bg == 0) continue; /* transparent background */
            for (int sy = 0; sy < scale; sy++) {
                int py = y + gy * scale + sy;
                if (py < 0 || py >= (int)win->height) continue;
                for (int sx = 0; sx < scale; sx++) {
                    int px = x + gx * scale + sx;
                    if (px < 0 || px >= (int)win->width) continue;
                    win->buffer[py * win->width + px] = color;
                }
            }
        }
    }
}

/* Fallback text renderer using a tiny 8x8 built-in font (ASCII 32-126). */
static void render_text_builtin(xserver_window_t *win, int x, int y,
                                const char *text, uint16_t len, uint32_t color) {
    /* 8x8 1bpp font — printable ASCII subset, stored as 8 bytes per char. */
    static const uint8_t font8x8[95][8] = {
        {0,0,0,0,0,0,0,0},         /* ' ' */
        {0x18,0x18,0x18,0x18,0,0,0x18,0},  /* ! */
        {0x6C,0x6C,0x6C,0,0,0,0,0},        /* " */
        {0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0}, /* # */
        {0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0}, /* $ */
        {0,0x63,0x33,0x18,0x0C,0x66,0x63,0},    /* % */
        {0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0}, /* & */
        {0x18,0x18,0x18,0,0,0,0,0},             /* ' */
        {0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0}, /* ( */
        {0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0}, /* ) */
        {0,0x36,0x1C,0x7F,0x1C,0x36,0,0},       /* * */
        {0,0x18,0x18,0x7E,0x18,0x18,0,0},        /* + */
        {0,0,0,0,0,0x18,0x18,0x0C},              /* , */
        {0,0,0,0x7E,0,0,0,0},                    /* - */
        {0,0,0,0,0,0,0x18,0},                    /* . */
        {0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0}, /* / */
        {0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0}, /* 0 */
        {0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0}, /* 1 */
        {0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0}, /* 2 */
        {0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0}, /* 3 */
        {0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0}, /* 4 */
        {0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0}, /* 5 */
        {0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0}, /* 6 */
        {0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0}, /* 7 */
        {0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0}, /* 8 */
        {0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0}, /* 9 */
        {0,0x18,0,0,0,0x18,0,0},                /* : */
        {0,0x18,0,0,0,0x18,0x18,0x0C},          /* ; */
        {0x30,0x18,0x0C,0x06,0x0C,0x18,0x30,0}, /* < */
        {0,0,0x7E,0,0x7E,0,0,0},                /* = */
        {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0}, /* > */
        {0x1E,0x33,0x30,0x18,0x0C,0,0x0C,0},    /* ? */
        {0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0}, /* @ */
        {0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0}, /* A */
        {0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0}, /* B */
        {0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0}, /* C */
        {0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0}, /* D */
        {0x7F,0x46,0x16,0x1E,0x16,0x46,0x7F,0}, /* E */
        {0x7F,0x46,0x16,0x1E,0x16,0x06,0x0F,0}, /* F */
        {0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0}, /* G */
        {0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0}, /* H */
        {0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0}, /* I */
        {0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0}, /* J */
        {0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0}, /* K */
        {0x0F,0x06,0x06,0x06,0x46,0x66,0x7F,0}, /* L */
        {0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0}, /* M */
        {0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0}, /* N */
        {0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0}, /* O */
        {0x3F,0x66,0x66,0x3E,0x06,0x06,0x0F,0}, /* P */
        {0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0}, /* Q */
        {0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0}, /* R */
        {0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0}, /* S */
        {0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0}, /* T */
        {0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0}, /* U */
        {0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0}, /* V */
        {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0}, /* W */
        {0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0}, /* X */
        {0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0}, /* Y */
        {0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0}, /* Z */
        {0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0}, /* [ */
        {0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0}, /* \ */
        {0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0}, /* ] */
        {0x08,0x1C,0x36,0x63,0,0,0,0},          /* ^ */
        {0,0,0,0,0,0,0,0x7F},                   /* _ */
        {0x0C,0x0C,0x18,0,0,0,0,0},             /* ` */
        {0,0,0x1E,0x30,0x3E,0x33,0x6E,0},       /* a */
        {0x07,0x06,0x06,0x3E,0x66,0x66,0x3B,0}, /* b */
        {0,0,0x1E,0x33,0x03,0x33,0x1E,0},       /* c */
        {0x38,0x30,0x30,0x3E,0x33,0x33,0x6E,0}, /* d */
        {0,0,0x1E,0x33,0x3F,0x03,0x1E,0},       /* e */
        {0x1C,0x36,0x06,0x0F,0x06,0x06,0x0F,0}, /* f */
        {0,0,0x6E,0x33,0x33,0x3E,0x30,0x1F},    /* g */
        {0x07,0x06,0x36,0x6E,0x66,0x66,0x67,0}, /* h */
        {0x18,0,0x0E,0x18,0x18,0x18,0x3F,0},    /* i */
        {0x30,0,0x30,0x30,0x30,0x33,0x33,0x1E}, /* j */
        {0x07,0x06,0x66,0x36,0x1E,0x36,0x67,0}, /* k */
        {0x0E,0x18,0x18,0x18,0x18,0x18,0x3F,0}, /* l */
        {0,0,0x33,0x7F,0x7F,0x6B,0x63,0},       /* m */
        {0,0,0x1F,0x33,0x33,0x33,0x33,0},       /* n */
        {0,0,0x1E,0x33,0x33,0x33,0x1E,0},       /* o */
        {0,0,0x3B,0x66,0x66,0x3E,0x06,0x0F},    /* p */
        {0,0,0x6E,0x33,0x33,0x3E,0x30,0x78},    /* q */
        {0,0,0x3B,0x6E,0x66,0x06,0x0F,0},       /* r */
        {0,0,0x3E,0x03,0x1E,0x30,0x1F,0},       /* s */
        {0x08,0x0C,0x3E,0x0C,0x0C,0x2C,0x18,0}, /* t */
        {0,0,0x33,0x33,0x33,0x33,0x6E,0},       /* u */
        {0,0,0x33,0x33,0x33,0x1E,0x0C,0},       /* v */
        {0,0,0x63,0x6B,0x7F,0x7F,0x36,0},       /* w */
        {0,0,0x63,0x36,0x1C,0x36,0x63,0},       /* x */
        {0,0,0x33,0x33,0x33,0x3E,0x30,0x1F},    /* y */
        {0,0,0x3F,0x19,0x0C,0x00,0x3F,0},       /* z */
        {0x38,0x0C,0x0C,0x07,0x0C,0x0C,0x38,0}, /* { */
        {0x18,0x18,0x18,0,0x18,0x18,0x18,0},    /* | */
        {0x07,0x0C,0x0C,0x38,0x0C,0x0C,0x07,0}, /* } */
        {0x6E,0x3B,0,0,0,0,0,0},                /* ~ */
    };

    for (uint16_t ci = 0; ci < len; ci++) {
        unsigned char ch = (unsigned char)text[ci];
        if (ch < 32 || ch > 126) { x += 8; continue; }
        const uint8_t *glyph = font8x8[ch - 32];
        for (int gy = 0; gy < 8; gy++) {
            int py = y + gy;
            if (py < 0 || py >= (int)win->height) continue;
            for (int gx = 0; gx < 8; gx++) {
                if (!(glyph[gy] & (0x80 >> gx))) continue;
                int px = x + gx;
                if (px < 0 || px >= (int)win->width) continue;
                win->buffer[py * win->width + px] = color;
            }
        }
        x += 8;
    }
}

/* Handle draw text (XAPI_CMD_TEXT). */
static void handle_draw_text(const void *buf, size_t len) {
    if (len < sizeof(xapi_cmd_text_t)) return;
    const xapi_cmd_text_t *msg = (const xapi_cmd_text_t *)buf;
    xserver_window_t *win = find_window(msg->hdr.window_id);
    if (!win || !win->buffer) return;

    uint16_t tlen = msg->text_len;
    if ((size_t)(sizeof(xapi_cmd_text_t) + tlen) > len) tlen = 0;
    const char *text = (const char *)buf + sizeof(xapi_cmd_text_t);

    render_text_builtin(win, msg->x, msg->y, text, tlen, msg->color);
}

/* Handle draw text with FNT font (XAPI_CMD_TEXT_FNT). */
static void handle_draw_text_fnt(const void *buf, size_t len,
                                 fnt_font_t *font) {
    if (len < sizeof(xapi_cmd_text_fnt_t)) return;
    const xapi_cmd_text_fnt_t *msg = (const xapi_cmd_text_fnt_t *)buf;
    xserver_window_t *win = find_window(msg->hdr.window_id);
    if (!win || !win->buffer) return;

    uint16_t tlen = msg->text_len;
    if ((size_t)(sizeof(xapi_cmd_text_fnt_t) + tlen) > len) tlen = 0;
    const char *text = (const char *)buf + sizeof(xapi_cmd_text_fnt_t);
    uint8_t scale = msg->scale ? msg->scale : 1;

    if (font) {
        int cx = msg->x;
        for (uint16_t ci = 0; ci < tlen; ci++) {
            fnt_glyph_t *g = fnt_get_glyph(font, (uint32_t)(unsigned char)text[ci]);
            if (!g) { cx += font->header.glyph_width * scale; continue; }
            render_glyph(win, g, cx, msg->y, msg->fg_color, msg->bg_color, scale);
            cx += g->width * scale;
        }
    } else {
        render_text_builtin(win, msg->x, msg->y, text, tlen, msg->fg_color);
    }
}

/* Handle window destroy. */
static void handle_destroy_window(uint32_t win_id) {
    xserver_window_t *win = find_window(win_id);
    if (!win) return;
    if (win->buffer) {
        free(win->buffer);
        win->buffer = NULL;
    }
    win->id = 0;
}

/* Handle window unmap (hide). */
static void handle_unmap_window(uint32_t win_id) {
    xserver_window_t *win = find_window(win_id);
    if (!win) return;
    win->mapped = 0;
}

/* Handle raise: move window to end of table (drawn last = on top). */
static void handle_raise_window(uint32_t win_id) {
    int idx = -1;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (g_windows[i].id == win_id) { idx = i; break; }
    }
    if (idx < 0) return;

    xserver_window_t tmp = g_windows[idx];
    for (int i = idx; i < MAX_WINDOWS - 1; i++)
        g_windows[i] = g_windows[i + 1];
    g_windows[MAX_WINDOWS - 1] = tmp;
}

/* Handle window move. */
static void handle_move_window(const xapi_win_geometry_t *msg) {
    xserver_window_t *win = find_window(msg->hdr.window_id);
    if (!win) return;
    win->x = msg->x;
    win->y = msg->y;
}

/* Handle window resize. */
static void handle_resize_window(const xapi_win_geometry_t *msg) {
    xserver_window_t *win = find_window(msg->hdr.window_id);
    if (!win) return;
    if (!msg->w || !msg->h) return;

    uint32_t *new_buf = (uint32_t *)malloc((size_t)msg->w * msg->h * 4);
    if (!new_buf) return;
    for (uint32_t i = 0; i < (uint32_t)msg->w * msg->h; i++)
        new_buf[i] = 0xFFFFFFFF;
    if (win->buffer) free(win->buffer);
    win->buffer = new_buf;
    win->width  = msg->w;
    win->height = msg->h;
}

/* Handle set window title. */
static void handle_set_title(const void *buf, size_t len) {
    if (len < sizeof(xapi_win_set_title_t)) return;
    const xapi_win_set_title_t *msg = (const xapi_win_set_title_t *)buf;
    xserver_window_t *win = find_window(msg->hdr.window_id);
    if (!win) return;

    uint16_t tlen = msg->title_len;
    size_t max = sizeof(win->title) - 1;
    if (tlen > max) tlen = (uint16_t)max;
    if ((size_t)(sizeof(xapi_win_set_title_t) + tlen) > len) tlen = 0;

    const char *title = (const char *)buf + sizeof(xapi_win_set_title_t);
    memcpy(win->title, title, tlen);
    win->title[tlen] = '\0';
}

/* Repaint the entire desktop using NodGL (background + all mapped windows).
 * Called after window moves/raises so the stacking order is reflected. */
static void __attribute__((unused)) repaint_all(void) {
    /* Redraw desktop background using NodGL clear */
    NodGL_ClearContext(g_display.context, NodGL_CLEAR_COLOR, 0xFF1A1A2E, 1.0f, 0);

    /* Composite all windows in stacking order */
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (g_windows[i].id != 0)
            composite_window(&g_windows[i], NULL);
    }

    /* Present to screen (INSTANT GPU flip!) */
    NodGL_PresentContext(g_display.context, 0);
}

/* Handle set window property */
static void handle_set_property(const void *buf, size_t len) {
    const xapi_win_set_property_t *msg = (const xapi_win_set_property_t *)buf;
    
    xserver_window_t *win = find_window(msg->hdr.window_id);
    if (!win) return;
    
    const uint8_t *data = (const uint8_t *)buf + sizeof(xapi_win_set_property_t);
    size_t data_len = len - sizeof(xapi_win_set_property_t);
    
    switch (msg->property) {
        case XAPI_PROP_WM_NAME:
            if (data_len > 0 && data_len < sizeof(win->title)) {
                memcpy(win->title, data, data_len);
                win->title[data_len] = '\0';
            }
            break;
            
        case XAPI_PROP_WM_CLASS:
            if (data_len > 0 && data_len < sizeof(win->wm_class)) {
                memcpy(win->wm_class, data, data_len);
                win->wm_class[data_len] = '\0';
            }
            break;
            
        case XAPI_PROP_WM_STATE:
            if (data_len >= sizeof(uint8_t)) {
                win->state = *(const uint8_t *)data;
            }
            break;
            
        case XAPI_PROP_WM_HINTS:
            if (data_len >= sizeof(xapi_window_hints_t)) {
                memcpy(&win->hints, data, sizeof(xapi_window_hints_t));
            }
            break;
            
        case XAPI_PROP_WM_TRANSIENT_FOR:
            if (data_len >= sizeof(uint32_t)) {
                win->transient_for = *(const uint32_t *)data;
            }
            break;
            
        case XAPI_PROP_WM_DESKTOP:
            if (data_len >= sizeof(uint32_t)) {
                win->desktop = *(const uint32_t *)data;
            }
            break;
            
        case XAPI_PROP_WM_PROTOCOLS:
            if (data_len >= sizeof(uint32_t)) {
                win->protocols = *(const uint32_t *)data;
            }
            break;
            
        default:
            break;
    }
}

/* Handle get window property (stub - would need reply mechanism) */
static void handle_get_property(const xapi_win_get_property_t *msg, uint32_t client_pid) {
    (void)client_pid;
    xserver_window_t *win = find_window(msg->hdr.window_id);
    if (!win) return;
    
    /* TODO: Implement reply mechanism via UserFS or separate response node */
    (void)msg->property;
}

/* Find pixmap by ID */
static xserver_pixmap_t *find_pixmap(uint32_t pixmap_id) {
    for (int i = 0; i < MAX_PIXMAPS; i++) {
        if (g_pixmaps[i].pixmap_id == pixmap_id) {
            return &g_pixmaps[i];
        }
    }
    return NULL;
}

/* Handle shared memory create */
static void handle_shm_create(const xapi_shm_create_t *msg, uint32_t client_pid) {
    /* Find free pixmap slot */
    xserver_pixmap_t *pixmap = NULL;
    for (int i = 0; i < MAX_PIXMAPS; i++) {
        if (g_pixmaps[i].pixmap_id == 0) {
            pixmap = &g_pixmaps[i];
            break;
        }
    }
    
    if (!pixmap) return; /* No free slots */
    
    /* TODO: In a real implementation, we'd use shmat() or similar to attach
     * to shared memory segment. For now, allocate local memory as placeholder */
    void *addr = malloc(msg->size);
    if (!addr) return;
    
    memset(addr, 0, msg->size);
    
    pixmap->pixmap_id = g_next_pixmap_id++;
    pixmap->shm_id = msg->shm_id;
    pixmap->addr = addr;
    pixmap->size = msg->size;
    pixmap->width = msg->width;
    pixmap->height = msg->height;
    pixmap->format = msg->format;
    pixmap->owner_pid = client_pid;
    
    /* TODO: Send response with pixmap_id back to client */
}

/* Handle shared memory attach */
static void handle_shm_attach(const xapi_shm_attach_t *msg, uint32_t client_pid) {
    (void)client_pid;
    /* TODO: Attach to existing shared memory segment */
    (void)msg;
}

/* Handle shared memory blit */
static void handle_shm_blit(const xapi_shm_blit_t *msg) {
    xserver_window_t *win = find_window(msg->hdr.window_id);
    xserver_pixmap_t *pixmap = find_pixmap(msg->pixmap_id);
    
    if (!win || !pixmap || !win->buffer || !pixmap->addr) return;
    
    /* Fast blit from shared memory pixmap to window buffer */
    uint32_t *src = (uint32_t *)pixmap->addr;
    uint32_t *dst = win->buffer;
    
    int16_t dx = msg->dst_x;
    int16_t dy = msg->dst_y;
    int16_t sx = msg->src_x;
    int16_t sy = msg->src_y;
    uint16_t w = msg->width;
    uint16_t h = msg->height;
    
    /* Clipping */
    if (dx < 0 || dy < 0 || dx + w > win->width || dy + h > win->height) return;
    if (sx < 0 || sy < 0 || sx + w > pixmap->width || sy + h > pixmap->height) return;
    
    for (uint16_t y = 0; y < h; y++) {
        uint32_t *src_row = &src[(sy + y) * pixmap->width + sx];
        uint32_t *dst_row = &dst[(dy + y) * win->width + dx];
        memcpy(dst_row, src_row, w * sizeof(uint32_t));
    }
    
    g_compositor_dirty = 1;
}

/* Handle shared memory detach */
static void handle_shm_detach(const xapi_shm_detach_t *msg) {
    xserver_pixmap_t *pixmap = find_pixmap(msg->pixmap_id);
    if (!pixmap) return;
    
    if (pixmap->addr) {
        free(pixmap->addr); /* In real impl, would call shmdt() */
        pixmap->addr = NULL;
    }
    
    memset(pixmap, 0, sizeof(xserver_pixmap_t));
}

/* Handle window restack */
static void handle_restack(const xapi_win_restack_t *msg) {
    xserver_window_t *win = find_window(msg->hdr.window_id);
    if (!win) return;
    
    switch (msg->operation) {
        case XAPI_STACK_RAISE:
            win->z_order = ++g_next_z_order;
            break;
            
        case XAPI_STACK_LOWER:
            win->z_order = --g_next_z_order;
            break;
            
        case XAPI_STACK_ABOVE:
        case XAPI_STACK_BELOW: {
            xserver_window_t *sibling = find_window(msg->sibling_id);
            if (sibling) {
                if (msg->operation == XAPI_STACK_ABOVE) {
                    win->z_order = sibling->z_order + 1;
                } else {
                    win->z_order = sibling->z_order - 1;
                }
            }
            break;
        }
    }
    
    g_compositor_dirty = 1;
}

/* Handle set decorations */
static void handle_set_decorations(const xapi_win_set_decorations_t *msg) {
    xserver_window_t *win = find_window(msg->hdr.window_id);
    if (!win) return;
    
    memcpy(&win->decorations, &msg->style, sizeof(xapi_decoration_style_t));
    g_compositor_dirty = 1;
}

/* Handle set opacity */
static void handle_set_opacity(const xapi_win_set_opacity_t *msg) {
    xserver_window_t *win = find_window(msg->hdr.window_id);
    if (!win) return;
    
    win->opacity = msg->opacity;
    g_compositor_dirty = 1;
}

/* Handle damage notification */
static void handle_damage(const xapi_win_damage_t *msg) {
    (void)msg;
    /* Mark region as needing recomposite */
    g_compositor_dirty = 1;
}

/* Handle selection own */
static void handle_selection_own(const xapi_selection_own_t *msg) {
    if (msg->selection >= 3) return;
    
    selection_t *sel = &g_selections[msg->selection];
    
    /* Free old data */
    if (sel->data) {
        free(sel->data);
        sel->data = NULL;
    }
    
    sel->owner_window = msg->hdr.window_id;
    sel->timestamp = msg->timestamp;
    sel->data_len = 0;
}

/* Handle selection request */
static void handle_selection_request(const xapi_selection_request_t *msg) {
    if (msg->selection >= 3) return;
    
    selection_t *sel = &g_selections[msg->selection];
    
    if (sel->owner_window == 0) return; /* No owner */
    
    /* Forward request to selection owner */
    xapi_selection_request_t req;
    memcpy(&req, msg, sizeof(req));
    req.hdr.window_id = sel->owner_window;
    
    xserver_window_t *owner = find_window(sel->owner_window);
    if (owner) {
        send_event_to_window(owner, &req, sizeof(req));
    }
}

/* Handle selection data */
static void handle_selection_data(const void *buf, size_t len) {
    const xapi_selection_data_t *msg = (const xapi_selection_data_t *)buf;
    
    if (msg->selection >= 3) return;
    if (len < sizeof(xapi_selection_data_t) + msg->data_len) return;
    
    selection_t *sel = &g_selections[msg->selection];
    
    /* Store selection data */
    if (sel->data) free(sel->data);
    
    sel->data = malloc(msg->data_len);
    if (sel->data) {
        memcpy(sel->data, (const uint8_t *)buf + sizeof(xapi_selection_data_t), msg->data_len);
        sel->data_len = msg->data_len;
        sel->format = msg->format;
    }
    
    /* Notify requestor window (TODO: track who requested) */
}

/* Handle drag begin */
static void handle_drag_begin(const void *buf, size_t len) {
    const xapi_drag_begin_t *msg = (const xapi_drag_begin_t *)buf;
    
    if (g_drag_state.active) return; /* Already dragging */
    
    g_drag_state.active = 1;
    g_drag_state.source_window = msg->hdr.window_id;
    g_drag_state.format = msg->format;
    g_drag_state.allowed_ops = msg->allowed_ops;
    g_drag_state.current_target = 0;
    
    /* Store drag data */
    if (len > sizeof(xapi_drag_begin_t)) {
        g_drag_state.data_len = msg->data_len;
        g_drag_state.data = malloc(msg->data_len);
        if (g_drag_state.data) {
            memcpy(g_drag_state.data, (const uint8_t *)buf + sizeof(xapi_drag_begin_t), msg->data_len);
        }
    }
}

/* Handle drag status */
static void handle_drag_status(const xapi_drag_status_t *msg) {
    (void)msg;
    /* Window accepting/rejecting drop - forward to source */
    if (!g_drag_state.active) return;
    
    xserver_window_t *source = find_window(g_drag_state.source_window);
    if (source) {
        send_event_to_window(source, msg, sizeof(*msg));
    }
}

/* Handle drag get data */
static void handle_drag_get_data(const xapi_drag_get_data_t *msg) {
    if (!g_drag_state.active) return;
    
    /* Send drag data to requesting window */
    xserver_window_t *win = find_window(msg->hdr.window_id);
    if (win && g_drag_state.data) {
        xapi_selection_data_t *response = malloc(sizeof(xapi_selection_data_t) + g_drag_state.data_len);
        if (response) {
            response->hdr.type = XAPI_SELECTION_DATA;
            response->hdr.size = sizeof(xapi_selection_data_t) + g_drag_state.data_len;
            response->hdr.window_id = msg->hdr.window_id;
            response->selection = XAPI_SELECTION_CLIPBOARD; /* Using clipboard for DnD */
            response->format = msg->format;
            response->data_len = g_drag_state.data_len;
            memcpy((uint8_t *)response + sizeof(xapi_selection_data_t), g_drag_state.data, g_drag_state.data_len);
            
            send_event_to_window(win, response, response->hdr.size);
            free(response);
        }
    }
}

/* Update drag state during mouse movement */
static void update_drag_motion(void) {
    if (!g_drag_state.active) return;
    
    /* Find window under cursor */
    uint32_t target = 0;
    for (int i = MAX_WINDOWS - 1; i >= 0; i--) {
        xserver_window_t *win = &g_windows[i];
        if (!win->id || !win->mapped) continue;
        
        if (g_mouse_x >= win->x && g_mouse_x < win->x + win->width &&
            g_mouse_y >= win->y && g_mouse_y < win->y + win->height) {
            target = win->id;
            break;
        }
    }
    
    /* Target changed? */
    if (target != g_drag_state.current_target) {
        /* Send leave to old target */
        if (g_drag_state.current_target != 0) {
            xserver_window_t *old = find_window(g_drag_state.current_target);
            if (old) {
                xapi_event_drag_leave_t leave;
                memset(&leave, 0, sizeof(leave));
                leave.hdr.type = XAPI_EVENT_DRAG_LEAVE;
                leave.hdr.size = sizeof(leave);
                leave.hdr.window_id = old->id;
                send_event_to_window(old, &leave, sizeof(leave));
            }
        }
        
        g_drag_state.current_target = target;
    }
    
    /* Send motion to current target */
    if (target != 0) {
        xserver_window_t *win = find_window(target);
        if (win) {
            xapi_event_drag_motion_t motion;
            memset(&motion, 0, sizeof(motion));
            motion.hdr.type = XAPI_EVENT_DRAG_MOTION;
            motion.hdr.size = sizeof(motion);
            motion.hdr.window_id = win->id;
            motion.x = g_mouse_x - win->x;
            motion.y = g_mouse_y - win->y;
            motion.format = g_drag_state.format;
            motion.allowed_ops = g_drag_state.allowed_ops;
            send_event_to_window(win, &motion, sizeof(motion));
        }
    }
}

/* Handle compositor register */
static void handle_compositor_register(const xapi_compositor_register_t *msg) {
    if (g_external_compositor) {
        printf("[FlareXd] Compositor already registered (PID %u)\n", g_compositor_pid);
        return;
    }
    
    g_compositor_pid = msg->compositor_pid;
    g_external_compositor = 1;
    g_compositor_dirty = 1;
    
    printf("[FlareXd] External compositor registered (PID %u)\n", g_compositor_pid);
}

/* Handle compositor unregister */
static void handle_compositor_unregister(void) {
    if (!g_external_compositor) return;
    
    printf("[FlareXd] Compositor unregistered (was PID %u)\n", g_compositor_pid);
    g_compositor_pid = 0;
    g_external_compositor = 0;
}

/* Handle compositor get windows */
static void handle_compositor_get_windows(uint32_t client_pid) {
    if (!g_external_compositor || client_pid != g_compositor_pid) return;
    
    /* Count mapped windows */
    uint32_t count = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (g_windows[i].id != 0 && g_windows[i].mapped) {
            count++;
        }
    }
    
    /* Build response */
    size_t resp_size = sizeof(xapi_compositor_window_list_t) + count * sizeof(xapi_window_info_t);
    uint8_t *resp_buf = malloc(resp_size);
    if (!resp_buf) return;
    
    xapi_compositor_window_list_t *resp = (xapi_compositor_window_list_t *)resp_buf;
    resp->hdr.type = XAPI_COMPOSITOR_GET_WINDOWS;
    resp->hdr.size = resp_size;
    resp->hdr.window_id = 0;
    resp->window_count = count;
    
    xapi_window_info_t *infos = (xapi_window_info_t *)(resp_buf + sizeof(xapi_compositor_window_list_t));
    int idx = 0;
    
    for (int i = 0; i < MAX_WINDOWS; i++) {
        xserver_window_t *win = &g_windows[i];
        if (win->id == 0 || !win->mapped) continue;
        
        /* Find GPU buffer handle */
        uint32_t gfx_handle = 0;
        for (int j = 0; j < MAX_WINDOWS; j++) {
            if (g_window_buffers[j].window_id == win->id) {
                gfx_handle = g_window_buffers[j].gfx_handle;
                break;
            }
        }
        
        infos[idx].window_id = win->id;
        infos[idx].x = win->x;
        infos[idx].y = win->y;
        infos[idx].width = win->width;
        infos[idx].height = win->height;
        infos[idx].z_order = win->z_order;
        infos[idx].mapped = win->mapped;
        infos[idx].opacity = win->opacity;
        infos[idx].gfx_handle = gfx_handle;
        idx++;
    }
    
    /* TODO: Send response to compositor via UserFS node */
    free(resp_buf);
}

/* Notify compositor of damage */
static void notify_compositor_damage(uint32_t window_id, int16_t x, int16_t y, uint16_t w, uint16_t h) {
    if (!g_external_compositor) return;
    
    xapi_event_compositor_damage_t damage;
    memset(&damage, 0, sizeof(damage));
    damage.hdr.type = XAPI_EVENT_COMPOSITOR_DAMAGE;
    damage.hdr.size = sizeof(damage);
    damage.hdr.window_id = window_id;
    damage.x = x;
    damage.y = y;
    damage.width = w;
    damage.height = h;
    
    /* TODO: Send to compositor process */
}

/* End drag operation */
static void end_drag_drop(void) {
    if (!g_drag_state.active) return;
    
    /* Send drop event to target */
    if (g_drag_state.current_target != 0) {
        xserver_window_t *win = find_window(g_drag_state.current_target);
        if (win) {
            xapi_event_drag_drop_t drop;
            memset(&drop, 0, sizeof(drop));
            drop.hdr.type = XAPI_EVENT_DRAG_DROP;
            drop.hdr.size = sizeof(drop);
            drop.hdr.window_id = win->id;
            drop.x = g_mouse_x - win->x;
            drop.y = g_mouse_y - win->y;
            drop.format = g_drag_state.format;
            drop.operation = XAPI_DRAG_COPY; /* Default to copy */
            send_event_to_window(win, &drop, sizeof(drop));
        }
    }
    
    /* Clean up */
    if (g_drag_state.data) {
        free(g_drag_state.data);
        g_drag_state.data = NULL;
    }
    
    memset(&g_drag_state, 0, sizeof(g_drag_state));
}

/* Handle client message (WM communication) */
static void handle_client_message(const xapi_client_message_t *msg) {
    xserver_window_t *win = find_window(msg->hdr.window_id);
    if (!win) return;
    
    switch (msg->message_type) {
        case XAPI_MSG_WM_CHANGE_STATE:
            /* Window manager requesting state change */
            if (msg->data[0] < 5) { /* Valid state */
                win->state = (uint8_t)msg->data[0];
                
                /* Send configure event to window */
                xapi_event_configure_t conf;
                memset(&conf, 0, sizeof(conf));
                conf.hdr.type = XAPI_WIN_CONFIGURE;
                conf.hdr.size = sizeof(conf);
                conf.hdr.window_id = win->id;
                conf.x = win->x;
                conf.y = win->y;
                conf.w = win->width;
                conf.h = win->height;
                conf.send_event = 1;
                send_event_to_window(win, &conf, sizeof(conf));
            }
            break;
            
        case XAPI_MSG_WM_TAKE_FOCUS:
            /* Window manager requesting focus change */
            if (g_focused_window != win->id) {
                /* Remove focus from old window */
                if (g_focused_window != 0) {
                    for (int i = 0; i < MAX_WINDOWS; i++) {
                        if (g_windows[i].id == g_focused_window) {
                            g_windows[i].focused = 0;
                            
                            xapi_msg_hdr_t focus_out;
                            memset(&focus_out, 0, sizeof(focus_out));
                            focus_out.type = XAPI_EVENT_FOCUS_OUT;
                            focus_out.size = sizeof(focus_out);
                            focus_out.window_id = g_focused_window;
                            send_event_to_window(&g_windows[i], &focus_out, sizeof(focus_out));
                            break;
                        }
                    }
                }
                
                /* Set new focus */
                g_focused_window = win->id;
                win->focused = 1;
                
                xapi_msg_hdr_t focus_in;
                memset(&focus_in, 0, sizeof(focus_in));
                focus_in.type = XAPI_EVENT_FOCUS_IN;
                focus_in.size = sizeof(focus_in);
                focus_in.window_id = win->id;
                send_event_to_window(win, &focus_in, sizeof(focus_in));
            }
            break;
            
        case XAPI_MSG_WM_DELETE_WINDOW:
            /* Window manager requesting window close - just forward to app */
            /* The app should handle this and call XAPI_WIN_DESTROY */
            break;
            
        case XAPI_MSG_WM_PING:
            /* Ping/pong for hang detection - echo back */
            break;
            
        default:
            break;
    }
}

/* Process a single message from a UserFS node. */
static void process_message(const void *buf, size_t len,
                            uint32_t client_pid, fnt_font_t *font) {
    if (len < sizeof(xapi_msg_hdr_t)) return;

    const xapi_msg_hdr_t *hdr = (const xapi_msg_hdr_t *)buf;

    switch (hdr->type) {
        case XAPI_WIN_CREATE:
            if (len >= sizeof(xapi_win_create_t))
                handle_create_window((const xapi_win_create_t *)buf, client_pid);
            break;
        case XAPI_WIN_DESTROY:
            handle_destroy_window(hdr->window_id);
            break;
        case XAPI_WIN_MAP:
            if (len >= sizeof(xapi_win_geometry_t))
                handle_map_window((const xapi_win_geometry_t *)buf);
            break;
        case XAPI_WIN_UNMAP:
            handle_unmap_window(hdr->window_id);
            break;
        case XAPI_WIN_RAISE:
            handle_raise_window(hdr->window_id);
            break;
        case XAPI_WIN_MOVE:
            if (len >= sizeof(xapi_win_geometry_t))
                handle_move_window((const xapi_win_geometry_t *)buf);
            break;
        case XAPI_WIN_RESIZE:
            if (len >= sizeof(xapi_win_geometry_t))
                handle_resize_window((const xapi_win_geometry_t *)buf);
            break;
        case XAPI_WIN_SET_TITLE:
            handle_set_title(buf, len);
            break;
        case XAPI_CMD_RECT:
            if (len >= sizeof(xapi_cmd_rect_t))
                handle_draw_rect((const xapi_cmd_rect_t *)buf);
            break;
        case XAPI_CMD_LINE:
            if (len >= sizeof(xapi_cmd_line_t))
                handle_draw_line((const xapi_cmd_line_t *)buf);
            break;
        case XAPI_CMD_PIXEL:
            if (len >= sizeof(xapi_cmd_pixel_t))
                handle_draw_pixel((const xapi_cmd_pixel_t *)buf);
            break;
        case XAPI_CMD_TEXT:
            handle_draw_text(buf, len);
            break;
        case XAPI_CMD_TEXT_FNT:
            handle_draw_text_fnt(buf, len, font);
            break;
        case XAPI_CMD_COMMIT:
            if (len >= sizeof(xapi_cmd_commit_t))
                handle_commit((const xapi_cmd_commit_t *)buf);
            break;
        
        /* Window Manager Protocol */
        case XAPI_WIN_SET_PROPERTY:
            if (len >= sizeof(xapi_win_set_property_t))
                handle_set_property(buf, len);
            break;
        case XAPI_WIN_GET_PROPERTY:
            if (len >= sizeof(xapi_win_get_property_t))
                handle_get_property((const xapi_win_get_property_t *)buf, client_pid);
            break;
        case XAPI_WIN_CLIENT_MESSAGE:
            if (len >= sizeof(xapi_client_message_t))
                handle_client_message((const xapi_client_message_t *)buf);
            break;
        
        /* Shared Memory */
        case XAPI_SHM_CREATE:
            if (len >= sizeof(xapi_shm_create_t))
                handle_shm_create((const xapi_shm_create_t *)buf, client_pid);
            break;
        case XAPI_SHM_ATTACH:
            if (len >= sizeof(xapi_shm_attach_t))
                handle_shm_attach((const xapi_shm_attach_t *)buf, client_pid);
            break;
        case XAPI_SHM_BLIT:
            if (len >= sizeof(xapi_shm_blit_t))
                handle_shm_blit((const xapi_shm_blit_t *)buf);
            break;
        case XAPI_SHM_DETACH:
            if (len >= sizeof(xapi_shm_detach_t))
                handle_shm_detach((const xapi_shm_detach_t *)buf);
            break;
        
        /* Compositor */
        case XAPI_WIN_RESTACK:
            if (len >= sizeof(xapi_win_restack_t))
                handle_restack((const xapi_win_restack_t *)buf);
            break;
        case XAPI_WIN_SET_DECORATIONS:
            if (len >= sizeof(xapi_win_set_decorations_t))
                handle_set_decorations((const xapi_win_set_decorations_t *)buf);
            break;
        case XAPI_WIN_SET_OPACITY:
            if (len >= sizeof(xapi_win_set_opacity_t))
                handle_set_opacity((const xapi_win_set_opacity_t *)buf);
            break;
        case XAPI_WIN_DAMAGE:
            if (len >= sizeof(xapi_win_damage_t))
                handle_damage((const xapi_win_damage_t *)buf);
            break;
        
        /* Clipboard/Selection */
        case XAPI_SELECTION_OWN:
            if (len >= sizeof(xapi_selection_own_t))
                handle_selection_own((const xapi_selection_own_t *)buf);
            break;
        case XAPI_SELECTION_REQUEST:
            if (len >= sizeof(xapi_selection_request_t))
                handle_selection_request((const xapi_selection_request_t *)buf);
            break;
        case XAPI_SELECTION_DATA:
            if (len >= sizeof(xapi_selection_data_t))
                handle_selection_data(buf, len);
            break;
        
        /* Drag and Drop */
        case XAPI_DRAG_BEGIN:
            if (len >= sizeof(xapi_drag_begin_t))
                handle_drag_begin(buf, len);
            break;
        case XAPI_DRAG_STATUS:
            if (len >= sizeof(xapi_drag_status_t))
                handle_drag_status((const xapi_drag_status_t *)buf);
            break;
        case XAPI_DRAG_GET_DATA:
            if (len >= sizeof(xapi_drag_get_data_t))
                handle_drag_get_data((const xapi_drag_get_data_t *)buf);
            break;
        
        /* External Compositor */
        case XAPI_COMPOSITOR_REGISTER:
            if (len >= sizeof(xapi_compositor_register_t))
                handle_compositor_register((const xapi_compositor_register_t *)buf);
            break;
        case XAPI_COMPOSITOR_UNREGISTER:
            if (len >= sizeof(xapi_compositor_unregister_t))
                handle_compositor_unregister();
            break;
        case XAPI_COMPOSITOR_GET_WINDOWS:
            if (len >= sizeof(xapi_compositor_get_windows_t))
                handle_compositor_get_windows(client_pid);
            break;
        case XAPI_COMPOSITOR_FRAME_DONE:
            /* Compositor finished a frame - could track FPS here */
            break;
            
        default:
            break;
    }
}

/* Per-node ring buffer for incoming client messages. */
/*
 * Ring buffers for incoming client messages. Allocated on the heap in
 * md_main to avoid inflating the BSS segment beyond the mapped region.
 */
#define NODE_RING_SIZE 4096

typedef struct {
    uint8_t  *buf;
    uint32_t  r;
    uint32_t  w;
    uint32_t  count;
    uint32_t  cap;
} node_ring_t;

static node_ring_t g_ring_gfx;
static node_ring_t g_ring_windows;

static void ring_push(node_ring_t *r, const uint8_t *data, size_t len) {
    if (!r->buf) return;
    for (size_t i = 0; i < len; i++) {
        if (r->count >= r->cap) break;
        r->buf[r->w] = data[i];
        r->w = (r->w + 1) % r->cap;
        r->count++;
    }
}

static size_t ring_pop(node_ring_t *r, uint8_t *out, size_t maxlen) {
    if (!r->buf) return 0;
    size_t n = 0;
    while (n < maxlen && r->count > 0) {
        out[n++] = r->buf[r->r];
        r->r = (r->r + 1) % r->cap;
        r->count--;
    }
    return n;
}

/* UserFS write callbacks — called by kernel when a client writes to the node. */
static ssize_t node_gfx_write(void *ctx, const void *buf, size_t count) {
    (void)ctx;
    ring_push(&g_ring_gfx, (const uint8_t *)buf, count);
    return (ssize_t)count;
}

static ssize_t node_windows_write(void *ctx, const void *buf, size_t count) {
    (void)ctx;
    ring_push(&g_ring_windows, (const uint8_t *)buf, count);
    return (ssize_t)count;
}

/* UserFS read callbacks — clients reading events get 0 for now (server pushes events). */
static ssize_t node_event_read(void *ctx, void *buf, size_t count) {
    (void)ctx; (void)buf; (void)count;
    return 0;
}

/* Initialize UserFS nodes */
static int init_userfs_nodes(void) {
    printf("[FlareXd] Registering UserFS nodes...\n");

    memset(&g_ring_gfx,     0, sizeof(g_ring_gfx));
    memset(&g_ring_windows, 0, sizeof(g_ring_windows));
    g_ring_gfx.buf     = (uint8_t *)malloc(NODE_RING_SIZE);
    g_ring_gfx.cap     = g_ring_gfx.buf     ? NODE_RING_SIZE : 0;
    g_ring_windows.buf = (uint8_t *)malloc(NODE_RING_SIZE);
    g_ring_windows.cap = g_ring_windows.buf ? NODE_RING_SIZE : 0;

    {
        userfs_user_node_t node;
        memset(&node, 0, sizeof(node));
        node.path     = NODE_GFX;
        node.owner_id = "xserver64";
        node.perms    = USERFS_PERM_READ_WRITE;
        node.ops.write = node_gfx_write;
        if (userfs_register(&node) < 0) {
            printf("[FlareXd] ERROR: Could not register %s\n", NODE_DEV_GFX);
            return -1;
        }
    }
    {
        userfs_user_node_t node;
        memset(&node, 0, sizeof(node));
        node.path     = NODE_EVENT;
        node.owner_id = "xserver64";
        node.perms    = USERFS_PERM_READ_WRITE;
        node.ops.read = node_event_read;
        if (userfs_register(&node) < 0) {
            printf("[FlareXd] ERROR: Could not register %s\n", NODE_DEV_EVENT);
            return -1;
        }
    }
    {
        userfs_user_node_t node;
        memset(&node, 0, sizeof(node));
        node.path     = NODE_WINDOWS;
        node.owner_id = "xserver64";
        node.perms    = USERFS_PERM_READ_WRITE;
        node.ops.write = node_windows_write;
        if (userfs_register(&node) < 0) {
            printf("[FlareXd] ERROR: Could not register %s\n", NODE_DEV_WINDOWS);
            return -1;
        }
    }

    printf("[FlareXd] UserFS nodes registered successfully\n");
    return 0;
}

int md_main(long argc, char** argv) {
    (void)argc; (void)argv;

    printf("=== FlareX Display Server ===\n");
    
    /* Initialize display */
    if (init_display() < 0) {
        return 1;
    }
    
    /* Initialize UserFS communication */
    if (init_userfs_nodes() < 0) {
        NodGL_ReleaseDevice(g_display.device);
        return 1;
    }
    
    /* Open input device for events */
    g_input_fd = open("$/dev/input/event0", O_RDONLY | O_NONBLOCK, 0);
    if (g_input_fd < 0) {
        printf("[FlareXd] WARNING: Could not open input device (events disabled)\n");
    } else {
        printf("[FlareXd] Input device opened successfully\n");
    }
    
    printf("[FlareXd] Server ready and waiting for clients...\n");

    /* Attempt to load the system FNT font for text rendering.
     * Read in chunks until EOF since userland has no stat() to query file size. */
    fnt_font_t *sys_font = NULL;
    {
        const char *font_path = "/ModuOS/shared/usr/assets/fonts/Unicode.fnt";
        int ffd = open(font_path, O_RDONLY, 0);
        if (ffd >= 0) {
            /* Grow a buffer dynamically as we read. */
            size_t cap = 16384;
            size_t total = 0;
            uint8_t *fdata = (uint8_t *)malloc(cap);
            if (fdata) {
                for (;;) {
                    if (total == cap) {
                        /* Double the buffer. */
                        uint8_t *nb = (uint8_t *)malloc(cap * 2);
                        if (!nb) break;
                        memcpy(nb, fdata, total);
                        free(fdata);
                        fdata = nb;
                        cap *= 2;
                    }
                    ssize_t got = read(ffd, fdata + total, cap - total);
                    if (got <= 0) break;
                    total += (size_t)got;
                }
                if (total > 0)
                    sys_font = fnt_load_font(fdata, total);
                free(fdata);
            }
            close(ffd);
        }
    }

    uint8_t msg_buf[4096];

    /*
     * Main event loop.
     * - Process input events (keyboard, mouse)
     * - Process client commands via UserFS
     * - Update compositor and render windows
     */
    while (1) {
        int did_work = 0;

        /* --- Input events (keyboard/mouse) --- */
        if (g_input_fd >= 0) {
            process_input_events();
        }

        /* --- Graphics commands from ring --- */
        {
            size_t n = ring_pop(&g_ring_gfx, msg_buf, sizeof(msg_buf));
            if (n > 0) {
                process_message(msg_buf, n, 0, sys_font);
                did_work = 1;
            }
        }

        /* --- Window management commands from ring --- */
        {
            size_t n = ring_pop(&g_ring_windows, msg_buf, sizeof(msg_buf));
            if (n > 0) {
                process_message(msg_buf, n, 0, sys_font);
                did_work = 1;
            }
        }

        if (!did_work)
            yield();
    }

    /* Unreachable, but maintain cleanup discipline. */
    if (sys_font) fnt_free_font(sys_font);
    NodGL_ReleaseDevice(g_display.device);
    return 0;
}

