#pragma once
/*
 * wm_client_resize.h — shared helper so every mdw client handles
 * MAXIMIZE/RESTORE/FULLSCREEN/RESIZE the same way, without each one
 * reinventing slightly-different polling code.
 *
 * WHY THIS IS NEEDED: mdw draws every window's titlebar/min/max/close
 * chrome itself — a client's own wm_ipc_call() never fires for a click on
 * its OWN titlebar button, mdw just handles it internally. That means the
 * normal "call a command, get a response back" pattern doesn't apply here:
 * the only way a client finds out its content SHM segment got reallocated
 * (by its own titlebar, or by an external taskbar driving the Window API
 * on its win_id) is to poll WM_CMD_LIST_WINDOWS and compare shm_name
 * against what it currently has mapped — see the remap-detection note on
 * wm_win_info_t in wm_protocol.h. This header is exactly that poll+remap,
 * plus the minimized flag, written once.
 *
 * Usage:
 *   wm_client_surface_t surf;
 *   wm_client_surface_init(&surf, g_buf, resp.shm_name, resp.shm_size,
 *                           resp_w, resp_h);
 *   ...
 *   while (!quit) {
 *       // drain key/mouse rings as usual, then:
 *       int rc = wm_client_surface_poll(mb, win_id, &surf);
 *       if (rc < 0) break;               // window torn down elsewhere
 *       if (surf.minimized) {            // nothing visible -- do no work
 *           next_tick pacing only; continue;
 *       }
 *       if (rc > 0) {                    // surf.buf/w/h just changed
 *           g_buf = surf.buf;            // re-point your own globals
 *           // either re-layout fully (fixed-size UIs: letterbox — clear
 *           // the whole new buffer, then redraw your normal fixed-size
 *           // content anchored top-left) or recompute a responsive
 *           // layout against surf.w/surf.h, then force a full redraw.
 *       }
 *       ... normal render-if-dirty + wm_damage ...
 *   }
 *   // teardown: munmap(surf.buf, surf.shm_size) -- NOT your original
 *   // resp.shm_size, which may be stale after any resize.
 */

#include "wm_protocol.h"

typedef struct {
    uint32_t *buf;
    uint64_t  shm_size;
    char      shm_name[64];
    int       w, h;
    int       state;      /* WM_STATE_* as of the last poll */
    int       minimized;  /* convenience: state == WM_STATE_MINIMIZED */
} wm_client_surface_t;

static inline void wm_client_surface_init(wm_client_surface_t *s, uint32_t *buf,
                                           const char *shm_name, uint64_t shm_size,
                                           int w, int h) {
    s->buf = buf;
    s->shm_size = shm_size;
    int i = 0;
    while (shm_name[i] && i + 1 < (int)sizeof(s->shm_name)) { s->shm_name[i] = shm_name[i]; i++; }
    s->shm_name[i] = 0;
    s->w = w; s->h = h;
    s->state = WM_STATE_NORMAL;
    s->minimized = 0;
}

static inline int wm__client_streq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

/* Returns 1 if the backing buffer was just remapped (caller must re-point
 * its own buffer pointer, clear/relayout, and force a full redraw), 0 if
 * nothing changed, -1 if the window no longer exists at all (closed out
 * from under the client some other way — caller should exit its loop).
 * One WM_CMD_LIST_WINDOWS round trip; call it at whatever cadence you
 * already poll input rings at. */
static inline int wm_client_surface_poll(wm_shm_t *mb, int win_id, wm_client_surface_t *s) {
    wm_win_info_t wins[WM_MAX_WINDOWS];
    int n = wm_list_windows(mb, wins);
    if (n < 0) return 0; /* one bad mailbox round trip shouldn't tear the client down */

    for (int i = 0; i < n; i++) {
        if (wins[i].win_id != win_id) continue;

        s->state = wins[i].state;
        s->minimized = (wins[i].state == WM_STATE_MINIMIZED);

        if (wm__client_streq(wins[i].shm_name, s->shm_name)) return 0;

        /* Content buffer changed under us (MAXIMIZE/RESTORE/FULLSCREEN/
         * RESIZE, whoever triggered it) -- remap to the new segment. */
        if (s->buf) munmap(s->buf, (size_t)s->shm_size);
        int fd = shm_open(wins[i].shm_name, O_RDWR, 0, 0);
        if (fd < 0) { s->buf = NULL; return 0; }
        void *p = mmap(NULL, (size_t)wins[i].shm_size, PROT_R | PROT_W, MAP_SHARED, fd);
        close(fd);
        if (p == MAP_FAILED) { s->buf = NULL; return 0; }

        s->buf = (uint32_t *)p;
        s->shm_size = wins[i].shm_size;
        int j = 0;
        while (wins[i].shm_name[j] && j + 1 < (int)sizeof(s->shm_name)) { s->shm_name[j] = wins[i].shm_name[j]; j++; }
        s->shm_name[j] = 0;
        s->w = wins[i].w;
        s->h = wins[i].h;
        return 1;
    }

    return -1; /* window gone */
}

/* Clears the ENTIRE current buffer to col, ignoring any fixed-size content
 * box a client's own fill_rect() might clip to. Use right after a remap
 * (rc > 0 above) for the "letterbox" strategy: a fixed-size UI redraws its
 * normal content anchored top-left afterward, and whatever's left over
 * (the window is now bigger than the UI) stays this flat color instead of
 * showing stale/garbage pixels from the old segment. */
static inline void wm_client_surface_clear(wm_client_surface_t *s, uint32_t col) {
    if (!s->buf) return;
    uint64_t npx = s->shm_size / 4;
    for (uint64_t i = 0; i < npx; i++) s->buf[i] = col;
}