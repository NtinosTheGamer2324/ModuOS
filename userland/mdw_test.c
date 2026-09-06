// mdw_test.c
// Smallest possible mdw client: connects (polling, no sleep()), creates a
// small window, paints a solid color + status text, damages it once, done.
// Meant as a quick smoke test — if this window shows up, the connect race
// with mdw's startup is actually fixed, not just theoretically fixed.
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

int md_main(long argc, char **argv) {
    (void)argc; (void)argv;

    uint64_t t0 = time_ms();
    int fd = wm_connect(3000);
    uint64_t connect_ms = time_ms() - t0;

    if (fd < 0) {
        printf("mdw_test: mdw never came up after %ums — giving up\n", (unsigned)connect_ms);
        return 1;
    }
    printf("mdw_test: connected after %ums\n", (unsigned)connect_ms);

    const int W = 220, H = 100;
    wm_request_t req; memset(&req, 0, sizeof(req));
    req.cmd = WM_CMD_CREATE;
    req.w = (uint32_t)W;
    req.h = (uint32_t)H;
    req.flags = 0;
    req.pid = (uint32_t)getpid();
    { const char *t = "MDW Test"; int i = 0; while (t[i] && i < 63) { req.title[i] = t[i]; i++; } req.title[i] = 0; }

    wm_response_t resp;
    if (invoke(fd, &req, sizeof(req), &resp, sizeof(resp)) <= 0 || !resp.success) {
        printf("mdw_test: create failed: %s\n", resp.message);
        close(fd);
        return 1;
    }

    int shmfd = shm_open(resp.shm_name, O_RDWR, 0, 0);
    if (shmfd < 0) {
        puts("mdw_test: shm_open failed\n");
        close(fd);
        return 1;
    }
    uint32_t *buf = (uint32_t *)mmap(NULL, (size_t)resp.shm_size, PROT_R | PROT_W, MAP_SHARED, shmfd);
    if (buf == MAP_FAILED) {
        puts("mdw_test: mmap failed\n");
        close(fd);
        return 1;
    }

    for (int i = 0; i < W * H; i++) buf[i] = 0xFF2FBF6Fu; /* solid green = "it worked" */

    fnt_font_t *font = try_load_font(FONT_PRIMARY);
    if (!font) font = try_load_font(FONT_FALLBACK);
    put_text(buf, W, H, 12, 12, "mdw_test", font, 20, 24, 20);
    char line[48];
    sprintf(line, "connected in %ums", (unsigned)connect_ms);
    put_text(buf, W, H, 12, 32, line, font, 20, 24, 20);
    if (font) fnt_free_font(font);

    wm_request_t dmg; memset(&dmg, 0, sizeof(dmg));
    dmg.cmd = WM_CMD_DAMAGE; dmg.win_id = resp.win_id;
    dmg.x = 0; dmg.y = 0; dmg.w = (uint32_t)W; dmg.h = (uint32_t)H;
    wm_response_t dresp;
    invoke(fd, &dmg, sizeof(dmg), &dresp, sizeof(dresp));

    for (;;) yield(); /* stay alive so the window (and mapping) sticks around */

    return 0;
}