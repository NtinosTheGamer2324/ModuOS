// nodds_client.c — NodDS client library implementation
// All control calls are synchronous (blocking invoke).
// Event reads are non-blocking (O_RDONLY | O_NONBLOCK).
#define LIBC_NO_START
#include "nodds_client.h"
#include "libc.h"

// ============================================================
// Internal client state
// ============================================================

struct nodds_client {
    int ctl_fd;   // fd for $/user/nodds/control (invoke)
};

// ============================================================
// Internal helper: send a control request, get a response.
//
// IMPORTANT: req and resp are heap-allocated here, NOT on the
// stack.  The kernel invoke handler stores the out-buffer pointer
// and writes the response into it (possibly after a reschedule).
// If the buffer lives on the user stack the kernel may end up
// holding a pointer into a stack frame that has since been
// reused or, worse, into the kernel-stack mirror of that frame
// (the page-fault we saw: CR2 just below RSP0, error code 0x5).
// Heap allocations sit in a stable, always-mapped region and
// avoid that class of bug entirely.
//
// Returns 0 on success, -1 on error.
// ============================================================

static int ctl_call(nodds_client_t *client,
                    const nodds_control_req_t *req,
                    nodds_control_resp_t      *resp)
{
    // Allocate stable heap buffers for the IPC round-trip.
    nodds_control_req_t  *hreq  = (nodds_control_req_t  *)malloc(sizeof(*hreq));
    nodds_control_resp_t *hresp = (nodds_control_resp_t *)calloc(1, sizeof(*hresp));

    if (!hreq || !hresp) {
        free(hreq);
        free(hresp);
        return -1;
    }

    // Copy caller's request into the heap buffer.
    memcpy(hreq, req, sizeof(*hreq));

    ssize_t r = invoke(client->ctl_fd,
                       hreq,  sizeof(*hreq),
                       hresp, sizeof(*hresp));

    if (r >= (ssize_t)sizeof(*hresp)) {
        // Copy result back to caller's (possibly stack) resp struct.
        memcpy(resp, hresp, sizeof(*hresp));
    }

    free(hreq);
    free(hresp);

    if (r < (ssize_t)sizeof(*hresp)) return -1;
    if (resp->status != 0)           return -1;
    return 0;
}

// helper
static void sleep_ms(uint32_t ms) {
    uint64_t end = time_ms() + ms;
    while (time_ms() < end) yield();
}

// ============================================================
// nodds_connect
// ============================================================

nodds_client_t *nodds_connect(int timeout_ms)
{
    int fd = -1;
    int elapsed = 0;
    const int step_ms = 10;

    while (1) {
        fd = open("$/user/nodds/control", O_RDWR, 0);
        if (fd >= 0) break;

        if (elapsed >= timeout_ms) {
            printf("[nodds_client] Cannot open control node\n");
            return NULL;
        }
        sleep_ms(step_ms);
        elapsed += step_ms;
    }

    nodds_client_t *c = (nodds_client_t *)malloc(sizeof(nodds_client_t));
    if (!c) { close(fd); return NULL; }
    c->ctl_fd = fd;
    return c;
}

// ============================================================
// nodds_disconnect
// ============================================================

void nodds_disconnect(nodds_client_t *client)
{
    if (!client) return;
    if (client->ctl_fd >= 0) close(client->ctl_fd);
    free(client);
}

// ============================================================
// nodds_query_screen
// ============================================================

int nodds_query_screen(nodds_client_t *client,
                       uint32_t *out_w, uint32_t *out_h)
{
    nodds_control_req_t  req  = {0};
    nodds_control_resp_t resp = {0};
    req.cmd = NODDS_CMD_QUERY_SCREEN;

    if (ctl_call(client, &req, &resp) != 0) return -1;
    if (out_w) *out_w = resp.screen_width;
    if (out_h) *out_h = resp.screen_height;
    return 0;
}

// ============================================================
// nodds_create_window
// ============================================================

int nodds_create_window(nodds_client_t *client,
                        int x, int y,
                        uint32_t width, uint32_t height,
                        const char *title,
                        nodds_window_t *out_win)
{
    if (!client || !out_win) return -1;

    nodds_control_req_t  req  = {0};
    nodds_control_resp_t resp = {0};

    req.cmd    = NODDS_CMD_CREATE_WINDOW;
    req.x      = x;
    req.y      = y;
    req.width  = width;
    req.height = height;
    if (title)
        strncpy(req.title, title, NODDS_MAX_TITLE - 1);

    if (ctl_call(client, &req, &resp) != 0) {
        printf("[nodds_client] create_window failed: %s\n", resp.msg);
        return -1;
    }

    uint32_t wid = resp.wid;

    // Open event node (non-blocking reads)
    char event_path[80];
    snprintf(event_path, sizeof(event_path), "$/user/nodds/events/%u", wid);
    int efd = open(event_path, O_RDONLY | O_NONBLOCK, 0);
    if (efd < 0) {
        printf("[nodds_client] Cannot open event node for wid %u\n", wid);
        nodds_control_req_t  dreq  = {0};
        nodds_control_resp_t dresp = {0};
        dreq.cmd = NODDS_CMD_DESTROY_WINDOW;
        dreq.wid = wid;
        ctl_call(client, &dreq, &dresp);
        return -1;
    }

    // Open frame node (write-only)
    char frame_path[80];
    snprintf(frame_path, sizeof(frame_path), "$/user/nodds/%u", wid);
    int ffd = open(frame_path, O_WRONLY, 0);
    if (ffd < 0) {
        printf("[nodds_client] Cannot open frame node for wid %u\n", wid);
        close(efd);
        nodds_control_req_t  dreq  = {0};
        nodds_control_resp_t dresp = {0};
        dreq.cmd = NODDS_CMD_DESTROY_WINDOW;
        dreq.wid = wid;
        ctl_call(client, &dreq, &dresp);
        return -1;
    }

    out_win->wid      = wid;
    out_win->width    = width;
    out_win->height   = height;
    out_win->event_fd = efd;
    out_win->frame_fd = ffd;
    return 0;
}

// ============================================================
// nodds_destroy_window
// ============================================================

int nodds_destroy_window(nodds_client_t *client, nodds_window_t *win)
{
    if (!client || !win) return -1;

    nodds_control_req_t  req  = {0};
    nodds_control_resp_t resp = {0};
    req.cmd = NODDS_CMD_DESTROY_WINDOW;
    req.wid = win->wid;

    int r = ctl_call(client, &req, &resp);

    if (win->event_fd >= 0) { close(win->event_fd); win->event_fd = -1; }
    if (win->frame_fd >= 0) { close(win->frame_fd); win->frame_fd = -1; }
    win->wid = 0;
    return r;
}

// ============================================================
// Simple window management calls
// ============================================================

int nodds_move_window(nodds_client_t *client, nodds_window_t *win,
                      int x, int y)
{
    nodds_control_req_t  req  = {0};
    nodds_control_resp_t resp = {0};
    req.cmd = NODDS_CMD_MOVE_WINDOW;
    req.wid = win->wid;
    req.x   = x;
    req.y   = y;
    return ctl_call(client, &req, &resp);
}

int nodds_resize_window(nodds_client_t *client, nodds_window_t *win,
                        uint32_t width, uint32_t height)
{
    nodds_control_req_t  req  = {0};
    nodds_control_resp_t resp = {0};
    req.cmd    = NODDS_CMD_RESIZE_WINDOW;
    req.wid    = win->wid;
    req.width  = width;
    req.height = height;
    int r = ctl_call(client, &req, &resp);
    if (r == 0) {
        win->width  = width;
        win->height = height;
    }
    return r;
}

int nodds_set_title(nodds_client_t *client, nodds_window_t *win,
                    const char *title)
{
    nodds_control_req_t  req  = {0};
    nodds_control_resp_t resp = {0};
    req.cmd = NODDS_CMD_SET_TITLE;
    req.wid = win->wid;
    if (title) strncpy(req.title, title, NODDS_MAX_TITLE - 1);
    return ctl_call(client, &req, &resp);
}

int nodds_raise_window(nodds_client_t *client, nodds_window_t *win)
{
    nodds_control_req_t  req  = {0};
    nodds_control_resp_t resp = {0};
    req.cmd = NODDS_CMD_RAISE_WINDOW;
    req.wid = win->wid;
    return ctl_call(client, &req, &resp);
}

int nodds_show_window(nodds_client_t *client, nodds_window_t *win)
{
    nodds_control_req_t  req  = {0};
    nodds_control_resp_t resp = {0};
    req.cmd = NODDS_CMD_SHOW_WINDOW;
    req.wid = win->wid;
    return ctl_call(client, &req, &resp);
}

int nodds_hide_window(nodds_client_t *client, nodds_window_t *win)
{
    nodds_control_req_t  req  = {0};
    nodds_control_resp_t resp = {0};
    req.cmd = NODDS_CMD_HIDE_WINDOW;
    req.wid = win->wid;
    return ctl_call(client, &req, &resp);
}

// ============================================================
// nodds_blit_frame
// ============================================================

int nodds_blit_frame(nodds_window_t *win,
                     const uint32_t *pixels,
                     uint32_t width, uint32_t height)
{
    if (!win || win->frame_fd < 0 || !pixels) return -1;
    if (width != win->width || height != win->height)  return -1;

    uint32_t data_size = width * height * sizeof(uint32_t);
    size_t   total     = sizeof(nodds_frame_header_t) + data_size;

    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) return -1;

    nodds_frame_header_t *hdr = (nodds_frame_header_t *)buf;
    hdr->magic     = NODDS_FRAME_MAGIC;
    hdr->wid       = win->wid;
    hdr->width     = width;
    hdr->height    = height;
    hdr->format    = 1; /* NodGL_FORMAT_R8G8B8A8_UNORM */
    hdr->data_size = data_size;

    memcpy(buf + sizeof(nodds_frame_header_t), pixels, data_size);

    ssize_t r = write(win->frame_fd, buf, total);
    free(buf);
    return (r == (ssize_t)total) ? 0 : -1;
}

// ============================================================
// nodds_poll_event
// ============================================================

int nodds_poll_event(nodds_window_t *win, nodds_event_t *out_evt)
{
    if (!win || win->event_fd < 0 || !out_evt) return -1;

    ssize_t r = read(win->event_fd, out_evt, sizeof(nodds_event_t));
    if (r == (ssize_t)sizeof(nodds_event_t)) return 1;
    if (r == 0)  return 0;  // empty queue
    if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
    return -1;
}