#pragma once
/*
 * wm_protocol.h — SHM-only control channel between the ModuOS window
 * manager (mdw) and client apps that want a window.
 *
 * Ported directly from netd's SHM mailbox (see netd_ipc.h): mdw creates
 * one shared-memory mailbox segment at startup (shm_open + SHM_O_CREAT),
 * clients attach to that same segment directly (shm_open with size 0),
 * and requests are exchanged through a single spinlock-guarded slot with
 * a small state machine (IDLE -> REQUEST_PENDING -> RESPONSE_READY ->
 * IDLE). No UserFS ($/user/*, open()/invoke()) anywhere in this path.
 *
 * Like netd's mailbox, this is a SINGLE slot: only one client's control-
 * channel call is in flight at a time system-wide. Others spin on the
 * lock until it's free. That's the same trade-off netd already makes and
 * is fine here too -- CREATE/DAMAGE/MOVE/CLOSE calls are short and this
 * isn't a hot path the way per-frame compositing is.
 *
 * Window pixel buffers are UNCHANGED -- still their own dedicated
 * shm_open() segment per window, created by mdw on WM_CMD_CREATE and
 * handed to the client via shm_name/shm_size in the response, exactly as
 * before. Only the *control* channel moved off UserFS onto this mailbox.
 *
 * Input: each window gets TWO further fixed-size SHM segments, both
 * created by mdw at CREATE time and both independent of w/h --
 *   key_shm_name   -> wm_key_ring_t    (key presses)
 *   mouse_shm_name -> wm_mouse_ring_t  (mouse move/press/release/enter/leave)
 * Neither touches the pixel ABI, and a client that ignores them behaves
 * exactly as it did before they existed.
 *
 * Usage (client side):
 *   wm_shm_t *mb = wm_ipc_connect(3000); // polls; mdw may still be starting
 *   if (!mb) { ... mdw never came up ... }
 *   wm_request_t req; memset(&req, 0, sizeof(req));
 *   req.cmd = WM_CMD_CREATE; req.w = 360; req.h = 200;
 *   wm_response_t resp;
 *   if (wm_ipc_call(mb, &req, &resp, 3000) != 0 || !resp.success) { ... }
 *   int shmfd = shm_open(resp.shm_name, O_RDWR, 0, 0);
 *   uint32_t *pixels = mmap(NULL, resp.shm_size, PROT_R|PROT_W, MAP_SHARED, shmfd);
 *   int keyfd = shm_open(resp.key_shm_name, O_RDWR, 0, 0);
 *   wm_key_ring_t *keys = mmap(NULL, resp.key_shm_size, PROT_R|PROT_W, MAP_SHARED, keyfd);
 *   int msfd = shm_open(resp.mouse_shm_name, O_RDWR, 0, 0);
 *   wm_mouse_ring_t *mice = mmap(NULL, resp.mouse_shm_size, PROT_R|PROT_W, MAP_SHARED, msfd);
 *   ... each loop tick:
 *         KeyboardEventData k;  while (wm_key_ring_pop(keys, &k))   { ... }
 *         wm_mouse_event_t  me; while (wm_mouse_ring_pop(mice, &me)) { ... }
 *   wm_request_t dmg; memset(&dmg, 0, sizeof(dmg));
 *   dmg.cmd = WM_CMD_DAMAGE; dmg.win_id = resp.win_id;
 *   dmg.x=0; dmg.y=0; dmg.w=req.w; dmg.h=req.h;
 *   wm_response_t resp2;
 *   wm_ipc_call(mb, &dmg, &resp2, 3000);
 *
 * Usage (server side / mdw only):
 *   wm_shm_t *mb = wm_ipc_create();   // shm_open+SHM_O_CREAT, mmap, zero it
 *   ... each main-loop iteration, non-blocking ...
 *   if (wm_ipc_get_state(mb) == WM_SLOT_REQUEST_PENDING) {
 *       wm_response_t resp;
 *       wm_service_handle_request(&mb->req, &resp);  // fills resp fully
 *       memcpy(&mb->resp, &resp, sizeof(resp));
 *       wm_ipc_set_state(mb, WM_SLOT_RESPONSE_READY);
 *   }
 */

#include "libc/libc.h"
#include <stdint.h>
#include "../include/moduos/kernel/events/events.h"

#define WM_IPC_SHM_NAME "wm_ipc"

/* -- Per-window key input ring ---------------------------------------
 * mdw owns a second SHM segment per window (separate from the pixel
 * buffer, so the pixel ABI -- exactly w*h*4 bytes, nothing else -- is
 * untouched for every existing client). mdw is the sole writer, the
 * client is the sole reader: a lock-free SPSC ring needs only atomic
 * head/tail, no spinlock, so polling it costs nothing on the hot path.
 *
 * Only key-press events reach the ring; key-release is not forwarded
 * (no client in this codebase needs it yet, and it halves the traffic
 * a terminal has to filter through). WM-level shortcuts (window-switch
 * Tab, compositor quit) are consumed by mdw and never reach the ring.
 */
#define WM_KEY_RING_SIZE 64

/* Max concurrent windowless key feeds (see WM_CMD_KEYFEED_OPEN below). */
#define WM_MAX_KEYFEEDS 4

typedef struct {
    volatile uint32_t head; /* mdw writes here */
    volatile uint32_t tail; /* client writes here (after consuming) */
    KeyboardEventData keys[WM_KEY_RING_SIZE];
} wm_key_ring_t;

/* Client side: pop one key if available. Returns 1 and fills *out, or
 * 0 if the ring is empty. Safe to call every frame/tick. */
static inline int wm_key_ring_pop(wm_key_ring_t *ring, KeyboardEventData *out) {
    uint32_t tail = __atomic_load_n(&ring->tail, __ATOMIC_RELAXED);
    uint32_t head = __atomic_load_n(&ring->head, __ATOMIC_ACQUIRE);
    if (tail == head) return 0; /* empty */
    *out = ring->keys[tail % WM_KEY_RING_SIZE];
    __atomic_store_n(&ring->tail, tail + 1, __ATOMIC_RELEASE);
    return 1;
}

/* mdw side: push one key. Drops the oldest entry (advances tail) if
 * the client isn't draining fast enough -- a stalled client shouldn't
 * be able to wedge the compositor's key delivery. */
static inline void wm_key_ring_push(wm_key_ring_t *ring, const KeyboardEventData *k) {
    uint32_t head = __atomic_load_n(&ring->head, __ATOMIC_RELAXED);
    uint32_t tail = __atomic_load_n(&ring->tail, __ATOMIC_ACQUIRE);
    if (head - tail >= WM_KEY_RING_SIZE) {
        __atomic_store_n(&ring->tail, tail + 1, __ATOMIC_RELEASE); /* drop oldest */
    }
    ring->keys[head % WM_KEY_RING_SIZE] = *k;
    __atomic_store_n(&ring->head, head + 1, __ATOMIC_RELEASE);
}

/* -- Per-window mouse input ring -------------------------------------
 * Third SHM segment per window, same SPSC discipline as the key ring
 * (mdw sole writer, client sole reader, no spinlock). Fixed size, not
 * derived from w/h, so again: pixel ABI untouched.
 *
 * Coordinates are CONTENT-SPACE: (0,0) is the top-left pixel of the
 * client's own buffer, i.e. below the title bar. The client never has
 * to know where its window sits on screen, and never has to subtract
 * WM_TITLEBAR_H itself. Coordinates CAN go negative or past w/h during
 * a drag that leaves the window (see grabbing below) -- clamp if you
 * care, most clients just hit-test and ignore misses.
 *
 * Delivery rules (mdw side):
 *   - MOVE goes to the window under the cursor, content region only.
 *     Title bar / close button / taskbar stay WM-private: a client
 *     never sees the events that drive its own chrome.
 *   - A press inside content GRABS the pointer for that window until
 *     every button is released. That's what makes click-and-drag work
 *     inside a client (sliders, selections, canvas drawing) even when
 *     the cursor wanders off the window mid-drag.
 *   - ENTER/LEAVE fire when the content-region hover target changes,
 *     so a client can un-highlight a hovered button when the pointer
 *     leaves without having to poll.
 *   - Only the focused/raised window gets presses; a press also raises
 *     and focuses, exactly as before.
 */
#define WM_MOUSE_RING_SIZE 64

#define WM_MOUSE_MOVE   1
#define WM_MOUSE_DOWN   2
#define WM_MOUSE_UP     3
#define WM_MOUSE_ENTER  4
#define WM_MOUSE_LEAVE  5

/* Button mask bits — same bit order the kernel's MouseEventData uses. */
#define WM_MBTN_LEFT    0x1u
#define WM_MBTN_RIGHT   0x2u
#define WM_MBTN_MIDDLE  0x4u

typedef struct {
    int32_t  type;     /* WM_MOUSE_*                                        */
    int32_t  x, y;     /* content-space position                            */
    uint32_t buttons;  /* full button mask AFTER this event                 */
    uint32_t changed;  /* DOWN/UP: which button(s) changed. 0 otherwise.    */
} wm_mouse_event_t;

typedef struct {
    volatile uint32_t head; /* mdw writes here */
    volatile uint32_t tail; /* client writes here (after consuming) */
    wm_mouse_event_t  events[WM_MOUSE_RING_SIZE];
} wm_mouse_ring_t;

/* Client side: pop one mouse event. Returns 1 and fills *out, or 0 if
 * the ring is empty. Drain it in a while() loop each tick. */
static inline int wm_mouse_ring_pop(wm_mouse_ring_t *ring, wm_mouse_event_t *out) {
    uint32_t tail = __atomic_load_n(&ring->tail, __ATOMIC_RELAXED);
    uint32_t head = __atomic_load_n(&ring->head, __ATOMIC_ACQUIRE);
    if (tail == head) return 0; /* empty */
    *out = ring->events[tail % WM_MOUSE_RING_SIZE];
    __atomic_store_n(&ring->tail, tail + 1, __ATOMIC_RELEASE);
    return 1;
}

/* mdw side: push one mouse event, dropping the oldest on overflow.
 * Mouse move traffic is far heavier than key traffic, so dropping
 * matters more here: a client that stalls for a second comes back to
 * the newest 64 events, not a second-old backlog it has to chew
 * through before it sees where the pointer actually is. */
static inline void wm_mouse_ring_push(wm_mouse_ring_t *ring, const wm_mouse_event_t *e) {
    uint32_t head = __atomic_load_n(&ring->head, __ATOMIC_RELAXED);
    uint32_t tail = __atomic_load_n(&ring->tail, __ATOMIC_ACQUIRE);
    if (head - tail >= WM_MOUSE_RING_SIZE) {
        __atomic_store_n(&ring->tail, tail + 1, __ATOMIC_RELEASE); /* drop oldest */
    }
    ring->events[head % WM_MOUSE_RING_SIZE] = *e;
    __atomic_store_n(&ring->head, head + 1, __ATOMIC_RELEASE);
}

/* -- Cursor ids (mirror mdw's mouse.ilib ids) ------------------------
 * Used with WM_CMD_SET_CURSOR so a client can say "while the pointer is
 * over my content, show a text caret / crosshair / wait cursor". mdw
 * still owns drawing it -- the client never touches the screen. */
#define WM_CURSOR_POINTER   0
#define WM_CURSOR_CROSS     1
#define WM_CURSOR_ERASER    2
#define WM_CURSOR_HAND      3
#define WM_CURSOR_MOVE_ALL  4
#define WM_CURSOR_NO        5
#define WM_CURSOR_PENCIL    6
#define WM_CURSOR_TEXT      7
#define WM_CURSOR_WAIT      8

/* How long a client will spin trying to grab the mailbox lock before
 * giving up (e.g. another client is mid-request). Mirrors
 * NETD_IPC_LOCK_TIMEOUT_MS. */
#define WM_IPC_LOCK_TIMEOUT_MS 2000

#define WM_CMD_CREATE     1  /* -> allocate a window + backing SHM segment       */
#define WM_CMD_DAMAGE     2  /* -> "I redrew this sub-rect of my content, repaint it" */
#define WM_CMD_MOVE       3  /* -> client-requested reposition (rarely needed;
                                  users normally move windows by dragging)    */
#define WM_CMD_CLOSE      4  /* -> tear the window down                          */
#define WM_CMD_SET_CURSOR 5  /* -> cursor to show over my content; id in flags   */
#define WM_CMD_KEYFEED_OPEN  6 /* -> global key ring, no window. resp.win_id is the
                                     feed id (pass to KEYFEED_CLOSE), key_shm_name/
                                     key_shm_size as usual. */
#define WM_CMD_KEYFEED_CLOSE 7 /* -> req.win_id = feed id from KEYFEED_OPEN */

/* -- Window state control + the "Window API" ---------------------------
 * These all take req.win_id. None of them check who's asking -- same
 * trust model as MOVE/CLOSE already had (owner_pid is self-reported,
 * not a security boundary). That's deliberate: it's what lets an
 * external taskbar app drive ANY window's state through this exact
 * mailbox, with no separate privileged channel needed.
 */
#define WM_CMD_MINIMIZE      8  /* -> hide from screen, keep alive. any win_id.
                                       Does not touch the content buffer. */
#define WM_CMD_MAXIMIZE      9  /* -> fill the available (non-reserved) screen area.
                                       Reallocates the content SHM segment to the new
                                       content size -- resp carries the new shm_name/
                                       shm_size exactly like CREATE does. See the
                                       remap-detection note on wm_win_info_t above if
                                       you're calling this on a win_id you don't own. */
#define WM_CMD_RESTORE        10 /* -> back to WM_STATE_NORMAL. From MAXIMIZED/
                                       FULLSCREEN this reallocates back to the saved
                                       pre-maximize content size (new shm info in
                                       resp, same caveat as MAXIMIZE). From MINIMIZED
                                       it's just an unhide, no buffer change. */
#define WM_CMD_FULLSCREEN    11 /* -> fill the whole screen, chrome hidden. Same
                                       reallocation behavior as MAXIMIZE. */
#define WM_CMD_RESIZE        12 /* -> req.w/h = new content size. Reallocates the
                                       content SHM segment; resp carries the new
                                       shm_name/shm_size, same contract as CREATE. */
#define WM_CMD_FOCUS         14 /* -> raise + focus this win_id. Restores it first
                                       if it's minimized -- that's what a taskbar
                                       click on a minimized entry should do. */

/* -> fills resp.windows[]/resp.window_count. Read-only snapshot of every
 * live window, for a taskbar app (or anything else) to enumerate. req
 * fields are ignored entirely for this command. */
#define WM_CMD_LIST_WINDOWS  13

/* CREATE flags */
#define WM_FLAG_HAS_ALPHA (1u << 0) /* set if content pixels can have a<255.
                                        Unset (default) lets the compositor
                                        take the fast memcpy blit path. */
#define WM_FLAG_DOCK      (1u << 1) /* chrome-less window pinned to a screen edge --
                                        no titlebar, no border, not draggable, can't
                                        be minimized/maximized/fullscreened. For dock/
                                        panel/taskbar-style apps. See dock_edge below.
                                        The whole window is "content" -- (0,0) in the
                                        mouse/key rings is its own top-left pixel,
                                        same as a normal window's content region. */
#define WM_FLAG_DESKTOP   (1u << 2) /* chrome-less window that always fills the WHOLE
                                        screen (req.w/req.h are ignored, same as
                                        WM_FLAG_DOCK ignoring the spanned dimension)
                                        and is pinned to the very BACK of the normal
                                        z-order -- every other normal window draws on
                                        top of it, and it can never be raised, focused,
                                        minimized, maximized, fullscreened, moved, or
                                        resized. Clicks only reach it where nothing
                                        else covers that point, exactly like a desktop
                                        icon layer. Meant for exactly one client (a
                                        desktop-shell app); mdw does not enforce
                                        single-instance, so a second one will just
                                        stack invisibly behind the first. */

/* CREATE, WM_FLAG_DOCK only: which screen edge to pin to. The window is
 * stretched to span that whole edge; w (top/bottom) or h (left/right) is
 * ignored and computed from the screen size, the OTHER dimension (the
 * thickness reserved off that edge) is taken from req.w/req.h as given.
 * mdw keeps a running "reserved" margin per edge so normal windows'
 * MAXIMIZE/FULLSCREEN and default cascade placement avoid docked space,
 * exactly like the old hardcoded taskbar strip did -- except now it's
 * driven by whatever dock windows actually exist. */
#define WM_DOCK_EDGE_TOP    0
#define WM_DOCK_EDGE_BOTTOM 1
#define WM_DOCK_EDGE_LEFT   2
#define WM_DOCK_EDGE_RIGHT  3

/* Window states, reported per-window by WM_CMD_LIST_WINDOWS and driven by
 * MINIMIZE/MAXIMIZE/RESTORE/FULLSCREEN. Dock windows are always
 * WM_STATE_NORMAL -- those state transitions don't apply to them. */
#define WM_STATE_NORMAL     0
#define WM_STATE_MINIMIZED  1
#define WM_STATE_MAXIMIZED  2
#define WM_STATE_FULLSCREEN 3

/* Cap on live windows mdw will ever track, and the fixed size of
 * resp.windows[] in WM_CMD_LIST_WINDOWS. Protocol-level (not just an
 * mdw implementation detail) so a client can size its own buffers/loops
 * against it without asking mdw first. */
#define WM_MAX_WINDOWS 16

typedef struct {
    int32_t  cmd;
    int32_t  win_id;   /* ignored for CREATE/LIST_WINDOWS; required for everything else */
    uint32_t w, h;     /* CREATE: window content size (px), or dock thickness (see
                           WM_FLAG_DOCK above).
                           DAMAGE: dirty rect size, content-space.
                           RESIZE: new content size. */
    int32_t  x, y;     /* DAMAGE: dirty rect origin, content-space (0,0 = top-left of content).
                           MOVE:   new outer top-left, screen-space. */
    uint32_t flags;    /* CREATE: WM_FLAG_*.  SET_CURSOR: WM_CURSOR_* id. */
    uint32_t pid;      /* CREATE only: client's own getpid(). Used purely for liveness
                           checks so a crashed client's window gets reclaimed -- NOT a
                           security boundary, it's self-reported. 0 means "don't track". */
    uint32_t dock_edge;/* CREATE, WM_FLAG_DOCK only: WM_DOCK_EDGE_* */
    char     title[64];/* CREATE only */
} wm_request_t;

/* One entry in a WM_CMD_LIST_WINDOWS snapshot.
 *
 * shm_name/shm_size are included so a client can detect a buffer that was
 * resized out from under it. MAXIMIZE/FULLSCREEN/RESTORE reallocate the
 * content SHM segment (the compositor can't blit past a buffer sized for
 * the OLD content dimensions), and mdw's response goes to whoever issued
 * the command -- which is the window's own client in the common case
 * (its own titlebar button), and gets remapped immediately from that
 * response. But when an external controller (a taskbar app, via this
 * Window API) calls MAXIMIZE/FULLSCREEN on someone else's win_id, the
 * OWNING client never sees that response directly. That client should
 * poll wm_list_windows() each tick (same cadence it already drains its
 * key/mouse rings at) and compare shm_name against what it currently has
 * mapped: on a mismatch, munmap the old pointer and shm_open+mmap the
 * new one before its next DAMAGE. Until it does, mdw still renders
 * correctly (the new segment is real, just blank until the client
 * redraws into it) -- this is a staleness/redraw gap, not a memory-
 * safety one.
 */
typedef struct {
    int32_t  win_id;
    int32_t  state;    /* WM_STATE_* */
    int32_t  focused;  /* 1 if this is the currently-focused window */
    int32_t  is_dock;  /* 1 if created with WM_FLAG_DOCK */
    int32_t  is_desktop; /* 1 if created with WM_FLAG_DESKTOP */
    uint32_t dock_edge; /* WM_DOCK_EDGE_*, valid only if is_dock */
    int32_t  x, y;     /* current outer top-left, screen-space */
    int32_t  w, h;     /* current content size */
    uint64_t shm_size; /* current content SHM segment size */
    char     shm_name[64]; /* current content SHM segment name */
    char     title[64];
} wm_win_info_t;

typedef struct {
    int32_t  success;
    int32_t  win_id;      /* valid after a successful CREATE */
    uint64_t shm_size;    /* exact size to pass to mmap() after CREATE      */
    char     shm_name[64];/* name to shm_open() after CREATE                */
    char     message[64]; /* human-readable error, set when success == 0    */

    /* CREATE only. Second, independent SHM segment: fixed-size key ring
     * for this window. sizeof(wm_key_ring_t) always -- not derived from
     * w/h -- so older clients that only read shm_size/shm_name (the
     * pixel fields) are completely unaffected by this addition. */
    uint64_t key_shm_size;
    char     key_shm_name[64];

    /* CREATE only. Third segment: the mouse ring, same deal. Appended
     * after the key fields on purpose -- every offset that existed
     * before is byte-identical, so a client built against the old
     * header keeps working untouched against the new mdw. */
    uint64_t mouse_shm_size;
    char     mouse_shm_name[64];

    /* WM_CMD_LIST_WINDOWS only. Appended last, same reasoning as the key/
     * mouse fields above: every byte offset that existed before is
     * unchanged, so nothing that ignores these two fields is affected. */
    int32_t       window_count;
    wm_win_info_t windows[WM_MAX_WINDOWS];
} wm_response_t;

/* -- Mailbox slot — identical shape to netd_shm_t --------------------- */

typedef enum {
    WM_SLOT_IDLE            = 0, /* free, nobody holds it */
    WM_SLOT_REQUEST_PENDING = 1, /* client posted req, waiting on mdw */
    WM_SLOT_RESPONSE_READY  = 2, /* mdw posted resp, waiting on client */
} wm_slot_state_t;

typedef struct {
    volatile uint32_t lock;   /* 0 = free, 1 = held. Only clients CAS this. */
    volatile uint32_t state;  /* wm_slot_state_t */
    wm_request_t      req;
    wm_response_t     resp;
} wm_shm_t;

/* -- Spinlock + state helpers — same pattern as netd_ipc.h, built on
 *    GCC atomic builtins (no futex/semaphore syscall exists) -------- */

static inline int wm_ipc_lock_try(wm_shm_t *shm) {
    uint32_t expected = 0;
    return __atomic_compare_exchange_n(&shm->lock, &expected, 1, 0,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

static inline int wm_ipc_lock(wm_shm_t *shm, uint32_t timeout_ms) {
    uint64_t deadline = time_ms() + timeout_ms;
    while (!wm_ipc_lock_try(shm)) {
        if (time_ms() >= deadline) return -1;
        yield();
    }
    return 0;
}

static inline void wm_ipc_unlock(wm_shm_t *shm) {
    __atomic_store_n(&shm->lock, 0, __ATOMIC_RELEASE);
}

static inline wm_slot_state_t wm_ipc_get_state(wm_shm_t *shm) {
    return (wm_slot_state_t)__atomic_load_n(&shm->state, __ATOMIC_ACQUIRE);
}

static inline void wm_ipc_set_state(wm_shm_t *shm, wm_slot_state_t s) {
    __atomic_store_n(&shm->state, (uint32_t)s, __ATOMIC_RELEASE);
}

/* -- Server side (mdw only): create + own the mailbox segment -------- */

/* shm_open(..., SHM_O_CREAT) + mmap + zero, exactly like netd's server
 * setup. Returns NULL on failure (caller should treat like the old
 * userfs_register() failure path -- fatal for mdw). */
static inline wm_shm_t *wm_ipc_create(void) {
    int h = shm_open(WM_IPC_SHM_NAME, O_RDWR | SHM_O_CREAT, 0644, sizeof(wm_shm_t));
    if (h < 0) return NULL;
    wm_shm_t *shm = (wm_shm_t *)mmap(NULL, sizeof(wm_shm_t), PROT_R | PROT_W, MAP_SHARED, h);
    if (shm == MAP_FAILED) return NULL;
    memset(shm, 0, sizeof(*shm));
    return shm;
}

/* -- Client side: attach + one full request/response round trip ------ */

/* Poll-attach to mdw's mailbox segment. mdw may not have created it yet
 * at boot -- a single shm_open() can race mdw's own startup the same way
 * the old open($/user/wm) could, so this busy-polls instead of failing
 * on the first race, bounded by timeout_ms. Deliberately no sleep() --
 * same reasoning as the old wm_connect(). Returns a mapped wm_shm_t* on
 * success, or NULL if mdw never showed up in time. */
static inline wm_shm_t *wm_ipc_connect(uint32_t timeout_ms) {
    uint64_t start = time_ms();
    for (;;) {
        int h = shm_open(WM_IPC_SHM_NAME, O_RDWR, 0, 0); /* 0 size = attach existing */
        if (h >= 0) {
            wm_shm_t *shm = (wm_shm_t *)mmap(NULL, sizeof(wm_shm_t), PROT_R | PROT_W, MAP_SHARED, h);
            if (shm != MAP_FAILED) return shm;
        }
        if (time_ms() - start >= (uint64_t)timeout_ms) return NULL;
        yield();
    }
}

/* Returns 0 on success (*out_resp filled in), -1 on timeout (either the
 * mailbox stayed locked by another client, or mdw never answered within
 * wait_ms). out_resp is always fully zeroed first, so a timed-out caller
 * never reads stale/uninitialized fields -- this is the exact bug class
 * that hit the old UserFS-based demo_welcome.c/demo_notes.c/mdw_test.c,
 * closed off at the helper level so no caller can reintroduce it. */
static inline int wm_ipc_call(wm_shm_t *shm, const wm_request_t *req,
                               wm_response_t *out_resp, uint32_t wait_ms) {
    memset(out_resp, 0, sizeof(*out_resp));

    if (wm_ipc_lock(shm, WM_IPC_LOCK_TIMEOUT_MS) != 0) return -1;

    memcpy(&shm->req, req, sizeof(*req));
    wm_ipc_set_state(shm, WM_SLOT_REQUEST_PENDING);

    uint64_t deadline = time_ms() + wait_ms;
    int got_response = 0;
    while (time_ms() < deadline) {
        if (wm_ipc_get_state(shm) == WM_SLOT_RESPONSE_READY) {
            got_response = 1;
            break;
        }
        yield();
    }

    if (got_response) memcpy(out_resp, &shm->resp, sizeof(*out_resp));

    wm_ipc_set_state(shm, WM_SLOT_IDLE);
    wm_ipc_unlock(shm);

    return got_response ? 0 : -1;
}

/* -- Small client conveniences ---------------------------------------- */

/* Fire-and-check a damage rect. Returns 0 on success. */
static inline int wm_damage(wm_shm_t *mb, int win_id, int x, int y, int w, int h) {
    wm_request_t req; memset(&req, 0, sizeof(req));
    req.cmd = WM_CMD_DAMAGE; req.win_id = win_id;
    req.x = x; req.y = y; req.w = (uint32_t)w; req.h = (uint32_t)h;
    wm_response_t resp;
    if (wm_ipc_call(mb, &req, &resp, 3000) != 0) return -1;
    return resp.success ? 0 : -1;
}

static inline int wm_set_cursor(wm_shm_t *mb, int win_id, uint32_t cursor_id) {
    wm_request_t req; memset(&req, 0, sizeof(req));
    req.cmd = WM_CMD_SET_CURSOR; req.win_id = win_id; req.flags = cursor_id;
    wm_response_t resp;
    if (wm_ipc_call(mb, &req, &resp, 3000) != 0) return -1;
    return resp.success ? 0 : -1;
}

/* Open a windowless global key feed: no WM_CMD_CREATE, no pixel buffer,
 * just every key press mdw sees, regardless of window focus. Maps the
 * ring and hands back *out_ring plus the feed id (needed for CLOSE).
 * Returns 0 on success. */
static inline int wm_keyfeed_open(wm_shm_t *mb, wm_key_ring_t **out_ring, int *out_feed_id) {
    wm_request_t req; memset(&req, 0, sizeof(req));
    req.cmd = WM_CMD_KEYFEED_OPEN;
    wm_response_t resp;
    if (wm_ipc_call(mb, &req, &resp, 3000) != 0 || !resp.success) return -1;

    int fd = shm_open(resp.key_shm_name, O_RDWR, 0, 0);
    if (fd < 0) return -1;
    void *ptr = mmap(NULL, (size_t)resp.key_shm_size, PROT_R | PROT_W, MAP_SHARED, fd);
    if (ptr == MAP_FAILED) return -1;

    *out_ring = (wm_key_ring_t *)ptr;
    *out_feed_id = resp.win_id;
    return 0;
}

static inline int wm_keyfeed_close(wm_shm_t *mb, int feed_id) {
    wm_request_t req; memset(&req, 0, sizeof(req));
    req.cmd = WM_CMD_KEYFEED_CLOSE; req.win_id = feed_id;
    wm_response_t resp;
    if (wm_ipc_call(mb, &req, &resp, 3000) != 0) return -1;
    return resp.success ? 0 : -1;
}

/* -- Window API convenience wrappers ----------------------------------
 * Each is a single mailbox round trip against an arbitrary win_id -- a
 * normal client typically passes its own resp.win_id from CREATE, while
 * a taskbar app passes whatever win_id it got back from wm_list_windows()
 * below. Returns 0 on success, -1 on timeout or WM-side failure.
 */
static inline int wm_minimize(wm_shm_t *mb, int win_id) {
    wm_request_t req; memset(&req, 0, sizeof(req));
    req.cmd = WM_CMD_MINIMIZE; req.win_id = win_id;
    wm_response_t resp;
    if (wm_ipc_call(mb, &req, &resp, 3000) != 0) return -1;
    return resp.success ? 0 : -1;
}

/* out_resp is optional but you want it if this is YOUR OWN window: on
 * success resp.shm_name/shm_size describe the new content buffer and you
 * must munmap the old one and map the new one before your next DAMAGE
 * (see the remap-detection note on wm_win_info_t if you're calling this
 * on a win_id that isn't yours). */
static inline int wm_maximize(wm_shm_t *mb, int win_id, wm_response_t *out_resp) {
    wm_request_t req; memset(&req, 0, sizeof(req));
    req.cmd = WM_CMD_MAXIMIZE; req.win_id = win_id;
    wm_response_t local, *r = out_resp ? out_resp : &local;
    if (wm_ipc_call(mb, &req, r, 3000) != 0) return -1;
    return r->success ? 0 : -1;
}

static inline int wm_restore(wm_shm_t *mb, int win_id, wm_response_t *out_resp) {
    wm_request_t req; memset(&req, 0, sizeof(req));
    req.cmd = WM_CMD_RESTORE; req.win_id = win_id;
    wm_response_t local, *r = out_resp ? out_resp : &local;
    if (wm_ipc_call(mb, &req, r, 3000) != 0) return -1;
    return r->success ? 0 : -1;
}

static inline int wm_fullscreen(wm_shm_t *mb, int win_id, wm_response_t *out_resp) {
    wm_request_t req; memset(&req, 0, sizeof(req));
    req.cmd = WM_CMD_FULLSCREEN; req.win_id = win_id;
    wm_response_t local, *r = out_resp ? out_resp : &local;
    if (wm_ipc_call(mb, &req, r, 3000) != 0) return -1;
    return r->success ? 0 : -1;
}

static inline int wm_resize(wm_shm_t *mb, int win_id, int w, int h, wm_response_t *out_resp) {
    wm_request_t req; memset(&req, 0, sizeof(req));
    req.cmd = WM_CMD_RESIZE; req.win_id = win_id;
    req.w = (uint32_t)w; req.h = (uint32_t)h;
    wm_response_t local, *r = out_resp ? out_resp : &local;
    if (wm_ipc_call(mb, &req, r, 3000) != 0) return -1;
    return r->success ? 0 : -1;
}

/* Raise + focus any window (restores it first if minimized). This is
 * what a taskbar entry click should call. */
static inline int wm_focus(wm_shm_t *mb, int win_id) {
    wm_request_t req; memset(&req, 0, sizeof(req));
    req.cmd = WM_CMD_FOCUS; req.win_id = win_id;
    wm_response_t resp;
    if (wm_ipc_call(mb, &req, &resp, 3000) != 0) return -1;
    return resp.success ? 0 : -1;
}

/* Snapshot every live window into out_windows (caller-provided array of
 * at least WM_MAX_WINDOWS entries). Returns the count (>= 0) on success,
 * -1 on timeout/failure. This is the read side of the Window API a
 * taskbar app polls to know what to draw. */
static inline int wm_list_windows(wm_shm_t *mb, wm_win_info_t *out_windows) {
    wm_request_t req; memset(&req, 0, sizeof(req));
    req.cmd = WM_CMD_LIST_WINDOWS;
    wm_response_t resp;
    if (wm_ipc_call(mb, &req, &resp, 3000) != 0 || !resp.success) return -1;
    int n = resp.window_count;
    if (n < 0) n = 0;
    if (n > WM_MAX_WINDOWS) n = WM_MAX_WINDOWS;
    if (out_windows) memcpy(out_windows, resp.windows, (size_t)n * sizeof(wm_win_info_t));
    return n;
}

/* Create a chrome-less window pinned to a screen edge (taskbar/dock/panel
 * style). thickness is the dock's size perpendicular to the edge (height
 * for top/bottom, width for left/right) -- the span along the edge is
 * always the full screen dimension, computed by mdw. Same success/fill-
 * out contract as a normal CREATE (see wm_ipc_connect()'s usage comment
 * at the top of this file): map resp.shm_name/key_shm_name/mouse_shm_name
 * exactly as you would for a normal window. */
static inline int wm_dock_create(wm_shm_t *mb, const char *title, uint32_t edge,
                                  uint32_t thickness, wm_response_t *out_resp) {
    wm_request_t req; memset(&req, 0, sizeof(req));
    req.cmd = WM_CMD_CREATE;
    req.flags = WM_FLAG_DOCK;
    req.dock_edge = edge;
    if (edge == WM_DOCK_EDGE_TOP || edge == WM_DOCK_EDGE_BOTTOM) { req.w = 1; req.h = thickness; }
    else                                                          { req.w = thickness; req.h = 1; }
    if (title) { size_t i = 0; while (title[i] && i + 1 < sizeof(req.title)) { req.title[i] = title[i]; i++; } req.title[i] = 0; }
    return wm_ipc_call(mb, &req, out_resp, 3000);
}

/* Create the chrome-less, always-behind-everything, unfocusable full-
 * screen window a desktop-shell app draws its background/icons into. w/h
 * in the request are ignored (mdw always sizes it to the current screen,
 * same as WM_FLAG_DOCK ignoring the spanned dimension) -- read the
 * granted resp.shm_size back, or just call wm_list_windows() right after
 * and read this window's own w/h off its wm_win_info_t entry, to learn
 * the actual screen size. Same success/fill-out contract as a normal
 * CREATE otherwise. */
static inline int wm_desktop_create(wm_shm_t *mb, const char *title, wm_response_t *out_resp) {
    wm_request_t req; memset(&req, 0, sizeof(req));
    req.cmd = WM_CMD_CREATE;
    req.flags = WM_FLAG_DESKTOP;
    if (title) { size_t i = 0; while (title[i] && i + 1 < sizeof(req.title)) { req.title[i] = title[i]; i++; } req.title[i] = 0; }
    return wm_ipc_call(mb, &req, out_resp, 3000);
}