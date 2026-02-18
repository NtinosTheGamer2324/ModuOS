/*
 * libX26 - Client library for Xenith26
 * Implementation
 */

#include "libX26.h"
#include "../server/libc.h"
#include "../server/string.h"
#include "../server/nodes.h"

struct X26Display {
    int fd_gfx;      /* $/user/xapi/gfxapi */
    int fd_event;    /* $/user/xapi/event */
    int fd_windows;  /* $/user/xapi/windows */
    int width;
    int height;
};

/* ========== Display Connection ========== */

X26Display *X26OpenDisplay(void) {
    X26Display *dpy = (X26Display *)malloc(sizeof(X26Display));
    if (!dpy) return NULL;
    
    memset(dpy, 0, sizeof(X26Display));
    
    /* Open communication channels */
    dpy->fd_gfx = open(NODE_DEV_GFX, O_RDWR, 0);
    dpy->fd_event = open(NODE_DEV_EVENT, O_RDONLY, 0);
    dpy->fd_windows = open(NODE_DEV_WINDOWS, O_RDWR, 0);
    
    if (dpy->fd_gfx < 0 || dpy->fd_event < 0 || dpy->fd_windows < 0) {
        if (dpy->fd_gfx >= 0) close(dpy->fd_gfx);
        if (dpy->fd_event >= 0) close(dpy->fd_event);
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

void X26CloseDisplay(X26Display *dpy) {
    if (!dpy) return;
    
    if (dpy->fd_gfx >= 0) close(dpy->fd_gfx);
    if (dpy->fd_event >= 0) close(dpy->fd_event);
    if (dpy->fd_windows >= 0) close(dpy->fd_windows);
    
    free(dpy);
}

void X26DisplaySize(X26Display *dpy, int *width, int *height) {
    if (!dpy) return;
    if (width) *width = dpy->width;
    if (height) *height = dpy->height;
}

/* ========== Window Management ========== */

X26Window X26CreateWindow(X26Display *dpy, int x, int y, 
                          int width, int height, xapi_win_type_t type) {
    if (!dpy) return 0;
    
    /* Build create window message */
    uint8_t buf[256];
    xapi_win_create_t *msg = (xapi_win_create_t *)buf;
    
    memset(msg, 0, sizeof(xapi_win_create_t));
    msg->hdr.type = XAPI_WIN_CREATE;
    msg->hdr.size = sizeof(xapi_win_create_t);
    msg->width = (uint16_t)width;
    msg->height = (uint16_t)height;
    msg->win_type = type;
    msg->title_len = 0;
    
    /* Send to server */
    ssize_t n = write(dpy->fd_windows, msg, msg->hdr.size);
    if (n != (ssize_t)msg->hdr.size) {
        return 0;
    }
    
    /* TODO: Read back window ID from server */
    /* For now, return dummy ID */
    static uint32_t next_id = 1;
    return next_id++;
}

X26Window X26CreateSimpleWindow(X26Display *dpy, int x, int y, 
                                int width, int height) {
    return X26CreateWindow(dpy, x, y, width, height, XAPI_WIN_TYPE_NORMAL);
}

int X26MapWindow(X26Display *dpy, X26Window win) {
    if (!dpy) return -1;
    
    xapi_win_geometry_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.hdr.type = XAPI_WIN_MAP;
    msg.hdr.size = sizeof(msg);
    msg.hdr.window_id = win;
    msg.x = 100;
    msg.y = 100;
    msg.w = 0;
    msg.h = 0;
    
    return write(dpy->fd_windows, &msg, sizeof(msg)) == sizeof(msg) ? 0 : -1;
}

int X26UnmapWindow(X26Display *dpy, X26Window win) {
    if (!dpy) return -1;
    
    xapi_msg_hdr_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = XAPI_WIN_UNMAP;
    msg.size = sizeof(msg);
    msg.window_id = win;
    
    return write(dpy->fd_windows, &msg, sizeof(msg)) == sizeof(msg) ? 0 : -1;
}

int X26DestroyWindow(X26Display *dpy, X26Window win) {
    if (!dpy) return -1;
    
    xapi_msg_hdr_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = XAPI_WIN_DESTROY;
    msg.size = sizeof(msg);
    msg.window_id = win;
    
    return write(dpy->fd_windows, &msg, sizeof(msg)) == sizeof(msg) ? 0 : -1;
}

int X26SetWindowTitle(X26Display *dpy, X26Window win, const char *title) {
    if (!dpy || !title) return -1;
    
    /* For now, this is a stub - title setting will be implemented later */
    (void)win;
    (void)title;
    return 0;
}

int X26MoveWindow(X26Display *dpy, X26Window win, int x, int y) {
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

int X26ResizeWindow(X26Display *dpy, X26Window win, int width, int height) {
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

int X26RaiseWindow(X26Display *dpy, X26Window win) {
    if (!dpy) return -1;
    
    xapi_msg_hdr_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = XAPI_WIN_RAISE;
    msg.size = sizeof(msg);
    msg.window_id = win;
    
    return write(dpy->fd_windows, &msg, sizeof(msg)) == sizeof(msg) ? 0 : -1;
}

/* ========== Drawing Functions ========== */

int X26FillRectangle(X26Display *dpy, X26Window win, 
                     int x, int y, int width, int height, X26Color color) {
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

int X26DrawLine(X26Display *dpy, X26Window win, 
                int x1, int y1, int x2, int y2, X26Color color) {
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

int X26DrawText(X26Display *dpy, X26Window win, 
                int x, int y, const char *text, X26Color color) {
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

int X26ClearWindow(X26Display *dpy, X26Window win, X26Color color) {
    /* Clear is just a full-window rectangle */
    return X26FillRectangle(dpy, win, 0, 0, 10000, 10000, color);
}

int X26Flush(X26Display *dpy, X26Window win) {
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

int X26NextEvent(X26Display *dpy, X26Event *event) {
    if (!dpy || !event) return -1;
    
    /* TODO: Read event from $/user/xapi/event */
    /* Note: No explicit yield() needed - kernel has preemptive scheduling */
    return 0;
}

int X26Pending(X26Display *dpy) {
    if (!dpy) return 0;
    /* TODO: Check if events available */
    return 0;
}

void X26EventLoop(X26Display *dpy, X26Window win, 
                  void (*draw_func)(X26Display*, X26Window, void*),
                  void *userdata) {
    if (!dpy || !draw_func) return;
    
    /* Initial draw */
    draw_func(dpy, win, userdata);
    X26Flush(dpy, win);
    
    /* Event loop */
    X26Event event;
    while (1) {
        if (X26NextEvent(dpy, &event) > 0) {
            if (event.type == XAPI_EVENT_EXPOSE) {
                draw_func(dpy, win, userdata);
                X26Flush(dpy, win);
            }
        }
    }
}
