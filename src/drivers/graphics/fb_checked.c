#include "moduos/drivers/graphics/fb_checked.h"
#include "moduos/kernel/panic.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/COM/com.h"

static void fb_panic_oob(const framebuffer_t *fb, uint32_t x, uint32_t y, uint64_t off, const char *op) {
    char msg[256];
    char tmp[32];
    // Very small formatting (no snprintf in kernel)
    // msg: op x y off pitch width height bpp
    msg[0] = 0;
    strcat(msg, op);
    strcat(msg, ": OOB framebuffer access. x=");
    itoa((int)x, tmp, 10); strcat(msg, tmp);
    strcat(msg, " y=");
    itoa((int)y, tmp, 10); strcat(msg, tmp);
    strcat(msg, " off=");
    // off can be large; truncate to 32-bit for message
    itoa((int)(off & 0xFFFFFFFFu), tmp, 10); strcat(msg, tmp);
    strcat(msg, " pitch="); itoa((int)fb->pitch, tmp, 10); strcat(msg, tmp);
    strcat(msg, " w="); itoa((int)fb->width, tmp, 10); strcat(msg, tmp);
    strcat(msg, " h="); itoa((int)fb->height, tmp, 10); strcat(msg, tmp);
    strcat(msg, " bpp="); itoa((int)fb->bpp, tmp, 10); strcat(msg, tmp);

    panic("Framebuffer OOB", msg, "Check pitch vs width*bytespp; clamp loops; consider double buffer", "GPU", "FB_OOB", 3);
}

void fb_checked_putpixel32(const framebuffer_t *fb, uint32_t x, uint32_t y, uint32_t px) {
    if (!fb || !fb->addr) return;
    if (fb->bpp != 32) {
        // only 32bpp supported in this helper
        return;
    }

    if (x >= fb->width || y >= fb->height) {
        fb_panic_oob(fb, x, y, 0, "putpixel32");
        return;
    }

    uint64_t off = (uint64_t)y * (uint64_t)fb->pitch + (uint64_t)x * 4ULL;
    uint64_t size = (uint64_t)fb->pitch * (uint64_t)fb->height;
    if (off + 4ULL > size) {
        fb_panic_oob(fb, x, y, off, "putpixel32");
        return;
    }

    uint8_t *base = (uint8_t*)fb->addr;
    *(uint32_t*)(base + off) = px;
}

void fb_checked_fill_rect32(const framebuffer_t *fb, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t px) {
    if (!fb || !fb->addr) return;
    if (fb->bpp != 32) return;

    // clamp end to detect overflow early
    uint64_t x2 = (uint64_t)x + (uint64_t)w;
    uint64_t y2 = (uint64_t)y + (uint64_t)h;
    if (x2 > fb->width || y2 > fb->height) {
        fb_panic_oob(fb, x, y, 0, "fill_rect32");
        return;
    }

    for (uint32_t yy = y; yy < y + h; yy++) {
        for (uint32_t xx = x; xx < x + w; xx++) {
            fb_checked_putpixel32(fb, xx, yy, px);
        }
    }
}
