#pragma once
/*
 * wm_protocol.h — control-channel protocol between the ModuOS window
 * manager ($/user/wm) and client apps that want a window.
 *
 * Shape mirrors calc_client.c / calc_service.c: a fixed-size request
 * struct in, a fixed-size response struct out, via invoke().
 *
 * Pixel data itself never goes through this channel — it lives in a
 * shm_open() segment the WM creates on WM_CMD_CREATE and hands the
 * client the name+size for. The client shm_open()s that name (no
 * SHM_O_CREAT — it must already exist) and mmap(MAP_SHARED)s it to get
 * a pointer to the same physical pixels the WM composites from.
 *
 * Usage (client side):
 *   int fd = wm_connect(3000); // polls, no sleep() — see wm_connect() below
 *   if (fd < 0) { ... WM never came up ... }
 *   wm_request_t req = { .cmd = WM_CMD_CREATE, .w = 360, .h = 200 };
 *   strcpy-ish into req.title;
 *   invoke(fd, &req, sizeof(req), &resp, sizeof(resp));
 *   int shmfd = shm_open(resp.shm_name, O_RDWR, 0, 0);
 *   uint32_t *pixels = mmap(NULL, resp.shm_size, PROT_R|PROT_W, MAP_SHARED, shmfd);
 *   // draw into pixels (row-major, 0xAARRGGBB, req.w * req.h)
 *   wm_request_t dmg = { .cmd = WM_CMD_DAMAGE, .win_id = resp.win_id, .x=0,.y=0,.w=req.w,.h=req.h };
 *   invoke(fd, &dmg, sizeof(dmg), &resp2, sizeof(resp2));
 */

#include <stdint.h>

#define WM_SERVICE_PATH "$/user/wm"

#define WM_CMD_CREATE 1  /* -> allocate a window + backing SHM segment       */
#define WM_CMD_DAMAGE 2  /* -> "I redrew this sub-rect of my content, repaint it" */
#define WM_CMD_MOVE   3  /* -> client-requested reposition (rarely needed;
                              users normally move windows by dragging)    */
#define WM_CMD_CLOSE  4  /* -> tear the window down                          */

/* CREATE flags */
#define WM_FLAG_HAS_ALPHA (1u << 0) /* set if content pixels can have a<255.
                                        Unset (default) lets the compositor
                                        take the fast memcpy blit path. */

typedef struct {
    int32_t  cmd;
    int32_t  win_id;   /* ignored for CREATE; required for DAMAGE/MOVE/CLOSE */
    uint32_t w, h;     /* CREATE: window content size (px).
                           DAMAGE: dirty rect size, content-space. */
    int32_t  x, y;     /* DAMAGE: dirty rect origin, content-space (0,0 = top-left of content).
                           MOVE:   new outer top-left, screen-space. */
    uint32_t flags;    /* CREATE: WM_FLAG_* */
    uint32_t pid;      /* CREATE only: client's own getpid(). Used purely for liveness
                           checks (md64api_get_pid_info) so a crashed client's window gets
                           reclaimed — NOT a security boundary, it's self-reported. 0 means
                           "don't track" (older clients, or pid genuinely unknown), and such
                           windows are never auto-closed. */
    char     title[64];/* CREATE only */
} wm_request_t;

typedef struct {
    int32_t  success;
    int32_t  win_id;      /* valid after a successful CREATE */
    uint64_t shm_size;    /* exact size to pass to mmap() after CREATE      */
    char     shm_name[64];/* name to shm_open() after CREATE                */
    char     message[64]; /* human-readable error, set when success == 0    */
} wm_response_t;

/* ------------------------------------------------------------------
 * Client-side connect helper.
 *
 * Apps can start before mdw has finished registering $/user/wm — a single
 * open() can race the WM's own init and fail even though the WM is about
 * to come up fine. Poll instead of failing outright, bounded by a
 * timeout so a genuinely-absent WM doesn't hang the app forever.
 *
 * Deliberately does NOT use sleep() — busy-polls via yield() + time_ms()
 * instead. Costs a bit more CPU during the (short, one-time) connect
 * window than a real sleep would, but doesn't depend on it working.
 *
 * Requires open()/yield()/time_ms() to already be visible, so include
 * libc.h before this header.
 *
 * Returns an open fd on success, or -1 if the WM never showed up within
 * timeout_ms.
 * ------------------------------------------------------------------ */
static inline int wm_connect(uint32_t timeout_ms) {
    uint64_t start = time_ms();
    for (;;) {
        int fd = open(WM_SERVICE_PATH, O_RDWR, 0);
        if (fd >= 0) return fd;
        if (time_ms() - start >= (uint64_t)timeout_ms) return -1;
        yield();
    }
}