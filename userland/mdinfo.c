/* mdinfo.c — System Information
 *
 * Reads $/dev/md64api/sysinfo once at startup (static facts: OS/kernel/CPU/
 * board/firmware/VM) and re-reads it periodically thereafter just to keep
 * the uptime line live. Laid out as a stack of labeled panels rather than
 * a single wrapped paragraph, so it's actually skimmable.
 *
 * Genuinely reflows on resize (not letterboxed): panel width tracks the
 * window's current content width, so MAXIMIZE/RESTORE/FULLSCREEN widen or
 * narrow the panels instead of leaving dead space or clipping content.
 */

#include "libc.h"
#include "lib_fnt.h"
#include "wm_protocol.h"
#include "wm_client_resize.h"

#define FONT_PRIMARY  "/ModuOS/shared/assets/fonts/Terminus.fnt"
#define FONT_FALLBACK "/ModuOS/shared/assets/fonts/Unicode.fnt"

#define INIT_W 640
#define INIT_H 640

#define PAD          16   /* outer window margin                      */
#define PANEL_PAD    12   /* inner panel padding                      */
#define PANEL_GAP    12   /* vertical gap between panels               */
#define ROW_H        20   /* label:value row height                   */
#define LABEL_COL_W  180  /* width reserved for the label column       */
#define HEADER_H     46   /* top title bar height                     */
#define PANEL_TITLE_H 26  /* chip + section-title row height inside a panel */

#define C_BG          0xFF14171Du
#define C_HEADER_BG   0xFF1B2130u
#define C_PANEL_BG    0xFF1B1F27u
#define C_PANEL_EDGE  0xFF2E3644u
#define C_TEXT        0xFFDCE2EEu
#define C_TEXT_DIM    0xFF7A8194u
#define C_ACCENT      0xFF6FB2FFu
#define C_OK          0xFF5FD37Fu
#define C_WARN        0xFFE0B84Fu
#define C_TITLE       0xFFF2F5FAu

typedef struct { char label[48]; char value[192]; } row_t;
typedef struct { char title[48]; uint32_t chip; row_t rows[6]; int nrows; } panel_t;

static fnt_font_t *g_font;
static uint32_t    *g_buf;
static int           g_W = INIT_W, g_H = INIT_H;

static md64api_sysinfo_data_u *g_info;
static int g_sysinfo_fd = -1;

static char g_uptime_str[64];

static fnt_font_t *try_load_font(const char *path) {
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return NULL;
    long fsz = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    fnt_font_t *out = NULL;
    if (fsz > 0 && fsz < 4 * 1024 * 1024) {
        void *data = malloc((size_t)fsz);
        if (data) {
            size_t got = 0;
            while (got < (size_t)fsz) {
                ssize_t r = read(fd, (uint8_t *)data + got, (size_t)fsz - got);
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

/* -- drawing ----------------------------------------------------------- */

static void fill_rect(int x, int y, int w, int h, uint32_t col) {
    int x1 = x + w, y1 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > g_W) x1 = g_W;
    if (y1 > g_H) y1 = g_H;
    for (int yy = y; yy < y1; yy++)
        for (int xx = x; xx < x1; xx++)
            g_buf[(size_t)yy * g_W + xx] = col;
}

static void frame_rect(int x, int y, int w, int h, uint32_t col) {
    fill_rect(x, y, w, 1, col);
    fill_rect(x, y + h - 1, w, 1, col);
    fill_rect(x, y, 1, h, col);
    fill_rect(x + w - 1, y, 1, h, col);
}

static int text_w(const char *s) {
    if (!g_font || !s) return 0;
    int w = 0;
    while (*s) { fnt_glyph_t *gl = fnt_get_glyph(g_font, (uint32_t)(unsigned char)*s); if (gl) w += gl->width; s++; }
    return w;
}

static void draw_text_clip(int x, int y, const char *s, uint32_t col, int clip_x1) {
    if (!g_font || !s) return;
    int cx = x;
    while (*s) {
        fnt_glyph_t *gl = fnt_get_glyph(g_font, (uint32_t)(unsigned char)*s);
        if (gl) {
            if (cx + gl->width > clip_x1) break;
            for (int dy = 0; dy < gl->bitmap_height; dy++)
                for (int dx = 0; dx < gl->bitmap_width; dx++)
                    if (fnt_get_pixel(gl, dx, dy)) {
                        int px = cx + dx, py = y + dy;
                        if (px >= 0 && px < g_W && py >= 0 && py < g_H)
                            g_buf[(size_t)py * g_W + px] = col;
                    }
            cx += gl->width;
        }
        s++;
    }
}

static void draw_text(int x, int y, const char *s, uint32_t col) {
    draw_text_clip(x, y, s, col, g_W - PAD);
}

/* -- sysinfo -------------------------------------------------------------
 * Re-opens the device each poll rather than keeping the fd parked open --
 * this is a low-frequency poll (uptime only needs ~1s resolution), and
 * matches how every other client in this codebase treats one-shot device
 * reads. Returns 1 if the formatted uptime string changed. */
static void format_uptime(uint64_t secs, char *out, size_t cap) {
    uint64_t d = secs / 86400;
    uint64_t h = (secs % 86400) / 3600;
    uint64_t m = (secs % 3600) / 60;
    uint64_t s = secs % 60;
    if (d > 0)      snprintf(out, cap, "%ud %uh %um %us", (unsigned)d, (unsigned)h, (unsigned)m, (unsigned)s);
    else if (h > 0) snprintf(out, cap, "%uh %um %us", (unsigned)h, (unsigned)m, (unsigned)s);
    else if (m > 0) snprintf(out, cap, "%um %us", (unsigned)m, (unsigned)s);
    else            snprintf(out, cap, "%us", (unsigned)s);
}

static int refresh_sysinfo(void) {
    int fd = open("$/dev/md64api/sysinfo", 0, 0);
    if (fd < 0) return 0;
    read(fd, g_info, sizeof(md64api_sysinfo_data_u));
    close(fd);

    char fresh[64];
    format_uptime(g_info->uptime_seconds, fresh, sizeof(fresh));
    int changed = 0;
    for (int i = 0; ; i++) {
        if (fresh[i] != g_uptime_str[i]) { changed = 1; break; }
        if (!fresh[i]) break;
    }
    if (changed) { int i = 0; while (fresh[i] && i + 1 < (int)sizeof(g_uptime_str)) { g_uptime_str[i] = fresh[i]; i++; } g_uptime_str[i] = 0; }
    return changed;
}

/* -- panels --------------------------------------------------------------
 * One vertically-stacked column, full window width -- this reflows
 * cleanly whether the window gets wider (panels stretch, more room for
 * long values) or taller (more room before anything would need to
 * scroll; this app doesn't support scrolling, so an extremely short
 * window will just clip the last panel against the bottom edge). */
static void draw_panel(int x, int y, int w, const panel_t *p, int *out_h) {
    int h = PANEL_TITLE_H + PANEL_PAD * 2 + p->nrows * ROW_H;

    fill_rect(x, y, w, h, C_PANEL_BG);
    frame_rect(x, y, w, h, C_PANEL_EDGE);

    /* chip + section title */
    fill_rect(x + PANEL_PAD, y + PANEL_PAD + 3, 4, 12, p->chip);
    draw_text(x + PANEL_PAD + 10, y + PANEL_PAD, p->title, C_ACCENT);

    int ry = y + PANEL_PAD + 22;
    for (int i = 0; i < p->nrows; i++) {
        draw_text_clip(x + PANEL_PAD, ry, p->rows[i].label, C_TEXT_DIM, x + LABEL_COL_W);
        draw_text_clip(x + PANEL_PAD + LABEL_COL_W, ry, p->rows[i].value, C_TEXT, x + w - PANEL_PAD);
        ry += ROW_H;
    }

    *out_h = h;
}

static void row_set(row_t *r, const char *label, const char *value) {
    int i = 0; while (label[i] && i + 1 < (int)sizeof(r->label)) { r->label[i] = label[i]; i++; } r->label[i] = 0;
    i = 0; while (value[i] && i + 1 < (int)sizeof(r->value)) { r->value[i] = value[i]; i++; } r->value[i] = 0;
}

static void render(void) {
    fill_rect(0, 0, g_W, g_H, C_BG);

    /* header bar */
    fill_rect(0, 0, g_W, HEADER_H, C_HEADER_BG);
    fill_rect(0, HEADER_H - 1, g_W, 1, C_PANEL_EDGE);
    draw_text(PAD, 14, "System Information", C_TITLE);
    {
        char sub[192];
        snprintf(sub, sizeof(sub), "%s %s", g_info->os_name, g_info->os_arch);
        draw_text_clip(PAD, HEADER_H - 16, sub, C_TEXT_DIM, g_W - PAD);
    }

    int panel_w = g_W - PAD * 2;
    int y = HEADER_H + PANEL_GAP;

    /* -- Operating System -- */
    {
        panel_t p; memset(&p, 0, sizeof(p));
        int i = 0;
        int j = 0; while ("Operating System"[j] && j+1 < (int)sizeof(p.title)) { p.title[j] = "Operating System"[j]; j++; } p.title[j]=0;
        p.chip = C_ACCENT;
        char ver[32]; snprintf(ver, sizeof(ver), "%u", (unsigned)g_info->SystemVersion);
        row_set(&p.rows[i++], "OS Name", g_info->os_name);
        row_set(&p.rows[i++], "Version", ver);
        row_set(&p.rows[i++], "Architecture", g_info->os_arch);
        char kver[32]; snprintf(kver, sizeof(kver), "%u", (unsigned)g_info->KernelVersion);
        row_set(&p.rows[i++], "Kernel Version", kver);
        row_set(&p.rows[i++], "Kernel Vendor", g_info->KernelVendor);
        p.nrows = i;
        int h; draw_panel(PAD, y, panel_w, &p, &h);
        y += h + PANEL_GAP;
    }

    /* -- Hardware -- */
    {
        panel_t p; memset(&p, 0, sizeof(p));
        int i = 0;
        int j = 0; while ("Hardware"[j] && j+1 < (int)sizeof(p.title)) { p.title[j] = "Hardware"[j]; j++; } p.title[j]=0;
        p.chip = C_OK;
        row_set(&p.rows[i++], "CPU Model", g_info->cpu_model);
        row_set(&p.rows[i++], "CPU Vendor", g_info->cpu_manufacturer);
        row_set(&p.rows[i++], "Motherboard", g_info->motherboard_model);
        p.nrows = i;
        int h; draw_panel(PAD, y, panel_w, &p, &h);
        y += h + PANEL_GAP;
    }

    /* -- Firmware -- */
    {
        panel_t p; memset(&p, 0, sizeof(p));
        int i = 0;
        int j = 0; while ("Firmware"[j] && j+1 < (int)sizeof(p.title)) { p.title[j] = "Firmware"[j]; j++; } p.title[j]=0;
        p.chip = C_WARN;
        row_set(&p.rows[i++], "Version", g_info->bios_version);
        row_set(&p.rows[i++], "Vendor", g_info->bios_vendor);
        p.nrows = i;
        int h; draw_panel(PAD, y, panel_w, &p, &h);
        y += h + PANEL_GAP;
    }

    /* -- Environment -- */
    {
        panel_t p; memset(&p, 0, sizeof(p));
        int i = 0;
        int j = 0; while ("Environment"[j] && j+1 < (int)sizeof(p.title)) { p.title[j] = "Environment"[j]; j++; } p.title[j]=0;
        p.chip = C_ACCENT;
        row_set(&p.rows[i++], "Virtual Machine", g_info->is_virtual_machine ? "Yes" : "No");
        if (g_info->is_virtual_machine) row_set(&p.rows[i++], "VM Vendor", g_info->virtualization_vendor);
        row_set(&p.rows[i++], "Uptime", g_uptime_str);
        p.nrows = i;
        int h; draw_panel(PAD, y, panel_w, &p, &h);
        y += h + PANEL_GAP;
    }
}

int md_main(long argc, char **argv) {
    (void)argc; (void)argv;

    g_info = (md64api_sysinfo_data_u *)malloc(sizeof(md64api_sysinfo_data_u));
    if (!g_info) { printf("mdinfo: out of memory!\n"); return 1; }
    memset(g_info, 0, sizeof(md64api_sysinfo_data_u));
    {
        int fd = open("$/dev/md64api/sysinfo", 0, 0);
        if (fd < 0) { free(g_info); printf("mdinfo: cannot read system info\n"); return 1; }
        read(fd, g_info, sizeof(md64api_sysinfo_data_u));
        close(fd);
    }
    format_uptime(g_info->uptime_seconds, g_uptime_str, sizeof(g_uptime_str));

    wm_shm_t *mb = wm_ipc_connect(3000); /* mdw may still be starting up — poll, don't fail on the first race */
    if (!mb) {
        printf("mdinfo: mdw never came up (waited 3s)\n");
        free(g_info);
        return 1;
    }

    wm_request_t req; memset(&req, 0, sizeof(req));
    req.cmd = WM_CMD_CREATE;
    req.w = (uint32_t)INIT_W;
    req.h = (uint32_t)INIT_H;
    req.flags = 0;
    req.pid = (uint32_t)getpid();
    { const char *t = "System Information"; int i = 0; while (t[i] && i < 63) { req.title[i] = t[i]; i++; } req.title[i] = 0; }

    wm_response_t resp;
    if (wm_ipc_call(mb, &req, &resp, 3000) != 0 || !resp.success) {
        printf("mdinfo: create failed: %s\n", resp.message);
        free(g_info);
        return 1;
    }

    int shmfd = shm_open(resp.shm_name, O_RDWR, 0, 0);
    if (shmfd < 0) { puts("mdinfo: shm_open failed\n"); free(g_info); return 1; }
    g_buf = (uint32_t *)mmap(NULL, (size_t)resp.shm_size, PROT_R | PROT_W, MAP_SHARED, shmfd);
    if (g_buf == MAP_FAILED) { puts("mdinfo: mmap failed\n"); free(g_info); return 1; }

    wm_mouse_ring_t *mice = NULL;
    int mousefd = shm_open(resp.mouse_shm_name, O_RDWR, 0, 0);
    if (mousefd >= 0) {
        void *p = mmap(NULL, (size_t)resp.mouse_shm_size, PROT_R | PROT_W, MAP_SHARED, mousefd);
        if (p != MAP_FAILED) mice = (wm_mouse_ring_t *)p;
    }

    g_font = try_load_font(FONT_PRIMARY);
    if (!g_font) g_font = try_load_font(FONT_FALLBACK);
    if (!g_font) {
        printf("mdinfo: failed to get a font\n");
        free(g_info);
        return -1;
    }

    g_W = INIT_W; g_H = INIT_H;

    wm_client_surface_t surf;
    wm_client_surface_init(&surf, g_buf, resp.shm_name, resp.shm_size, g_W, g_H);

    render();
    wm_damage(mb, resp.win_id, 0, 0, g_W, g_H);

    uint64_t next_resize_poll = time_ms();
    uint64_t next_sysinfo_poll = time_ms();
    uint64_t next_tick = time_ms();
    int quit = 0;

    while (!quit) {
        if (mice) {
            wm_mouse_event_t m;
            while (wm_mouse_ring_pop(mice, &m)) { /* nothing clickable yet -- purely informational */ }
        }

        /* mdw draws this window's titlebar itself, so its own maximize/
         * restore/close button never round-trips through our own
         * wm_ipc_call -- polling wm_list_windows() is the only way we
         * find out the content buffer got reallocated (or that we've
         * been closed some other way, e.g. from a taskbar). */
        if (time_ms() >= next_resize_poll) {
            int rc = wm_client_surface_poll(mb, resp.win_id, &surf);
            if (rc < 0) { quit = 1; break; }
            if (rc > 0) {
                g_buf = surf.buf;
                g_W = surf.w;
                g_H = surf.h;
                render();
                wm_damage(mb, resp.win_id, 0, 0, g_W, g_H);
            }
            next_resize_poll = time_ms() + 200;
        }

        if (surf.minimized) {
            next_tick = time_ms();
            yield();
            continue;
        }

        if (time_ms() >= next_sysinfo_poll) {
            if (refresh_sysinfo()) {
                render();
                wm_damage(mb, resp.win_id, 0, 0, g_W, g_H);
            }
            next_sysinfo_poll = time_ms() + 1000;
        }

        next_tick += 16;
        while (time_ms() < next_tick) yield();
        if (time_ms() > next_tick + 200) next_tick = time_ms();
    }

    wm_request_t close_req; memset(&close_req, 0, sizeof(close_req));
    close_req.cmd = WM_CMD_CLOSE;
    close_req.win_id = resp.win_id;
    wm_response_t cresp;
    wm_ipc_call(mb, &close_req, &cresp, 3000);

    if (surf.buf) munmap(surf.buf, (size_t)surf.shm_size); /* NOT resp.shm_size -- may be stale after a resize */
    if (g_font) fnt_free_font(g_font);
    free(g_info);
    return 0;
}