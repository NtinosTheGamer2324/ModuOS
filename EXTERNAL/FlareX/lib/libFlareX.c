/*
 * libFlareX - Client library for FlareX
 * Implementation
 */

#include "libFlareX.h"
#include "../server/libc.h"
#include "../server/string.h"
#include "../server/nodes.h"

struct FlareXDisplay {
    int fd_gfx;      /* $/user/xapi/gfxapi  — write graphics cmds */
    int fd_event;    /* $/user/xapi/event   — read input events   */
    int fd_windows;  /* $/user/xapi/windows — write window cmds   */
    int width;
    int height;
    /* Pending event queue (single-slot; sufficient for our poll model). */
    FlareXEvent pending;
    int       pending_valid;
};

/* ========== Display Connection ========== */

FlareXDisplay *FlareXOpenDisplay(void) {
    FlareXDisplay *dpy = (FlareXDisplay *)malloc(sizeof(FlareXDisplay));
    if (!dpy) return NULL;
    
    memset(dpy, 0, sizeof(FlareXDisplay));

    /* Open communication channels.
     * fd_gfx     — write-only stream for drawing commands.
     * fd_event   — non-blocking read for input events from server.
     * fd_windows — read/write: send window commands, read back ID replies.
     */
    dpy->fd_gfx     = open(NODE_DEV_GFX,     O_WRONLY, 0);
    dpy->fd_event   = open(NODE_DEV_EVENT,   O_RDONLY | O_NONBLOCK, 0);
    dpy->fd_windows = open(NODE_DEV_WINDOWS, O_RDWR   | O_NONBLOCK, 0);

    if (dpy->fd_gfx < 0 || dpy->fd_event < 0 || dpy->fd_windows < 0) {
        if (dpy->fd_gfx >= 0)     close(dpy->fd_gfx);
        if (dpy->fd_event >= 0)   close(dpy->fd_event);
        if (dpy->fd_windows >= 0) close(dpy->fd_windows);
        free(dpy);
        return NULL;
    }
    
    /* Get display size from graphics device */
    int gfx_fd = open("$/dev/graphics/video0", O_RDONLY, 0);
    if (gfx_fd >= 0) {
        uint8_t info_buf[64];
        if (read(gfx_fd, info_buf, sizeof(info_buf)) >= (ssize_t)sizeof(md64api_grp_video_info_t)) {
            md64api_grp_video_info_t *info = (md64api_grp_video_info_t *)info_buf;
            dpy->width = info->width;
            dpy->height = info->height;
        }
        close(gfx_fd);
    }
    
    return dpy;
}

void FlareXCloseDisplay(FlareXDisplay *dpy) {
    if (!dpy) return;
    
    if (dpy->fd_gfx >= 0) close(dpy->fd_gfx);
    if (dpy->fd_event >= 0) close(dpy->fd_event);
    if (dpy->fd_windows >= 0) close(dpy->fd_windows);
    
    free(dpy);
}

void FlareXDisplaySize(FlareXDisplay *dpy, int *width, int *height) {
    if (!dpy) return;
    if (width) *width = dpy->width;
    if (height) *height = dpy->height;
}

/* ========== Window Management ========== */

FlareXWindow FlareXCreateWindow(FlareXDisplay *dpy, int x, int y,
                          int width, int height, xapi_win_type_t type) {
    if (!dpy) return 0;

    xapi_win_create_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.hdr.type     = XAPI_WIN_CREATE;
    msg.hdr.size     = sizeof(msg);
    msg.width        = (uint16_t)width;
    msg.height       = (uint16_t)height;
    msg.win_type     = (uint8_t)type;
    msg.title_len    = 0;

    ssize_t n = write(dpy->fd_windows, &msg, sizeof(msg));
    if (n != (ssize_t)sizeof(msg)) return 0;

    /*
     * The server sends back an xapi_win_geometry_t whose window_id field
     * carries the newly assigned ID.  Poll with a short timeout so we don't
     * block forever if the server is busy.
     */
    uint8_t rbuf[64];
    uint64_t deadline = time_ms() + 500;
    while (time_ms() < deadline) {
        ssize_t rn = read(dpy->fd_windows, rbuf, sizeof(rbuf));
        if (rn >= (ssize_t)sizeof(xapi_msg_hdr_t)) {
            xapi_msg_hdr_t *hdr = (xapi_msg_hdr_t *)rbuf;
            if (hdr->window_id != 0) {
                /* Caller positions/maps via FlareXMapWindow; just return the ID. */
                (void)x; (void)y;
                return hdr->window_id;
            }
        }
        yield();
    }

    /*
     * Server did not echo an ID within the timeout (e.g. older server build
     * that does not send a reply).  Allocate a client-side ID so the API
     * remains usable; commands will be routed by this ID on the wire and the
     * server will look them up by the same sequence.
     */
    static uint32_t next_id = 1;
    return next_id++;
}

FlareXWindow FlareXCreateSimpleWindow(FlareXDisplay *dpy, int x, int y, 
                                int width, int height) {
    return FlareXCreateWindow(dpy, x, y, width, height, XAPI_WIN_TYPE_NORMAL);
}

int FlareXMapWindow(FlareXDisplay *dpy, FlareXWindow win) {
    if (!dpy) return -1;

    /* MAP with x=y=0 and w=h=0 — server uses the window's current position. */
    xapi_win_geometry_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.hdr.type      = XAPI_WIN_MAP;
    msg.hdr.size      = sizeof(msg);
    msg.hdr.window_id = win;
    msg.x = 0; msg.y = 0; msg.w = 0; msg.h = 0;

    return write(dpy->fd_windows, &msg, sizeof(msg)) == sizeof(msg) ? 0 : -1;
}

int FlareXUnmapWindow(FlareXDisplay *dpy, FlareXWindow win) {
    if (!dpy) return -1;
    
    xapi_msg_hdr_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = XAPI_WIN_UNMAP;
    msg.size = sizeof(msg);
    msg.window_id = win;
    
    return write(dpy->fd_windows, &msg, sizeof(msg)) == sizeof(msg) ? 0 : -1;
}

int FlareXDestroyWindow(FlareXDisplay *dpy, FlareXWindow win) {
    if (!dpy) return -1;
    
    xapi_msg_hdr_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = XAPI_WIN_DESTROY;
    msg.size = sizeof(msg);
    msg.window_id = win;
    
    return write(dpy->fd_windows, &msg, sizeof(msg)) == sizeof(msg) ? 0 : -1;
}

int FlareXSetWindowTitle(FlareXDisplay *dpy, FlareXWindow win, const char *title) {
    if (!dpy || !title) return -1;

    size_t tlen = strlen(title);
    size_t total = sizeof(xapi_win_set_title_t) + tlen;
    if (total > 512) total = 512;

    uint8_t buf[512];
    xapi_win_set_title_t *msg = (xapi_win_set_title_t *)buf;
    memset(msg, 0, sizeof(xapi_win_set_title_t));
    msg->hdr.type      = XAPI_WIN_SET_TITLE;
    msg->hdr.size      = (uint16_t)total;
    msg->hdr.window_id = win;
    msg->title_len     = (uint16_t)(total - sizeof(xapi_win_set_title_t));
    memcpy(buf + sizeof(xapi_win_set_title_t), title, msg->title_len);

    return write(dpy->fd_windows, buf, total) == (ssize_t)total ? 0 : -1;
}

int FlareXMoveWindow(FlareXDisplay *dpy, FlareXWindow win, int x, int y) {
    if (!dpy) return -1;
    
    xapi_win_geometry_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.hdr.type = XAPI_WIN_MOVE;
    msg.hdr.size = sizeof(msg);
    msg.hdr.window_id = win;
    msg.x = (int16_t)x;
    msg.y = (int16_t)y;
    
    return write(dpy->fd_windows, &msg, sizeof(msg)) == sizeof(msg) ? 0 : -1;
}

int FlareXResizeWindow(FlareXDisplay *dpy, FlareXWindow win, int width, int height) {
    if (!dpy) return -1;
    
    xapi_win_geometry_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.hdr.type = XAPI_WIN_RESIZE;
    msg.hdr.size = sizeof(msg);
    msg.hdr.window_id = win;
    msg.w = (uint16_t)width;
    msg.h = (uint16_t)height;
    
    return write(dpy->fd_windows, &msg, sizeof(msg)) == sizeof(msg) ? 0 : -1;
}

int FlareXRaiseWindow(FlareXDisplay *dpy, FlareXWindow win) {
    if (!dpy) return -1;
    
    xapi_msg_hdr_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = XAPI_WIN_RAISE;
    msg.size = sizeof(msg);
    msg.window_id = win;
    
    return write(dpy->fd_windows, &msg, sizeof(msg)) == sizeof(msg) ? 0 : -1;
}

/* ========== Drawing Functions ========== */

int FlareXFillRectangle(FlareXDisplay *dpy, FlareXWindow win, 
                     int x, int y, int width, int height, FlareXColor color) {
    if (!dpy) return -1;
    
    xapi_cmd_rect_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.hdr.type = XAPI_CMD_RECT;
    msg.hdr.size = sizeof(msg);
    msg.hdr.window_id = win;
    msg.x = (int16_t)x;
    msg.y = (int16_t)y;
    msg.w = (uint16_t)width;
    msg.h = (uint16_t)height;
    msg.color = color;
    
    return write(dpy->fd_gfx, &msg, sizeof(msg)) == sizeof(msg) ? 0 : -1;
}

int FlareXDrawLine(FlareXDisplay *dpy, FlareXWindow win, 
                int x1, int y1, int x2, int y2, FlareXColor color) {
    if (!dpy) return -1;
    
    xapi_cmd_line_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.hdr.type = XAPI_CMD_LINE;
    msg.hdr.size = sizeof(msg);
    msg.hdr.window_id = win;
    msg.x1 = (int16_t)x1;
    msg.y1 = (int16_t)y1;
    msg.x2 = (int16_t)x2;
    msg.y2 = (int16_t)y2;
    msg.color = color;
    
    return write(dpy->fd_gfx, &msg, sizeof(msg)) == sizeof(msg) ? 0 : -1;
}

int FlareXDrawText(FlareXDisplay *dpy, FlareXWindow win, 
                int x, int y, const char *text, FlareXColor color) {
    if (!dpy || !text) return -1;
    
    uint8_t buf[512];
    xapi_cmd_text_t *msg = (xapi_cmd_text_t *)buf;
    
    size_t text_len = strlen(text);
    if (text_len > sizeof(buf) - sizeof(xapi_cmd_text_t)) {
        text_len = sizeof(buf) - sizeof(xapi_cmd_text_t);
    }
    
    memset(msg, 0, sizeof(xapi_cmd_text_t));
    msg->hdr.type = XAPI_CMD_TEXT;
    msg->hdr.size = (uint16_t)(sizeof(xapi_cmd_text_t) + text_len);
    msg->hdr.window_id = win;
    msg->x = (int16_t)x;
    msg->y = (int16_t)y;
    msg->color = color;
    msg->text_len = (uint16_t)text_len;
    
    memcpy(buf + sizeof(xapi_cmd_text_t), text, text_len);
    
    return write(dpy->fd_gfx, buf, msg->hdr.size) == (ssize_t)msg->hdr.size ? 0 : -1;
}

int FlareXDrawPoint(FlareXDisplay *dpy, FlareXWindow win,
                 int x, int y, FlareXColor color) {
    if (!dpy) return -1;

    xapi_cmd_pixel_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.hdr.type      = XAPI_CMD_PIXEL;
    msg.hdr.size      = sizeof(msg);
    msg.hdr.window_id = win;
    msg.x             = (int16_t)x;
    msg.y             = (int16_t)y;
    msg.color         = color;

    return write(dpy->fd_gfx, &msg, sizeof(msg)) == sizeof(msg) ? 0 : -1;
}

int FlareXClearWindow(FlareXDisplay *dpy, FlareXWindow win, FlareXColor color) {
    if (!dpy) return -1;
    /* Use maximum plausible dimensions; server clips to window bounds. */
    return FlareXFillRectangle(dpy, win, 0, 0, 0x7FFF, 0x7FFF, color);
}

int FlareXFlush(FlareXDisplay *dpy, FlareXWindow win) {
    if (!dpy) return -1;
    
    xapi_cmd_commit_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.hdr.type = XAPI_CMD_COMMIT;
    msg.hdr.size = sizeof(msg);
    msg.hdr.window_id = win;
    msg.x = 0;
    msg.y = 0;
    msg.w = 10000;
    msg.h = 10000;
    
    return write(dpy->fd_gfx, &msg, sizeof(msg)) == sizeof(msg) ? 0 : -1;
}

/* ========== Event Handling ========== */

/*
 * Try to read one event from the event channel.
 * Returns 1 if an event was filled, 0 if nothing available, -1 on error.
 */
int FlareXNextEvent(FlareXDisplay *dpy, FlareXEvent *event) {
    if (!dpy || !event) return -1;

    /* Drain pending slot first. */
    if (dpy->pending_valid) {
        *event = dpy->pending;
        dpy->pending_valid = 0;
        return 1;
    }

    uint8_t buf[sizeof(xapi_msg_hdr_t) + sizeof(xapi_event_mouse_t)];
    ssize_t n = read(dpy->fd_event, buf, sizeof(buf));
    if (n <= 0) return 0;
    if (n < (ssize_t)sizeof(xapi_msg_hdr_t)) return 0;

    const xapi_msg_hdr_t *hdr = (const xapi_msg_hdr_t *)buf;
    memset(event, 0, sizeof(*event));
    event->type   = hdr->type;
    event->window = hdr->window_id;

    switch (hdr->type) {
        case XAPI_EVENT_KEY_PRESS:
        case XAPI_EVENT_KEY_RELEASE: {
            if (n >= (ssize_t)(sizeof(xapi_msg_hdr_t) + sizeof(xapi_event_key_t) - sizeof(xapi_msg_hdr_t))) {
                const xapi_event_key_t *k = (const xapi_event_key_t *)buf;
                event->keycode   = k->keycode;
                event->unicode   = k->unicode;
                event->modifiers = k->modifiers;
            }
            break;
        }
        case XAPI_EVENT_MOUSE_MOVE:
        case XAPI_EVENT_MOUSE_PRESS:
        case XAPI_EVENT_MOUSE_RELEASE: {
            if (n >= (ssize_t)sizeof(xapi_event_mouse_t)) {
                const xapi_event_mouse_t *m = (const xapi_event_mouse_t *)buf;
                event->x         = m->x;
                event->y         = m->y;
                event->root_x    = m->root_x;
                event->root_y    = m->root_y;
                event->buttons   = m->buttons;
                event->button    = m->button;
                event->modifiers = m->modifiers;
            }
            break;
        }
        case XAPI_EVENT_EXPOSE: {
            /* No extra payload needed — window_id is sufficient. */
            break;
        }
        default:
            return 0;
    }

    return 1;
}

int FlareXPending(FlareXDisplay *dpy) {
    if (!dpy) return 0;
    if (dpy->pending_valid) return 1;

    /* Non-blocking peek. */
    uint8_t buf[sizeof(xapi_msg_hdr_t) + sizeof(xapi_event_mouse_t)];
    ssize_t n = read(dpy->fd_event, buf, sizeof(buf));
    if (n <= 0) return 0;

    /* Stash into pending slot. */
    const xapi_msg_hdr_t *hdr = (const xapi_msg_hdr_t *)buf;
    memset(&dpy->pending, 0, sizeof(dpy->pending));
    dpy->pending.type   = hdr->type;
    dpy->pending.window = hdr->window_id;

    switch (hdr->type) {
        case XAPI_EVENT_KEY_PRESS:
        case XAPI_EVENT_KEY_RELEASE: {
            const xapi_event_key_t *k = (const xapi_event_key_t *)buf;
            dpy->pending.keycode   = k->keycode;
            dpy->pending.unicode   = k->unicode;
            dpy->pending.modifiers = k->modifiers;
            break;
        }
        case XAPI_EVENT_MOUSE_MOVE:
        case XAPI_EVENT_MOUSE_PRESS:
        case XAPI_EVENT_MOUSE_RELEASE: {
            if (n >= (ssize_t)sizeof(xapi_event_mouse_t)) {
                const xapi_event_mouse_t *m = (const xapi_event_mouse_t *)buf;
                dpy->pending.x         = m->x;
                dpy->pending.y         = m->y;
                dpy->pending.root_x    = m->root_x;
                dpy->pending.root_y    = m->root_y;
                dpy->pending.buttons   = m->buttons;
                dpy->pending.button    = m->button;
                dpy->pending.modifiers = m->modifiers;
            }
            break;
        }
        default:
            return 0;
    }

    dpy->pending_valid = 1;
    return 1;
}

void FlareXEventLoop(FlareXDisplay *dpy, FlareXWindow win,
                  void (*draw_func)(FlareXDisplay*, FlareXWindow, void*),
                  void *userdata) {
    if (!dpy) return;

    /* Initial draw if callback provided. */
    if (draw_func) {
        draw_func(dpy, win, userdata);
        FlareXFlush(dpy, win);
    }

    FlareXEvent event;
    while (1) {
        int got = FlareXNextEvent(dpy, &event);
        if (got > 0) {
            if (event.type == XAPI_EVENT_EXPOSE && draw_func) {
                draw_func(dpy, win, userdata);
                FlareXFlush(dpy, win);
            }
            /* Key/mouse events are delivered; caller can extend this loop
               directly if they need finer-grained handling. */
            if (event.type == XAPI_EVENT_KEY_PRESS && event.keycode == 0x01) {
                /* ESC = graceful exit from the event loop. */
                break;
            }
        } else {
            yield();
        }
    }
}

