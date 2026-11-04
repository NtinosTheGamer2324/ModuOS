/*
 * mdrun.c — "Run" dialog for ModuOS: type a path, launch it.
 * Only accepts paths ending in ".sqr". Uses the standard mdw client
 * protocol (wm_protocol.h) for its window, keyboard and mouse — no
 * special-cased mdw support, same as any other client.
 */

#include "libc.h"
#include "string.h"
#include "lib_fnt.h"
#include "wm_protocol.h"
#include "wm_client_resize.h"
#include <stdint.h>

#define FONT_PRIMARY  "/ModuOS/shared/assets/fonts/Terminus.fnt"
#define FONT_FALLBACK "/ModuOS/shared/assets/fonts/Unicode.fnt"

#define WIN_W 420
#define WIN_H 110

#define PAD        12
#define LABEL_Y    12
#define FIELD_Y    32
#define FIELD_H    22
#define BTN_Y      66
#define BTN_W      90
#define BTN_H      26
#define BTN_GAP    8
#define MSG_Y      BTN_Y + BTN_H + 8

#define C_BG        0xFF1B1F27u
#define C_TEXT      0xFFDCE2EEu
#define C_TEXT_DIM  0xFF8C94A6u
#define C_FIELD     0xFF11151Cu
#define C_ACCENT    0xFF6FB2FFu
#define C_ERR       0xFFE0483Fu
#define C_OK        0xFF5FD37Fu
#define C_BTN       0xFF303848u
#define C_BTN_HOVER 0xFF3D4759u
#define C_BTN_DOWN  0xFF232B39u
#define C_BTN_EDGE  0xFF454F63u

enum { BTN_RUN = 0, BTN_CANCEL, BTN_COUNT };
typedef struct { int x, y, w, h; const char *label; } btn_t;
static btn_t g_btns[BTN_COUNT];

static uint32_t   *g_buf;
static fnt_font_t *g_font;

/* Row width of the ACTUAL mapped buffer -- equals WIN_W normally, but
 * after MAXIMIZE/FULLSCREEN mdw reallocates to a bigger segment whose row
 * stride is the new (larger) width, not WIN_W. This dialog's layout stays
 * fixed-size (letterboxed top-left); only pixel-indexing math needs the
 * real stride. */
static int g_stride = WIN_W;

static char field[256];
static char msg[128];
static uint32_t msg_col;
static int hover_btn = -1;
static int active_btn = -1;
static int dirty = 1;
static int quit = 0;

static int sl(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void sset(char *d, int cap, const char *s) { int n=0; while (*s && n<cap-1) d[n++]=*s++; d[n]=0; }

static int ends_with_sqr(const char *s) {
    int n = sl(s);
    if (n < 4) return 0;
    return s[n-4]=='.' && s[n-3]=='s' && s[n-2]=='q' && s[n-1]=='r';
}

/* -- drawing ----------------------- */

static void fill_rect(int x, int y, int w, int h, uint32_t col) {
    int x1=x+w, y1=y+h;
    if (x<0) x=0; if (y<0) y=0;
    if (x1>WIN_W) x1=WIN_W; if (y1>WIN_H) y1=WIN_H;
    for (int yy=y; yy<y1; yy++)
        for (int xx=x; xx<x1; xx++)
            g_buf[(size_t)yy*g_stride+xx] = col;
}

static void frame_rect(int x, int y, int w, int h, uint32_t col) {
    fill_rect(x,y,w,1,col); fill_rect(x,y+h-1,w,1,col);
    fill_rect(x,y,1,h,col); fill_rect(x+w-1,y,1,h,col);
}

static int text_w(const char *s) {
    if (!g_font || !s) return 0;
    int w=0;
    while (*s) { fnt_glyph_t *gl=fnt_get_glyph(g_font,(uint32_t)(unsigned char)*s); if (gl) w+=gl->width; s++; }
    return w;
}

static void draw_text(int x, int y, const char *s, uint32_t col) {
    if (!g_font || !s) return;
    int cx=x;
    while (*s) {
        fnt_glyph_t *gl=fnt_get_glyph(g_font,(uint32_t)(unsigned char)*s);
        if (gl) {
            for (int dy=0; dy<gl->bitmap_height; dy++)
                for (int dx=0; dx<gl->bitmap_width; dx++)
                    if (fnt_get_pixel(gl,dx,dy)) {
                        int px=cx+dx, py=y+dy;
                        if (px>=0 && px<WIN_W && py>=0 && py<WIN_H)
                            g_buf[(size_t)py*g_stride+px]=col;
                    }
            cx += gl->width;
        }
        s++;
    }
}

static void draw_text_center(int x, int y, int w, const char *s, uint32_t col) {
    draw_text(x + (w - text_w(s))/2, y, s, col);
}

static fnt_font_t *try_load_font(const char *path) {
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return NULL;
    long fsz = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    fnt_font_t *out = NULL;
    if (fsz > 0 && fsz < 4*1024*1024) {
        void *data = malloc((size_t)fsz);
        if (data) {
            size_t got=0;
            while (got < (size_t)fsz) {
                ssize_t r = read(fd, (uint8_t*)data+got, (size_t)fsz-got);
                if (r <= 0) break;
                got += (size_t)r;
            }
            if (got == (size_t)fsz) out = fnt_load_font(data, got);
            free(data);
        }
    }
    close(fd);
    return out;
}

static void layout_buttons(void) {
    int x = WIN_W - PAD - BTN_W;
    g_btns[BTN_CANCEL].x=x; g_btns[BTN_CANCEL].y=BTN_Y; g_btns[BTN_CANCEL].w=BTN_W; g_btns[BTN_CANCEL].h=BTN_H;
    g_btns[BTN_CANCEL].label="Cancel";
    x -= BTN_W + BTN_GAP;
    g_btns[BTN_RUN].x=x; g_btns[BTN_RUN].y=BTN_Y; g_btns[BTN_RUN].w=BTN_W; g_btns[BTN_RUN].h=BTN_H;
    g_btns[BTN_RUN].label="Run";
}

static int hit_button(int x, int y) {
    for (int i=0; i<BTN_COUNT; i++) {
        btn_t *b=&g_btns[i];
        if (x>=b->x && x<b->x+b->w && y>=b->y && y<b->y+b->h) return i;
    }
    return -1;
}

static void draw_button(int id) {
    btn_t *b=&g_btns[id];
    int hot=(hover_btn==id), down=(active_btn==id && hot);
    uint32_t bg = down ? C_BTN_DOWN : (hot ? C_BTN_HOVER : C_BTN);
    fill_rect(b->x,b->y,b->w,b->h,bg);
    frame_rect(b->x,b->y,b->w,b->h, hot?C_ACCENT:C_BTN_EDGE);
    draw_text_center(b->x, b->y+(b->h-8)/2, b->w, b->label, C_TEXT);
}

static void render(void) {
    fill_rect(0,0,WIN_W,WIN_H,C_BG);
    draw_text(PAD, LABEL_Y, "Run (.sqr only):", C_TEXT_DIM);

    fill_rect(PAD, FIELD_Y, WIN_W-2*PAD, FIELD_H, C_FIELD);
    frame_rect(PAD, FIELD_Y, WIN_W-2*PAD, FIELD_H, C_ACCENT);
    draw_text(PAD+6, FIELD_Y+6, field, C_TEXT);
    fill_rect(PAD+6+text_w(field)+1, FIELD_Y+4, 1, FIELD_H-8, C_ACCENT);

    draw_button(BTN_RUN);
    draw_button(BTN_CANCEL);

    if (msg[0]) draw_text(PAD, MSG_Y, msg, msg_col);
}

/* -- actions --------------------------------------------------------- */

static void set_msg(const char *s, uint32_t col) { sset(msg, sizeof(msg), s); msg_col = col; dirty = 1; }

static void do_run(void) {
    if (!field[0]) { set_msg("Enter a path.", C_ERR); return; }
    if (!ends_with_sqr(field)) { set_msg("Only .sqr files can be run here.", C_ERR); return; }

    int pid = fork();
    if (pid < 0) { set_msg("fork() failed.", C_ERR); return; }
    if (pid == 0) {
        char *argv[2]; argv[0] = field; argv[1] = NULL;
        execve(field, argv, NULL);
        exit(1); /* only reached if execve failed */
    }

    set_msg("Launched.", C_OK);
    field[0] = '\0';
}

/* -- input handling ------------------------------------------------- */

static void handle_mouse(const wm_mouse_event_t *m) {
    if (m->type == WM_MOUSE_LEAVE) {
        if (hover_btn != -1 || active_btn != -1) dirty = 1;
        hover_btn = -1; active_btn = -1;
        return;
    }

    int hb = hit_button(m->x, m->y);
    if (hb != hover_btn) { hover_btn = hb; dirty = 1; }

    if (m->type == WM_MOUSE_DOWN) {
        if (m->changed & WM_MBTN_LEFT) { active_btn = hb; dirty = 1; }
        return;
    }

    if (m->type == WM_MOUSE_UP) {
        if (m->changed & WM_MBTN_LEFT) {
            int was = active_btn;
            active_btn = -1;
            if (was >= 0 && was == hb) {
                if (was == BTN_RUN) do_run();
                else if (was == BTN_CANCEL) quit = 1;
            }
            dirty = 1;
        }
    }
}

static void handle_key(const KeyboardEventData *k) {
    char c = (char)k->ascii;

    if (c == 27) { quit = 1; return; }

    if (c == 8 || c == 127) {
        int n = sl(field);
        if (n > 0) { field[n-1] = '\0'; dirty = 1; }
        return;
    }

    if (c == '\n' || c == '\r') { do_run(); return; }

    if (c >= 32 && c < 127) {
        int n = sl(field);
        if (n < (int)sizeof(field) - 1) { field[n] = c; field[n+1] = '\0'; dirty = 1; }
    }
}

/* -- entry point ----------------------------------------------------- */

int md_main(long argc, char **argv) {
    (void)argc; (void)argv;
    field[0] = '\0';
    msg[0] = '\0';

    wm_shm_t *mb = wm_ipc_connect(3000);
    if (!mb) { puts("mdrun: mdw never came up (waited 3s)\n"); return 1; }

    wm_request_t req; memset(&req, 0, sizeof(req));
    req.cmd   = WM_CMD_CREATE;
    req.w     = WIN_W;
    req.h     = WIN_H;
    req.flags = 0;
    req.pid   = (uint32_t)getpid();
    { const char *t = "Run"; int i=0; while (t[i] && i<63) { req.title[i]=t[i]; i++; } req.title[i]=0; }

    wm_response_t resp;
    if (wm_ipc_call(mb, &req, &resp, 3000) != 0 || !resp.success) {
        printf("mdrun: create failed: %s\n", resp.message);
        return 1;
    }

    int shmfd = shm_open(resp.shm_name, O_RDWR, 0, 0);
    if (shmfd < 0) { puts("mdrun: shm_open failed\n"); return 1; }
    g_buf = (uint32_t *)mmap(NULL, (size_t)resp.shm_size, PROT_R|PROT_W, MAP_SHARED, shmfd);
    if (g_buf == MAP_FAILED) { puts("mdrun: mmap failed\n"); return 1; }

    wm_key_ring_t *keys = NULL;
    int keyfd = shm_open(resp.key_shm_name, O_RDWR, 0, 0);
    if (keyfd >= 0) {
        void *p = mmap(NULL, (size_t)resp.key_shm_size, PROT_R|PROT_W, MAP_SHARED, keyfd);
        if (p != MAP_FAILED) keys = (wm_key_ring_t *)p;
    }
    wm_mouse_ring_t *mice = NULL;
    int mousefd = shm_open(resp.mouse_shm_name, O_RDWR, 0, 0);
    if (mousefd >= 0) {
        void *p = mmap(NULL, (size_t)resp.mouse_shm_size, PROT_R|PROT_W, MAP_SHARED, mousefd);
        if (p != MAP_FAILED) mice = (wm_mouse_ring_t *)p;
    }
    if (!mice) { puts("mdrun: no mouse ring (mdw too old?)\n"); return 1; }
    if (!keys)   puts("mdrun: no key ring — typing disabled\n");

    g_font = try_load_font(FONT_PRIMARY);
    if (!g_font) g_font = try_load_font(FONT_FALLBACK);

    layout_buttons();
    wm_set_cursor(mb, resp.win_id, WM_CURSOR_TEXT);

    wm_client_surface_t surf;
    wm_client_surface_init(&surf, g_buf, resp.shm_name, resp.shm_size, WIN_W, WIN_H);

    render();
    wm_damage(mb, resp.win_id, 0, 0, WIN_W, WIN_H);

    uint64_t next_poll = time_ms();
    uint64_t next_resize_poll = time_ms();
    while (!quit) {
        wm_mouse_event_t m;
        while (wm_mouse_ring_pop(mice, &m)) handle_mouse(&m);

        if (keys) {
            KeyboardEventData k;
            while (wm_key_ring_pop(keys, &k)) handle_key(&k);
        }

        /* mdw draws this window's titlebar itself, so a click on ITS OWN
         * maximize/restore button never round-trips through our own
         * wm_ipc_call -- polling wm_list_windows() is the only way we
         * find out the content buffer got reallocated. Throttled a bit
         * looser than the input rings since it's not latency-sensitive. */
        if (time_ms() >= next_resize_poll) {
            int rc = wm_client_surface_poll(mb, resp.win_id, &surf);
            if (rc < 0) break; /* window torn down some other way */
            if (rc > 0) {
                g_buf = surf.buf;
                g_stride = surf.w;
                /* Fixed 420x110 dialog: letterbox rather than reflow --
                 * clear the whole (possibly now much bigger) buffer, then
                 * redraw the normal content anchored top-left. */
                wm_client_surface_clear(&surf, C_BG);
                dirty = 1;
            }
            next_resize_poll = time_ms() + 200;
        }

        if (surf.minimized) {
            /* Nothing on screen to update -- skip render/damage entirely
             * rather than burning cycles redrawing into a hidden buffer. */
            next_poll = time_ms();
            yield();
            continue;
        }

        if (dirty) {
            render();
            wm_damage(mb, resp.win_id, 0, 0, WIN_W, WIN_H);
            dirty = 0;
        }

        next_poll += 8;
        while (time_ms() < next_poll) yield();
        if (time_ms() > next_poll + 200) next_poll = time_ms();
    }

    wm_request_t close_req; memset(&close_req, 0, sizeof(close_req));
    close_req.cmd = WM_CMD_CLOSE;
    close_req.win_id = resp.win_id;
    wm_response_t cresp;
    wm_ipc_call(mb, &close_req, &cresp, 3000);

    if (surf.buf) munmap(surf.buf, (size_t)surf.shm_size); /* NOT resp.shm_size -- may be stale after a resize */
    if (g_font) fnt_free_font(g_font);
    return 0;
}