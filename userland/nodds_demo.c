// nodds_demo.c — NodDS demo application
//
// Demonstrates the NodDS client library:
//   - Connects to the display server
//   - Creates a single window
//   - Renders static colored rectangles + text via lib_fnt
//   - Handles NODDS_EVT_CLOSE to exit cleanly

#include "libc.h"
#include "nodds_client.h"
#include "lib_fnt.h"

// ============================================================
// Demo window size
// ============================================================

#define WIN_W 480
#define WIN_H 320
#define WIN_X 100
#define WIN_Y 80

// ============================================================
// Color palette
// ============================================================

#define COL_BG          0xFF1A1A2E
#define COL_PANEL       0xFF16213E
#define COL_ACCENT      0xFF0F3460
#define COL_HIGHLIGHT   0xFFE94560
#define COL_WHITE       0xFFFFFFFF
#define COL_GREY        0xFF888888
#define COL_GREEN       0xFF4CAF50
#define COL_BLUE        0xFF2196F3
#define COL_YELLOW      0xFFFFEB3B

// ============================================================
// Software pixel buffer helpers
// ============================================================

static uint32_t g_pixels[WIN_W * WIN_H];

static inline void pb_fill_rect(int x, int y, int w, int h, uint32_t col)
{
    for (int ry = y; ry < y + h; ry++) {
        if (ry < 0 || ry >= WIN_H) continue;
        for (int rx = x; rx < x + w; rx++) {
            if (rx < 0 || rx >= WIN_W) continue;
            g_pixels[ry * WIN_W + rx] = col;
        }
    }
}

// Draw a 1px border rectangle (outline only)
static void pb_draw_rect_outline(int x, int y, int w, int h, uint32_t col)
{
    pb_fill_rect(x,         y,         w, 1, col);  // top
    pb_fill_rect(x,         y + h - 1, w, 1, col);  // bottom
    pb_fill_rect(x,         y,         1, h, col);  // left
    pb_fill_rect(x + w - 1, y,         1, h, col);  // right
}

// ============================================================
// Text rendering via lib_fnt
// Renders each glyph as 1×1 filled pixels into g_pixels.
// scale=1 → native glyph size; scale=2 → 2× upscale.
// ============================================================

static void pb_draw_char(fnt_font_t *font, uint32_t cp,
                         int *cx, int cy, int scale, uint32_t col)
{
    fnt_glyph_t *g = fnt_get_glyph(font, cp);
    if (!g) {
        *cx += font->header.glyph_width * scale;
        return;
    }

    for (int gy = 0; gy < g->bitmap_height; gy++) {
        for (int gx = 0; gx < g->bitmap_width; gx++) {
            if (fnt_get_pixel(g, gx, gy)) {
                pb_fill_rect(*cx + gx * scale,
                             cy  + gy * scale,
                             scale, scale, col);
            }
        }
    }
    *cx += g->width * scale;
}

static void pb_draw_string(fnt_font_t *font, const char *text,
                           int x, int y, int scale, uint32_t col)
{
    int cx = x;
    for (const char *p = text; *p; p++) {
        pb_draw_char(font, (uint32_t)(unsigned char)*p, &cx, y, scale, col);
    }
}

// Center a string horizontally within [x, x+w)
static void pb_draw_string_centered(fnt_font_t *font, const char *text,
                                    int x, int y, int w, int scale, uint32_t col)
{
    int tw = fnt_string_width_scaled(font, text, scale);
    int cx = x + (w - tw) / 2;
    if (cx < x) cx = x;
    pb_draw_string(font, text, cx, y, scale, col);
}

// ============================================================
// Draw the demo scene into g_pixels
// ============================================================

static void render_scene(fnt_font_t *font)
{
    // Background
    pb_fill_rect(0, 0, WIN_W, WIN_H, COL_BG);

    // ---- Top banner ----
    pb_fill_rect(0, 0, WIN_W, 40, COL_PANEL);
    pb_draw_rect_outline(0, 0, WIN_W, 40, COL_ACCENT);
    if (font)
        pb_draw_string_centered(font, "NodDS Demo", 0, 10, WIN_W, 2, COL_WHITE);

    // ---- Colour swatches row ----
    int swatch_y  = 60;
    int swatch_h  = 50;
    int swatch_w  = 60;
    int swatch_gap = 16;
    uint32_t swatches[] = { COL_HIGHLIGHT, COL_GREEN, COL_BLUE, COL_YELLOW, COL_GREY };
    const char *swatch_labels[] = { "Red", "Green", "Blue", "Yellow", "Grey" };
    int n_swatches = (int)(sizeof(swatches) / sizeof(swatches[0]));
    int row_total  = n_swatches * swatch_w + (n_swatches - 1) * swatch_gap;
    int row_x      = (WIN_W - row_total) / 2;

    for (int i = 0; i < n_swatches; i++) {
        int sx = row_x + i * (swatch_w + swatch_gap);
        pb_fill_rect(sx, swatch_y, swatch_w, swatch_h, swatches[i]);
        pb_draw_rect_outline(sx, swatch_y, swatch_w, swatch_h, COL_WHITE);
        if (font) {
            int lw = fnt_string_width(font, swatch_labels[i]);
            int lx = sx + (swatch_w - lw) / 2;
            pb_draw_string(font, swatch_labels[i], lx,
                           swatch_y + swatch_h + 4, 1, COL_WHITE);
        }
    }

    // ---- Info panel ----
    int info_x = 20, info_y = 150, info_w = WIN_W - 40, info_h = 100;
    pb_fill_rect(info_x, info_y, info_w, info_h, COL_PANEL);
    pb_draw_rect_outline(info_x, info_y, info_w, info_h, COL_ACCENT);

    if (font) {
        pb_draw_string(font, "NodDS Display Server",
                       info_x + 12, info_y + 12, 1, COL_HIGHLIGHT);
        pb_draw_string(font, "Window Manager + Compositor",
                       info_x + 12, info_y + 28, 1, COL_GREY);
        pb_draw_string(font, "Client Library: nodds_client",
                       info_x + 12, info_y + 48, 1, COL_WHITE);
        pb_draw_string(font, "Font Rendering: lib_fnt",
                       info_x + 12, info_y + 64, 1, COL_WHITE);
    }

    // ---- Divider line ----
    pb_fill_rect(20, WIN_H - 40, WIN_W - 40, 1, COL_ACCENT);

    // ---- Footer ----
    if (font) {
        pb_draw_string_centered(font, "Close window to exit",
                                0, WIN_H - 28, WIN_W, 1, COL_GREY);
    }
}

// ============================================================
// Main
// ============================================================

int md_main(long argc, char **argv)
{
    (void)argc; (void)argv;

    printf("[demo] NodDS demo starting...\n");

    // Connect to display server (wait up to 3 seconds for nodds to start)
    nodds_client_t *client = nodds_connect(3000);
    if (!client) {
        printf("[demo] Failed to connect to NodDS\n");
        return 1;
    }

    // Query screen resolution (informational)
    uint32_t sw = 0, sh = 0;
    nodds_query_screen(client, &sw, &sh);
    printf("[demo] Screen: %ux%u\n", sw, sh);

    // Load font
    fnt_font_t *font = NULL;
    {
        // Read font file into memory
        int ffd = open("/ModuOS/shared/assets/fonts/Terminus.fnt", O_RDONLY, 0);
        if (ffd < 0) {
            printf("[demo] Warning: cannot open Terminus.fnt — text disabled\n");
        } else {
            // Seek to end to get size
            off_t fsz = lseek(ffd, 0, SEEK_END);
            lseek(ffd, 0, SEEK_SET);
            if (fsz > 0) {
                void *fdata = malloc((size_t)fsz);
                if (fdata) {
                    read(ffd, fdata, (size_t)fsz);
                    font = fnt_load_font(fdata, (size_t)fsz);
                    free(fdata);
                    if (!font)
                        printf("[demo] Warning: fnt_load_font failed\n");
                    else
                        printf("[demo] Font loaded\n");
                }
            }
            close(ffd);
        }
    }

    // Create window (center on screen if possible)
    int wx = (sw > WIN_W) ? (int)(sw - WIN_W) / 2 : WIN_X;
    int wy = (sh > WIN_H) ? (int)(sh - WIN_H) / 2 : WIN_Y;

    nodds_window_t win = {0};
    win.event_fd = -1;
    win.frame_fd = -1;

    if (nodds_create_window(client, wx, wy, WIN_W, WIN_H,
                            "NodDS Demo", &win) != 0) {
        printf("[demo] Failed to create window\n");
        if (font) fnt_free_font(font);
        nodds_disconnect(client);
        return 1;
    }

    printf("[demo] Window created (wid=%u)\n", win.wid);

    // Render initial frame
    render_scene(font);
    nodds_blit_frame(&win, g_pixels, WIN_W, WIN_H);

    // ---- Event loop ----
    int running = 1;
    while (running) {
        nodds_event_t evt;
        int r = nodds_poll_event(&win, &evt);

        if (r == 1) {
            switch (evt.type) {

            case NODDS_EVT_CLOSE:
                printf("[demo] CLOSE event received — exiting\n");
                running = 0;
                break;

            case NODDS_EVT_EXPOSE:
                // Compositor requested a redraw (e.g. window was uncovered)
                nodds_blit_frame(&win, g_pixels, WIN_W, WIN_H);
                break;

            case NODDS_EVT_RESIZE:
                // For this demo we ignore resize — the window is fixed-size.
                // A real app would reallocate its pixel buffer here.
                printf("[demo] Resize event: %ux%u (ignored by demo)\n",
                       evt.data.resize.width, evt.data.resize.height);
                break;

            case NODDS_EVT_FOCUS_IN:
                printf("[demo] Focus gained\n");
                break;

            case NODDS_EVT_FOCUS_OUT:
                printf("[demo] Focus lost\n");
                break;

            default:
                break;
            }
        } else if (r < 0) {
            printf("[demo] Event read error\n");
            running = 0;
        }

        // Yield to avoid spinning the CPU when the queue is empty
        yield();
    }

    // ---- Cleanup ----
    nodds_destroy_window(client, &win);
    if (font) fnt_free_font(font);
    nodds_disconnect(client);

    printf("[demo] Exiting cleanly\n");
    return 0;
}