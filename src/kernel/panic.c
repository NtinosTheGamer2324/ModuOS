#include "moduos/kernel/panic.h"
#include "moduos/drivers/graphics/VGA.h"
#include "moduos/kernel/shell/zenith4.h"
#include "moduos/drivers/power/ACPI.h"
#include "moduos/drivers/Time/RTC.h"

// Shared boilerplate
static void panic_header(const char* title)
{
    (void)title;
    panicer_close_shell4();

    /* Ensure the panic screen starts from a known visible state */
    if (VGA_GetFrameBufferMode() == FB_MODE_GRAPHICS) {
        VGA_ClearFrameBuffer(0x00000000);
    } else {
        VGA_Clear();
    }
}

/**
 * Generic panic function
 * @param title        The panic title
 * @param message      Detailed message explaining the problem
 * @param tips         Optional troubleshooting tips (can be NULL)
 * @param err_cat      Error category string (e.g., "DEV")
 * @param err_code     Specific error code string (e.g., "ATA_DEV_NONE")
 * @param reboot_delay Seconds to wait before reboot
 */
/* ---------------- Windows-11-style framebuffer panic UI ---------------- */

#include "moduos/drivers/graphics/framebuffer.h"
#include "moduos/drivers/graphics/bitmap_font.h"
#include "moduos/kernel/memory/string.h"

static uint32_t fb_pack_rgb(const framebuffer_t *fb, uint8_t r, uint8_t g, uint8_t b) {
    if (fb->bpp == 16) {
        uint16_t rr = (uint16_t)((r * 31u) / 255u);
        uint16_t gg = (uint16_t)((g * 63u) / 255u);
        uint16_t bb = (uint16_t)((b * 31u) / 255u);
        return (uint32_t)((rr << 11) | (gg << 5) | bb);
    }

    /* 32bpp RGB layout via multiboot positions */
    uint32_t rp = fb->red_pos;
    uint32_t gp = fb->green_pos;
    uint32_t bp = fb->blue_pos;

    uint32_t rm = (fb->red_mask_size >= 32) ? 0xFFFFFFFFu : ((1u << fb->red_mask_size) - 1u);
    uint32_t gm = (fb->green_mask_size >= 32) ? 0xFFFFFFFFu : ((1u << fb->green_mask_size) - 1u);
    uint32_t bm = (fb->blue_mask_size >= 32) ? 0xFFFFFFFFu : ((1u << fb->blue_mask_size) - 1u);

    uint32_t rv = ((uint32_t)r * rm) / 255u;
    uint32_t gv = ((uint32_t)g * gm) / 255u;
    uint32_t bv = ((uint32_t)b * bm) / 255u;

    return (rv << rp) | (gv << gp) | (bv << bp);
}

static void fb_put_pixel(const framebuffer_t *fb, uint32_t x, uint32_t y, uint32_t px) {
    if (!fb || !fb->addr) return;
    if (x >= fb->width || y >= fb->height) return;

    uint8_t *row = (uint8_t*)fb->addr + (uint64_t)y * fb->pitch;
    if (fb->bpp == 32) {
        ((uint32_t*)row)[x] = px;
    } else if (fb->bpp == 16) {
        ((uint16_t*)row)[x] = (uint16_t)px;
    }
}

static void fb_fill_rect(const framebuffer_t *fb, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t px) {
    if (!fb || !fb->addr) return;
    if (x >= fb->width || y >= fb->height) return;
    if (x + w > fb->width) w = fb->width - x;
    if (y + h > fb->height) h = fb->height - y;

    for (uint32_t yy = 0; yy < h; yy++) {
        uint32_t ry = y + yy;
        uint8_t *row = (uint8_t*)fb->addr + (uint64_t)ry * fb->pitch;
        if (fb->bpp == 32) {
            uint32_t *p = (uint32_t*)row;
            for (uint32_t xx = 0; xx < w; xx++) p[x + xx] = px;
        } else if (fb->bpp == 16) {
            uint16_t *p = (uint16_t*)row;
            for (uint32_t xx = 0; xx < w; xx++) p[x + xx] = (uint16_t)px;
        }
    }
}

static void fb_draw_gradient_bg(const framebuffer_t *fb) {
    for (uint32_t y = 0; y < fb->height; y++) {
        /* vertical gradient: deep navy -> near-black */
        uint8_t t = (uint8_t)((y * 255u) / (fb->height ? fb->height : 1));
        uint8_t r = (uint8_t)(10 + (t * 8u) / 255u);
        uint8_t g = (uint8_t)(16 + (t * 10u) / 255u);
        uint8_t b = (uint8_t)(35 + (t * 20u) / 255u);
        uint32_t px = fb_pack_rgb(fb, r, g, b);
        fb_fill_rect(fb, 0, y, fb->width, 1, px);
    }
}

static void fb_draw_glyph_scaled(const framebuffer_t *fb, uint32_t x, uint32_t y, uint8_t ch, uint32_t fg, uint32_t scale) {
    const uint8_t *g = bitmap_font_glyph8x16(ch);
    if (!g) return;
    for (uint32_t yy = 0; yy < 16; yy++) {
        uint8_t row = g[yy];
        for (uint32_t xx = 0; xx < 8; xx++) {
            if (row & (0x80u >> xx)) {
                fb_fill_rect(fb, x + xx*scale, y + yy*scale, scale, scale, fg);
            }
        }
    }
}

static void fb_draw_text(const framebuffer_t *fb, uint32_t x, uint32_t y, const char *s, uint32_t fg, uint32_t scale, uint32_t max_w_px) {
    uint32_t cx = x;
    uint32_t cy = y;
    uint32_t cell_w = 8 * scale;
    uint32_t cell_h = 16 * scale;

    while (s && *s) {
        char ch = *s++;
        if (ch == '\n') {
            cx = x;
            cy += cell_h;
            continue;
        }
        if (max_w_px && (cx + cell_w > x + max_w_px)) {
            cx = x;
            cy += cell_h;
        }
        fb_draw_glyph_scaled(fb, cx, cy, (uint8_t)ch, fg, scale);
        cx += cell_w;
    }
}

static void panic_draw_gui(const char* title, const char* message, const char* tips, const char* err_cat, const char* err_code, int seconds_left) {
    framebuffer_t fb;
    if (VGA_GetFrameBufferMode() != FB_MODE_GRAPHICS) {
        /* fallback */
        VGA_Clear();
        VGA_Write(title);
        VGA_Write("\n\n");
        VGA_Write(message);
        VGA_Write("\n\n");
        VGA_Writef("ERR_CODE_CAT: %s | ERR_CODE: %s\n", err_cat, err_code);
        VGA_Writef("Rebooting in %d seconds...\n", seconds_left);
        return;
    }
    if (VGA_GetFrameBuffer(&fb) != 0 || !fb.addr) {
        /* fallback */
        VGA_Clear();
        VGA_Write(title);
        VGA_Write("\n");
        VGA_Write(message);
        return;
    }

    /* Always clear first so we never end up with a blank/unchanged screen */
    fb_draw_gradient_bg(&fb);

    /* card */
    uint32_t card_w = (fb.width > 900) ? 900 : (fb.width > 40 ? fb.width - 40 : fb.width);
    uint32_t card_h = (fb.height > 520) ? 520 : (fb.height > 40 ? fb.height - 40 : fb.height);
    uint32_t card_x = (fb.width - card_w) / 2;
    uint32_t card_y = (fb.height - card_h) / 2;
    if (card_y < 180) card_y = 180; /* leave room for big top-left sad face */

    uint32_t card_bg = fb_pack_rgb(&fb, 20, 24, 38);
    uint32_t card_edge = fb_pack_rgb(&fb, 40, 50, 80);

    /* pseudo-rounded: draw border + inset */
    fb_fill_rect(&fb, card_x, card_y, card_w, card_h, card_edge);
    fb_fill_rect(&fb, card_x + 2, card_y + 2, card_w - 4, card_h - 4, card_bg);

    uint32_t fg1 = fb_pack_rgb(&fb, 240, 245, 255);
    uint32_t fg2 = fb_pack_rgb(&fb, 170, 185, 210);
    uint32_t accent = fb_pack_rgb(&fb, 90, 180, 255);

    /* Sad face (classic BSOD vibe) - top-left */
    fb_draw_text(&fb, 40, 30, ":(", fg1, 6, 0);

    /* Title (old panic style, but HD) */
    fb_draw_text(&fb, card_x + 28, card_y + 24, title, fg1, 2, card_w - 56);

    /* Guaranteed fallback text (fb console), in case direct fb drawing isn't visible */
    VGA_WriteTextAtPosition(0, 0, "[PANIC]");

    /* message */
    fb_draw_text(&fb, card_x + 28, card_y + 80, message, fg1, 1, card_w - 56);

    uint32_t y = card_y + 260;

    /* tips */
    if (tips && tips[0]) {
        fb_draw_text(&fb, card_x + 28, y, "Troubleshooting Tips:", fg2, 1, card_w - 56);
        y += 22;
        fb_draw_text(&fb, card_x + 28, y, tips, fg1, 1, card_w - 56);
        y += 86;
    }

    /* error code + footer */
    char code_line[256];
    code_line[0] = 0;
    strcat(code_line, "ERR_CODE_CAT: ");
    strcat(code_line, err_cat);
    strcat(code_line, " | ERR_CODE: ");
    strcat(code_line, err_code);

    fb_draw_text(&fb, card_x + 28, card_y + card_h - 140, code_line, fg2, 1, card_w - 56);
    fb_draw_text(&fb, card_x + 28, card_y + card_h - 110,
                 "If this issue repeats, please contact customer support at support.new-tech.com", fg2, 1, card_w - 56);

    char reboot_line[64];
    reboot_line[0] = 0;
    strcat(reboot_line, "The system will reboot shortly. Rebooting in ");
    char num[16];
    itoa(seconds_left, num, 10);
    strcat(reboot_line, num);
    strcat(reboot_line, " seconds...");
    fb_draw_text(&fb, card_x + 28, card_y + card_h - 80, reboot_line, fg2, 1, card_w - 56);
}

void panic(const char* title, const char* message, const char* tips, const char* err_cat, const char* err_code, int reboot_delay)
{
    for (int i = reboot_delay; i >= 0; i--) {
        panic_header(title);
        panic_draw_gui(title, message, tips, err_cat, err_code, i);
        rtc_wait_seconds(1);
    }

    acpi_reboot();
    for (;;) { __asm__("hlt"); }
}

void trigger_no_shell_panic() {
    panic(
        "Zenith4 has stopped responding",
        "The system cannot continue without the shell running.\n"
        "This may be due to memory corruption.",
        " - Check if your RAM is properly connected and not loose.\n"
        " - Try a different RAM stick.",
        "SYS_PROCESS",
        "ZENITH4_NOT_RUNNING",
        6
    );
}

void trigger_panic_dodev() {
    panic(
        "No hard disks were detected during boot.",
        "The system cannot continue without at least one storage device.\n"
        "This may be due to missing drivers, hardware failure, or misconfiguration.",
        " - Check if your storage devices are properly connected.\n"
        " - Try a different hardware configuration if available.",
        "HW_DEVICE",
        "NO_MEDIUM_FOUND",
        6
    );
}

void trigger_panic_doata() {
    panic(
        "The ATA Controller did not respond during boot.",
        "The system cannot continue without a functional ATA controller.\n"
        "This may be due to missing drivers, hardware failure, or misconfiguration.",
        " - Ensure your storage controller is enabled in BIOS/UEFI.\n"
        " - Verify that drives are properly connected.\n"
        " - Try a different hardware or emulator configuration if available.",
        "HW_DEVICE",
        "ATA_CONTROLLER_UNRESPONSIVE",
        6
    );
}

void trigger_panic_dops2() {
    panic(
        "The PS/2 keyboard did not respond during boot.",
        "The system cannot continue without a keyboard device.\n"
        "This may be due to missing drivers, hardware failure, or misconfiguration.",
        " - Check if your PS/2 device is properly connected.\n"
        " - Try a different hardware configuration if available.",
        "HW_TIMEOUT",
        "PS2_DEVICE_TIMEOUT",
        6
    );
}

void trigger_panic_dofs() {
    panic(
        "No FAT32 or ISO9660 filesystem was detected during boot.",
        "The system cannot continue without a valid filesystem.\n"
        "This may be due to:\n"
        " - Missing or corrupted partition/boot sector.\n"
        " - Unsupported filesystem type.\n"
        " - Drive not properly formatted.",
        " - Verify that your disk is formatted with FAT32.\n"
        " - Ensure the drive is properly connected and detected.\n"
        " - If using an image, confirm it contains a valid ISO9660 volume.",
        "FS_LAYER",
        "FS_INIT_NO_VALID_FS",
        6
    );
}

void trigger_panic_unknown() {
    panic(
        "An unexpected system crash has occurred.",
        "The system encountered a fatal error and cannot continue.\n"
        "Please restart your computer or contact a developer.",
        "If this error persists, report it with the steps to reproduce.",
        "UNKNOWN",
        "UNKNOWN_ERROR",
        6
    );
}
