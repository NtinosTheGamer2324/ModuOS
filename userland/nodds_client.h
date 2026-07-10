#pragma once
// nodds_client.h — NodDS client library
// Wraps UserFS IPC for apps that want to create and manage windows.
//
// Usage:
//   1. nodds_connect()          — open control node (blocking)
//   2. nodds_create_window()    — create a window, get wid
//   3. nodds_blit_frame()       — push a pixel buffer
//   4. nodds_poll_event()       — non-blocking event read
//   5. nodds_destroy_window()   — destroy window
//   6. nodds_disconnect()       — close control fd

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================
// Re-export protocol constants (clients shouldn't need nodds.c)
// ============================================================

#define NODDS_MAX_TITLE     64

// Commands (sent to $/user/nodds/control)
#define NODDS_CMD_CREATE_WINDOW   1
#define NODDS_CMD_DESTROY_WINDOW  2
#define NODDS_CMD_MOVE_WINDOW     3
#define NODDS_CMD_RESIZE_WINDOW   4
#define NODDS_CMD_RAISE_WINDOW    5
#define NODDS_CMD_LOWER_WINDOW    6
#define NODDS_CMD_SET_TITLE       7
#define NODDS_CMD_QUERY_SCREEN    8
#define NODDS_CMD_SHOW_WINDOW     9
#define NODDS_CMD_HIDE_WINDOW     10
#define NODDS_CMD_FOCUS_WINDOW    11

// Event types (read from $/user/nodds/events/<wid>)
#define NODDS_EVT_EXPOSE          1
#define NODDS_EVT_KEY_PRESS       2
#define NODDS_EVT_KEY_RELEASE     3
#define NODDS_EVT_MOUSE_MOVE      4
#define NODDS_EVT_MOUSE_BUTTON    5
#define NODDS_EVT_RESIZE          6
#define NODDS_EVT_CLOSE           7
#define NODDS_EVT_FOCUS_IN        8
#define NODDS_EVT_FOCUS_OUT       9

// Frame blit magic
#define NODDS_FRAME_MAGIC 0x4E445346u

// ============================================================
// Wire structs (must match nodds.c exactly)
// ============================================================

typedef struct __attribute__((packed)) {
    int      cmd;
    uint32_t wid;
    int      x, y;
    uint32_t width, height;
    uint32_t flags;
    char     title[NODDS_MAX_TITLE];
} nodds_control_req_t;

typedef struct __attribute__((packed)) {
    int      status;
    uint32_t wid;
    uint32_t screen_width;
    uint32_t screen_height;
    char     msg[64];
} nodds_control_resp_t;

typedef struct __attribute__((packed)) {
    uint32_t wid;
    uint32_t type;
    uint32_t timestamp;
    union {
        struct { uint32_t keycode; uint32_t modifiers; } key;
        struct { int32_t x; int32_t y; uint32_t buttons; } mouse;
        struct { uint32_t width; uint32_t height; }       resize;
    } data;
} nodds_event_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t wid;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t data_size;
} nodds_frame_header_t;

// ============================================================
// Client window handle
// ============================================================

typedef struct {
    uint32_t wid;
    uint32_t width;
    uint32_t height;
    int      event_fd;   // fd for $/user/nodds/events/<wid>, O_NONBLOCK
    int      frame_fd;   // fd for $/user/nodds/<wid> (write)
} nodds_window_t;

// ============================================================
// Library state (opaque to callers)
// ============================================================

typedef struct nodds_client nodds_client_t;

// ============================================================
// API
// ============================================================

/*
 * nodds_connect — open the control node.
 * Returns an opaque client handle on success, NULL on failure.
 * Blocking: will retry up to `timeout_ms` milliseconds if nodds
 * hasn't registered the node yet (pass 0 for no retry).
 */
nodds_client_t *nodds_connect(int timeout_ms);

/*
 * nodds_disconnect — close the control fd and free the handle.
 * Does NOT destroy any windows; call nodds_destroy_window() first.
 */
void nodds_disconnect(nodds_client_t *client);

/*
 * nodds_query_screen — fill out_w/out_h with the screen resolution.
 * Returns 0 on success, -1 on error.
 */
int nodds_query_screen(nodds_client_t *client,
                       uint32_t *out_w, uint32_t *out_h);

/*
 * nodds_create_window — create a window and open its event/frame nodes.
 * Returns 0 on success and fills *out_win.
 * Returns -1 on failure.
 */
int nodds_create_window(nodds_client_t *client,
                        int x, int y,
                        uint32_t width, uint32_t height,
                        const char *title,
                        nodds_window_t *out_win);

/*
 * nodds_destroy_window — send DESTROY_WINDOW, close fds, zero the struct.
 */
int nodds_destroy_window(nodds_client_t *client, nodds_window_t *win);

/*
 * nodds_move_window / nodds_resize_window / nodds_set_title
 * Standard window management — all blocking (synchronous invoke).
 */
int nodds_move_window(nodds_client_t *client, nodds_window_t *win,
                      int x, int y);
int nodds_resize_window(nodds_client_t *client, nodds_window_t *win,
                        uint32_t width, uint32_t height);
int nodds_set_title(nodds_client_t *client, nodds_window_t *win,
                    const char *title);
int nodds_raise_window(nodds_client_t *client, nodds_window_t *win);
int nodds_show_window(nodds_client_t *client, nodds_window_t *win);
int nodds_hide_window(nodds_client_t *client, nodds_window_t *win);

/*
 * nodds_blit_frame — push a pixel buffer to the compositor.
 * `pixels` must be width*height uint32_t values in NodGL_FORMAT_R8G8B8A8_UNORM.
 * Returns 0 on success, -1 on error.
 */
int nodds_blit_frame(nodds_window_t *win,
                     const uint32_t *pixels,
                     uint32_t width, uint32_t height);

/*
 * nodds_poll_event — non-blocking read of one event from the window's event node.
 * Returns 1 if an event was read into *out_evt.
 * Returns 0 if the queue is empty (EAGAIN).
 * Returns -1 on error.
 */
int nodds_poll_event(nodds_window_t *win, nodds_event_t *out_evt);

#ifdef __cplusplus
}
#endif