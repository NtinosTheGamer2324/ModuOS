// demo_notes.c
// A second $/user/wm client. Draws a static note list once, then updates
// just a one-line "uptime" counter every second — and only sends damage
// for that one line's rect, not the whole window. This is the payoff of
// clients reporting precise sub-rects: the WM never has to guess.
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

static void fill_rect(uint32_t *buf, int bw, int bh, int x, int y, int w, int h, uint32_t col) {
    int x1 = x + w, y1 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > bw) x1 = bw;
    if (y1 > bh) y1 = bh;
    for (int yy = y; yy < y1; yy++)
        for (int xx = x; xx < x1; xx++)
            buf[(size_t)yy * bw + xx] = col;
}

int md_main(long argc, char **argv) {
    (void)argc; (void)argv;

    int fd = wm_connect(3000); /* mdw may still be starting up — poll, don't fail on the first race */
    if (fd < 0) {
        puts("notes: mdw never came up (waited 3s)\n");
        return 1;
    }

    const int W = 300, H = 180;
    wm_request_t req; memset(&req, 0, sizeof(req));
    req.cmd = WM_CMD_CREATE;
    req.w = (uint32_t)W;
    req.h = (uint32_t)H;
    req.flags = 0;
    req.pid = (uint32_t)getpid(); /* lets the WM reap this window if we crash */
    { const char *t = "Notes"; int i = 0; while (t[i] && i < 63) { req.title[i] = t[i]; i++; } req.title[i] = 0; }

    wm_response_t resp;
    if (invoke(fd, &req, sizeof(req), &resp, sizeof(resp)) <= 0 || !resp.success) {
        printf("notes: create failed: %s\n", resp.message);
        close(fd);
        return 1;
    }

    int shmfd = shm_open(resp.shm_name, O_RDWR, 0, 0);
    if (shmfd < 0) {
        puts("notes: shm_open failed\n");
        close(fd);
        return 1;
    }
    uint32_t *buf = (uint32_t *)mmap(NULL, (size_t)resp.shm_size, PROT_R | PROT_W, MAP_SHARED, shmfd);
    if (buf == MAP_FAILED) {
        puts("notes: mmap failed\n");
        close(fd);
        return 1;
    }

    fnt_font_t *font = try_load_font(FONT_PRIMARY);
    if (!font) font = try_load_font(FONT_FALLBACK);

    fill_rect(buf, W, H, 0, 0, W, H, 0xFFF4F2E8u); /* paper */

    const char *lines[] = {
        "TODO:",
        " - Port real apps onto shm windows",
        " - Add resize + minimize",
        " - Maybe a start menu someday",
    };
    int ty = 12;
    for (int i = 0; i < 4; i++) {
        put_text(buf, W, H, 12, ty, lines[i], font, 32, 36, 46);
        ty += 18;
    }

    /* Reserve a dedicated line for the live counter, below the static list. */
    const int uptime_y = ty + 8;
    const int uptime_h = 16;

    wm_request_t full_dmg; memset(&full_dmg, 0, sizeof(full_dmg));
    full_dmg.cmd = WM_CMD_DAMAGE; full_dmg.win_id = resp.win_id;
    full_dmg.x = 0; full_dmg.y = 0; full_dmg.w = (uint32_t)W; full_dmg.h = (uint32_t)H;
    wm_response_t dresp;
    invoke(fd, &full_dmg, sizeof(full_dmg), &dresp, sizeof(dresp));

    unsigned int secs = 0;
    uint64_t next_tick = time_ms() + 1000;
    for (;;) {
        /* sleep() is unreliable right now — pace with time_ms()+yield() instead. */
        while (time_ms() < next_tick) yield();
        next_tick += 1000;

        char line[48];
        sprintf(line, "Uptime: %us", secs);

        /* Only touch this line's pixels, not the whole window. */
        fill_rect(buf, W, H, 12, uptime_y, W - 24, uptime_h, 0xFFF4F2E8u);
        put_text(buf, W, H, 12, uptime_y, line, font, 47, 91, 47);

        wm_request_t dmg; memset(&dmg, 0, sizeof(dmg));
        dmg.cmd = WM_CMD_DAMAGE; dmg.win_id = resp.win_id;
        dmg.x = 12; dmg.y = uptime_y; dmg.w = (uint32_t)(W - 24); dmg.h = (uint32_t)uptime_h;
        wm_response_t r2;
        invoke(fd, &dmg, sizeof(dmg), &r2, sizeof(r2));

        secs++;
    }

    return 0;
}