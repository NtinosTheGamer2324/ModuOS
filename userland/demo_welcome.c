// demo_welcome.c
// A tiny client of $/user/wm — creates a window, draws a static welcome
// panel into its shared-memory buffer, tells the WM about it, then just
// keeps the mapping alive. Mirrors calc_client.c's structure.
#include "libc.h"
#include "lib_fnt.h"
#include "wm_protocol.h"

#define FONT_PRIMARY  "/ModuOS/shared/assets/fonts/Terminus.fnt"
#define FONT_FALLBACK "/ModuOS/shared/assets/fonts/Unicode.fnt"

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

static int text_width(fnt_font_t *font, const char *s) {
    if (!font || !s) return 0;
    return fnt_string_width(font, s);
}

static void put_text(uint32_t *buf, int bw, int bh, int x, int y, const char *s,
                      fnt_font_t *font, uint8_t r, uint8_t g, uint8_t b) {
    if (!font || !s) return;
    int cx = x;
    while (*s) {
        fnt_glyph_t *gl = fnt_get_glyph(font, (uint32_t)(unsigned char)*s);
        if (gl) {
            for (int dy = 0; dy < gl->bitmap_height; dy++)
                for (int dx = 0; dx < gl->bitmap_width; dx++)
                    if (fnt_get_pixel(gl, dx, dy) && cx + dx >= 0 && cx + dx < bw && y + dy >= 0 && y + dy < bh)
                        buf[(size_t)(y + dy) * bw + (cx + dx)] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
            cx += gl->width;
        }
        s++;
    }
}

int md_main(long argc, char **argv) {
    (void)argc; (void)argv;

    int fd = wm_connect(3000); /* mdw may still be starting up — poll, don't fail on the first race */
    if (fd < 0) {
        puts("welcome: mdw never came up (waited 3s)\n");
        return 1;
    }

    const int W = 360, H = 200;
    wm_request_t req; memset(&req, 0, sizeof(req));
    req.cmd = WM_CMD_CREATE;
    req.w = (uint32_t)W;
    req.h = (uint32_t)H;
    req.flags = 0; /* fully opaque content -> WM takes the fast blit path */
    req.pid = (uint32_t)getpid(); /* lets the WM reap this window if we crash */
    { const char *t = "Welcome"; int i = 0; while (t[i] && i < 63) { req.title[i] = t[i]; i++; } req.title[i] = 0; }

    wm_response_t resp;
    if (invoke(fd, &req, sizeof(req), &resp, sizeof(resp)) <= 0 || !resp.success) {
        printf("welcome: create failed: %s\n", resp.message);
        close(fd);
        return 1;
    }

    int shmfd = shm_open(resp.shm_name, O_RDWR, 0, 0);
    if (shmfd < 0) {
        puts("welcome: shm_open failed\n");
        close(fd);
        return 1;
    }
    uint32_t *buf = (uint32_t *)mmap(NULL, (size_t)resp.shm_size, PROT_R | PROT_W, MAP_SHARED, shmfd);
    if (buf == MAP_FAILED) {
        puts("welcome: mmap failed\n");
        close(fd);
        return 1;
    }

    fnt_font_t *font = try_load_font(FONT_PRIMARY);
    if (!font) font = try_load_font(FONT_FALLBACK);

    /* Paint a gentle vertical gradient background. */
    for (int y = 0; y < H; y++) {
        uint8_t shade = (uint8_t)(30 + (y * 40) / H);
        for (int x = 0; x < W; x++)
            buf[(size_t)y * W + x] = 0xFF000000u | ((uint32_t)shade << 16) | ((uint32_t)(shade + 10) << 8) | (shade + 24);
    }

    const char *lines[] = { "NodGL Window Manager", "Software compositor, running on ModuOS", "Drag this title bar to move me." };
    int ty = 24;
    for (int i = 0; i < 3; i++) {
        int tx = W / 2 - text_width(font, lines[i]) / 2;
        put_text(buf, W, H, tx, ty, lines[i], font, 235, 240, 255);
        ty += 20;
    }

    /* small swatch grid, just to prove content isn't just text */
    uint32_t swatches[4] = { 0xFF2F6FE0, 0xFFE0483F, 0xFF3FBF6F, 0xFFE0B23F };
    for (int i = 0; i < 4; i++) {
        int sx = 16 + i * 40, sy = H - 40;
        for (int yy = sy; yy < sy + 24 && yy < H; yy++)
            for (int xx = sx; xx < sx + 32 && xx < W; xx++)
                buf[(size_t)yy * W + xx] = swatches[i];
    }

    wm_request_t dmg; memset(&dmg, 0, sizeof(dmg));
    dmg.cmd = WM_CMD_DAMAGE;
    dmg.win_id = resp.win_id;
    dmg.x = 0; dmg.y = 0; dmg.w = (uint32_t)W; dmg.h = (uint32_t)H;
    wm_response_t dresp;
    invoke(fd, &dmg, sizeof(dmg), &dresp, sizeof(dresp));

    if (font) fnt_free_font(font);

    /* Content is static — nothing left to draw. Stay alive so the mapping
     * (and the window) keeps existing; a real app would do its own work
     * here instead. sleep() is currently unreliable, so just yield —
     * there's no real work to wait on here anyway. */
    for (;;) yield();

    return 0;
}