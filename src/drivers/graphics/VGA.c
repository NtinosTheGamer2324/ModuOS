#include <stdint.h>
#include <stddef.h>
#include "moduos/drivers/graphics/VGA.h"
#include "moduos/kernel/io/io.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/COM/com.h"
#include "moduos/kernel/debug.h"
#include "moduos/drivers/graphics/fb_console.h"
#include "moduos/kernel/bootscreen.h"
#include <stdbool.h>
#include <stdarg.h>

#define WIDTH 80
#define HEIGHT 25
#define SCROLLBACK_LINES 100

#define VGA_ADDRESS 0xB8000
static volatile uint16_t* const VGA_BUFFER = (volatile uint16_t*)VGA_ADDRESS;

// Optional graphics framebuffer (for VBE / linear framebuffer usage)
static framebuffer_mode_t g_fb_mode = FB_MODE_TEXT;
static framebuffer_t g_fb = {0};
static bool vga_scrolling_enabled = true;

/* Graphics-mode text console (framebuffer-backed) */
static fb_console_t g_fbcon;
static bool g_fbcon_inited = false;
static bool g_splash_lock = false;

/* optional BMP font (owned buffer loaded from FS) */
static void *g_fbcon_font_bmp_buf = NULL;
static size_t g_fbcon_font_bmp_size = 0;
static int g_fbcon_font_bmp_loaded = 0;
// Scrollback buffer
static uint16_t scrollback_buffer[SCROLLBACK_LINES * WIDTH];
static int scrollback_position = 0;
static int scrollback_count = 0;
static int scroll_offset = 0;

// Color state
static uint8_t vga_fg_color = 0x07;
static uint8_t vga_bg_color = 0x00;
static uint8_t vga_color = 0x07;

// Cursor position
static int cursor_row = 0;
static int cursor_col = 0;

static void com_print_dec64(uint64_t v) {
    char tmp[32];
    int pos = 0;
    if (v == 0) { 
        com_write_string(COM1_PORT, "0"); 
        return; 
    }
    while (v > 0 && pos < 31) {
        tmp[pos++] = '0' + (v % 10);
        v /= 10;
    }
    for (int i = pos - 1; i >= 0; i--) {
        char c[2] = { tmp[i], 0 };
        com_write_string(COM1_PORT, c);
    }
}

// Helper functions
static inline uint16_t vga_entry(char c, uint8_t color) {
    return (uint16_t)c | ((uint16_t)color << 8);
}

static inline void update_vga_color(void) {
    vga_color = (vga_fg_color & 0x0F) | ((vga_bg_color & 0x0F) << 4);
}

framebuffer_mode_t VGA_GetFrameBufferMode(void) {
    return g_fb_mode;
}

int VGA_GetFrameBuffer(framebuffer_t *out) {
    if (!out) return -1;
    if (g_fb_mode != FB_MODE_GRAPHICS) {
        memset(out, 0, sizeof(*out));
        return -1;
    }
    *out = g_fb;
    return 0;
}

#include "moduos/fs/hvfs.h"
#include "moduos/kernel/kernel.h"

static void vga_try_init_fb_console(void) {
    if (g_fb_mode != FB_MODE_GRAPHICS) return;

    /* If the console is already initialized, we may still be able to attach the BMP font later
     * once the boot filesystem is mounted.
     */
    if (g_fbcon_inited) {
        if (!g_fbcon_font_bmp_loaded) {
            int slot = kernel_get_boot_slot();
            if (slot >= 0) {
                const char *path = "/ModuOS/shared/fonts/ModuOSDEF.bmp";
                int r = hvfs_read(slot, path, &g_fbcon_font_bmp_buf, &g_fbcon_font_bmp_size);
                if (r == 0 && g_fbcon_font_bmp_buf && g_fbcon_font_bmp_size) {
                    int fr = fbcon_set_bmp_font_moduosdef(&g_fbcon, g_fbcon_font_bmp_buf, g_fbcon_font_bmp_size);
                    if (fr == 0) {
                        g_fbcon_font_bmp_loaded = 1;
                        if (kernel_debug_is_med()) {
                            com_write_string(COM1_PORT, "[FBCON] Using BMP font: ");
                            com_write_string(COM1_PORT, path);
                            com_write_string(COM1_PORT, "\n");
                        }
                        if (kernel_debug_is_on()) {
                            com_printf(COM1_PORT,
                                       "[FBCON] BMP: w=%u h=%u cell=%ux%u alpha=%s thr=%u invert=%u\n",
                                       (unsigned)g_fbcon.bmp_font.img.width,
                                       (unsigned)g_fbcon.bmp_font.img.height,
                                       (unsigned)g_fbcon.bmp_font.cell_w,
                                       (unsigned)g_fbcon.bmp_font.cell_h,
                                       (g_fbcon.bmp_font.img.amask != 0) ? "yes" : "no",
                                       (unsigned)g_fbcon.bmp_font.threshold,
                                       (unsigned)g_fbcon.bmp_font.invert);
                            com_printf(COM1_PORT,
                                       "[FBCON] BMP masks: r=0x%08x g=0x%08x b=0x%08x a=0x%08x\n",
                                       (unsigned)g_fbcon.bmp_font.img.rmask,
                                       (unsigned)g_fbcon.bmp_font.img.gmask,
                                       (unsigned)g_fbcon.bmp_font.img.bmask,
                                       (unsigned)g_fbcon.bmp_font.img.amask);
                        }
                    } else {
                        if (kernel_debug_is_med()) {
                            com_write_string(COM1_PORT, "[FBCON] BMP font parse failed; keeping built-in font\n");
                        }
                        g_fbcon_font_bmp_loaded = 1; /* don't retry endlessly */
                    }
                } else {
                    if (kernel_debug_is_med()) {
                        com_write_string(COM1_PORT, "[FBCON] Could not read ModuOSDEF.bmp; keeping built-in font\n");
                    }
                    g_fbcon_font_bmp_loaded = 1; /* don't retry endlessly */
                }
            }
        }
        return;
    }

    if (fbcon_init(&g_fbcon, &g_fb) != 0) {
        if (kernel_debug_is_med()) {
            com_write_string(COM1_PORT, "[FBCON] fbcon_init failed\n");
        }
        return;
    }

    /* We'll attach ModuOSDEF.bmp later (after boot FS mount) if possible. */

    g_fbcon_inited = true;
    if (kernel_debug_is_med()) {
        com_write_string(COM1_PORT, "[FBCON] Framebuffer console initialized\n");
    }
}

void VGA_SetFrameBuffer(const framebuffer_t *fb) {
    // Explicit disable if NULL
    if (!fb) {
        g_fb_mode = FB_MODE_TEXT;
        memset(&g_fb, 0, sizeof(g_fb));
        g_fbcon_inited = false;
        if (kernel_debug_is_med()) {
            com_write_string(COM1_PORT, "[VGA] Framebuffer disabled (TEXT mode)\n");
        }
        return;
    }

    // If invalid, ignore request (do NOT downgrade from graphics mode silently)
    if (!fb->addr || fb->width == 0 || fb->height == 0 || fb->pitch == 0 || fb->bpp == 0) {
        return;
    }

    g_fb = *fb;

    /*
     * Ensure fmt is set to a stable internal value.
     * Some boot paths only provide bpp and leave fmt unknown.
     */
    if (g_fb.fmt == FB_FMT_UNKNOWN) {
        if (g_fb.bpp == 32) g_fb.fmt = FB_FMT_XRGB8888;
        else if (g_fb.bpp == 16) g_fb.fmt = FB_FMT_RGB565;
    }

    g_fb_mode = FB_MODE_GRAPHICS;
    if (kernel_debug_is_med()) {
        com_write_string(COM1_PORT, "[VGA] Framebuffer enabled (GRAPHICS mode)\n");
    }

    /* Lazily initialize fb console on first write/clear */
    vga_try_init_fb_console();
}

static uint32_t fb_pack_rgb888(uint8_t r, uint8_t g, uint8_t b) {
    // If we have channel info (Multiboot2), use it.
    if (g_fb.red_mask_size && g_fb.green_mask_size && g_fb.blue_mask_size) {
        uint32_t rp = (g_fb.red_pos);
        uint32_t gp = (g_fb.green_pos);
        uint32_t bp = (g_fb.blue_pos);

        uint32_t rm = (g_fb.red_mask_size >= 32) ? 0xFFFFFFFFu : ((1u << g_fb.red_mask_size) - 1u);
        uint32_t gm = (g_fb.green_mask_size >= 32) ? 0xFFFFFFFFu : ((1u << g_fb.green_mask_size) - 1u);
        uint32_t bm = (g_fb.blue_mask_size >= 32) ? 0xFFFFFFFFu : ((1u << g_fb.blue_mask_size) - 1u);

        uint32_t rv = ((uint32_t)r * rm) / 255u;
        uint32_t gv = ((uint32_t)g * gm) / 255u;
        uint32_t bv = ((uint32_t)b * bm) / 255u;

        return (rv << rp) | (gv << gp) | (bv << bp);
    }

    // Fallback: assume XRGB8888 (0x00RRGGBB)
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

void VGA_ClearFrameBuffer(uint32_t color) {
    if (g_fb_mode != FB_MODE_GRAPHICS) return;
    if (!g_fb.addr || g_fb.pitch == 0 || g_fb.height == 0) return;

    uint8_t *p = (uint8_t*)g_fb.addr;

    // Input color is 0x00RRGGBB
    uint8_t r = (uint8_t)((color >> 16) & 0xFF);
    uint8_t g = (uint8_t)((color >> 8) & 0xFF);
    uint8_t b = (uint8_t)(color & 0xFF);

    if (g_fb.bpp == 32) {
        uint32_t px = fb_pack_rgb888(r, g, b);
        for (uint32_t y = 0; y < g_fb.height; y++) {
            uint32_t *row = (uint32_t*)(p + (uint64_t)y * g_fb.pitch);
            for (uint32_t x = 0; x < g_fb.width; x++) row[x] = px;
        }
    } else if (g_fb.bpp == 24) {
        uint32_t px = fb_pack_rgb888(r, g, b);
        for (uint32_t y = 0; y < g_fb.height; y++) {
            uint8_t *row = (uint8_t*)(p + (uint64_t)y * g_fb.pitch);
            for (uint32_t x = 0; x < g_fb.width; x++) {
                // write least significant 3 bytes
                row[x*3 + 0] = (uint8_t)(px & 0xFF);
                row[x*3 + 1] = (uint8_t)((px >> 8) & 0xFF);
                row[x*3 + 2] = (uint8_t)((px >> 16) & 0xFF);
            }
        }
    } else if (g_fb.bpp == 16) {
        // For 16bpp, pack into the provided field sizes/positions if present.
        uint32_t px32 = fb_pack_rgb888(r, g, b);
        uint16_t px = (uint16_t)px32;
        for (uint32_t y = 0; y < g_fb.height; y++) {
            uint16_t *row = (uint16_t*)(p + (uint64_t)y * g_fb.pitch);
            for (uint32_t x = 0; x < g_fb.width; x++) row[x] = px;
        }
    } else {
        // Unknown bpp: just zero the whole buffer
        for (uint32_t y = 0; y < g_fb.height; y++) {
            memset(p + (uint64_t)y * g_fb.pitch, 0, g_fb.pitch);
        }
    }
}

/* Text color control (public API) */
void VGA_SetTextColor(uint8_t fg, uint8_t bg) {
    vga_fg_color = (uint8_t)(fg & 0x0F);
    vga_bg_color = (uint8_t)(bg & 0x0F);
    update_vga_color();
    if (g_fb_mode == FB_MODE_GRAPHICS && g_fbcon_inited) {
        fbcon_set_text_color(&g_fbcon, vga_fg_color, vga_bg_color);
    }
}

uint8_t VGA_GetTextColor(void) {
    return (uint8_t)((vga_bg_color << 4) | (vga_fg_color & 0x0F));
}

void VGA_ResetTextColor(void) {
    vga_fg_color = 0x07;
    vga_bg_color = 0x00;
    update_vga_color();
    if (g_fb_mode == FB_MODE_GRAPHICS && g_fbcon_inited) {
        fbcon_set_text_color(&g_fbcon, vga_fg_color, vga_bg_color);
    }
}

static void update_cursor(int row, int col) {
    uint16_t pos = row * WIDTH + col;
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}

static void save_line_to_scrollback(int row) {
    int buf_row = scrollback_position % SCROLLBACK_LINES;
    for (int col = 0; col < WIDTH; col++) {
        scrollback_buffer[buf_row * WIDTH + col] = VGA_BUFFER[row * WIDTH + col];
    }
    scrollback_position++;
    if (scrollback_count < SCROLLBACK_LINES) {
        scrollback_count++;
    }
}

static void scroll(void) {
    if (!vga_scrolling_enabled) {
        cursor_row = HEIGHT - 1;
        cursor_col = 0;
        return;
    }

    save_line_to_scrollback(0);
    
    for (int row = 1; row < HEIGHT; row++) {
        for (int col = 0; col < WIDTH; col++) {
            VGA_BUFFER[(row - 1) * WIDTH + col] = VGA_BUFFER[row * WIDTH + col];
        }
    }

    for (int col = 0; col < WIDTH; col++) {
        VGA_BUFFER[(HEIGHT - 1) * WIDTH + col] = vga_entry(' ', vga_color);
    }

    cursor_row = HEIGHT - 1;
    scroll_offset = 0;
}

static uint8_t parse_color_code(const char* code, int len, bool* is_background) {
    *is_background = false;
    
    if (len == 2 && code[0] == 'c') {
        switch (code[1]) {
            case 'b': return 0x01; case 'p': return 0x05;
            case 'r': return 0x04; case 'g': return 0x02;
            case 'y': return 0x06; case 'c': return 0x03;
            case 'w': return 0x0F; case 'l': return 0x08;
            case 'k': return 0x00;
        }
    } else if (len == 3 && code[0] == 'c' && code[1] == 'l') {
        switch (code[2]) {
            case 'b': return 0x09; case 'p': return 0x0D;
            case 'r': return 0x0C; case 'g': return 0x0A;
            case 'y': return 0x0E; case 'c': return 0x0B;
            case 'w': return 0x0F;
        }
    } else if (len == 2 && code[0] == 'b') {
        *is_background = true;
        switch (code[1]) {
            case 'b': return 0x01; case 'p': return 0x05;
            case 'r': return 0x04; case 'g': return 0x02;
            case 'y': return 0x06; case 'c': return 0x03;
            case 'w': return 0x07; case 'l': return 0x08;
            case 'k': return 0x00;
        }
    } else if (len == 3 && code[0] == 'b' && code[1] == 'l') {
        *is_background = true;
        switch (code[2]) {
            case 'b': return 0x09; case 'p': return 0x0D;
            case 'r': return 0x0C; case 'g': return 0x0A;
            case 'y': return 0x0E; case 'c': return 0x0B;
            case 'w': return 0x0F;
        }
    }
    return 0xFF;
}

static int try_parse_color(const char* code, int len, uint8_t* out_color, bool* is_background) {
    uint8_t color = parse_color_code(code, len, is_background);
    if (color == 0xFF) return 0;
    *out_color = color;
    return 1;
}

/* ============================================================
   ANSI escape support (minimal)

   Supports: ESC [ ... m  (SGR)
     - 0 reset
     - 30-37, 90-97 foreground
     - 40-47, 100-107 background
     - 39 default foreground (07)
     - 49 default background (00)
   ============================================================ */

static uint8_t ansi_to_vga_16(uint8_t base8, bool bright) {
    /* ANSI base8: 0=black 1=red 2=green 3=yellow 4=blue 5=magenta 6=cyan 7=white */
    static const uint8_t map8_to_vga[8] = {
        0x0, /* black */
        0x4, /* red */
        0x2, /* green */
        0x6, /* yellow/brown */
        0x1, /* blue */
        0x5, /* magenta */
        0x3, /* cyan */
        0x7  /* white/lt gray */
    };

    uint8_t v = map8_to_vga[base8 & 7];
    if (bright) v |= 0x8;
    return v;
}

static void vga_apply_sgr_param(int p) {
    if (p == 0) {
        VGA_ResetTextColor();
        return;
    }

    if (p == 39) {
        vga_fg_color = 0x07;
        update_vga_color();
        return;
    }

    if (p == 49) {
        vga_bg_color = 0x00;
        update_vga_color();
        return;
    }

    /* Foreground */
    if (p >= 30 && p <= 37) {
        vga_fg_color = ansi_to_vga_16((uint8_t)(p - 30), false);
        update_vga_color();
        return;
    }
    if (p >= 90 && p <= 97) {
        vga_fg_color = ansi_to_vga_16((uint8_t)(p - 90), true);
        update_vga_color();
        return;
    }

    /* Background */
    if (p >= 40 && p <= 47) {
        vga_bg_color = ansi_to_vga_16((uint8_t)(p - 40), false) & 0x7; /* keep bg non-bright */
        update_vga_color();
        return;
    }
    if (p >= 100 && p <= 107) {
        /* bright backgrounds aren't well supported in classic VGA (blink bit). Treat as normal. */
        vga_bg_color = ansi_to_vga_16((uint8_t)(p - 100), false) & 0x7;
        update_vga_color();
        return;
    }
}

static int vga_try_parse_ansi_sgr(const char *s, size_t max_len, size_t *consumed) {
    /* Bounded parse for buffers (used by VGA_WriteN) */
    *consumed = 0;
    if (max_len < 2) return 0;
    if ((uint8_t)s[0] != 0x1B) return 0;
    if (s[1] != '[') return 0;

    size_t i = 2;
    int param = 0;
    bool have_param = false;

    /* Empty ESC[m means reset */
    if (i < max_len && s[i] == 'm') {
        VGA_ResetTextColor();
        *consumed = 3;
        return 1;
    }

    while (i < max_len) {
        char c = s[i];

        if (c >= '0' && c <= '9') {
            have_param = true;
            param = (param * 10) + (c - '0');
            i++;
            continue;
        }

        if (c == ';') {
            vga_apply_sgr_param(have_param ? param : 0);
            param = 0;
            have_param = false;
            i++;
            continue;
        }

        if (c == 'm') {
            vga_apply_sgr_param(have_param ? param : 0);
            i++;
            *consumed = i;
            return 1;
        }

        return 0;
    }

    return 0;
}

static int vga_try_parse_ansi_sgr_z(const char *s, size_t *consumed) {
    /* Null-terminated parse for VGA_Write */
    *consumed = 0;
    if (!s) return 0;
    if ((uint8_t)s[0] != 0x1B) return 0;
    if (s[1] != '[') return 0;

    size_t i = 2;
    int param = 0;
    bool have_param = false;

    if (s[i] == 'm') {
        VGA_ResetTextColor();
        *consumed = 3;
        return 1;
    }

    while (s[i] != '\0') {
        char c = s[i];

        if (c >= '0' && c <= '9') {
            have_param = true;
            param = (param * 10) + (c - '0');
            i++;
            continue;
        }

        if (c == ';') {
            vga_apply_sgr_param(have_param ? param : 0);
            param = 0;
            have_param = false;
            i++;
            continue;
        }

        if (c == 'm') {
            vga_apply_sgr_param(have_param ? param : 0);
            i++;
            *consumed = i;
            return 1;
        }

        return 0;
    }

    return 0;
}

static void print_padded_number(int num, bool zero_pad, int width, int base, bool is_unsigned) {
    char buffer[32];
    unsigned int n;
    bool negative = false;

    if (!is_unsigned && num < 0) {
        negative = true;
        n = (unsigned int)(-num);
    } else {
        n = (unsigned int)num;
    }

    int i = 0;
    do {
        unsigned int digit = n % base;
        buffer[i++] = (digit < 10) ? '0' + digit : 'a' + (digit - 10);
        n /= base;
    } while (n > 0);

    if (negative) buffer[i++] = '-';
    buffer[i] = '\0';

    // Reverse
    for (int j = 0; j < i / 2; j++) {
        char tmp = buffer[j];
        buffer[j] = buffer[i - j - 1];
        buffer[i - j - 1] = tmp;
    }

    int len = i;
    int pad = (width > len) ? width - len : 0;

    char temp[2] = {0};
    for (int j = 0; j < pad; j++) {
        temp[0] = zero_pad ? '0' : ' ';
        VGA_Write(temp);
    }

    VGA_Write(buffer);
}

// Public API
void VGA_SetSplashLock(bool enabled) {
    g_splash_lock = enabled;
}

void VGA_Clear(void) {
    // In graphics mode, clear the active framebuffer instead of text memory.
    if (g_fb_mode == FB_MODE_GRAPHICS) {
        vga_try_init_fb_console();
        if (g_fbcon_inited) {
            fbcon_clear(&g_fbcon);
        } else {
            VGA_ClearFrameBuffer(0);
        }
        return;
    }

    for (int i = 0; i < WIDTH * HEIGHT; i++) {
        VGA_BUFFER[i] = vga_entry(' ', vga_color);
    }
    cursor_row = 0;
    cursor_col = 0;
    scroll_offset = 0;
    update_cursor(cursor_row, cursor_col);
}

void VGA_WriteChar(char c) {
    if (g_fb_mode == FB_MODE_GRAPHICS && g_splash_lock) {
        return;
    }

    /* Treat ASCII backspace as a backspace operation. */
    if (c == '\b') {
        VGA_Backspace();
        return;
    }

    if (g_fb_mode == FB_MODE_GRAPHICS) {
        vga_try_init_fb_console();
        if (g_fbcon_inited) {
            fbcon_set_text_color(&g_fbcon, vga_fg_color, vga_bg_color);
            fbcon_putc(&g_fbcon, c);
            // Keep bootscreen/logo on top
            bootscreen_overlay_redraw();
        }
        return;
    }
    if (scroll_offset > 0) {
        scroll_offset = 0;
    }
    
    if (c == '\n') {
        cursor_col = 0;
        cursor_row++;
        if (cursor_row >= HEIGHT) {
            scroll();
        }
    } else if (c == '\r') {
        cursor_col = 0;
    } else {
        if (cursor_col >= WIDTH) {
            cursor_col = 0;
            cursor_row++;
            if (cursor_row >= HEIGHT) {
                if (vga_scrolling_enabled)
                    scroll();
                else
                    cursor_row = HEIGHT - 1;
            }
        }

        VGA_BUFFER[cursor_row * WIDTH + cursor_col] = vga_entry(c, vga_color);
        cursor_col++;

        if (cursor_col >= WIDTH) {
            cursor_col = 0;
            cursor_row++;
            if (cursor_row >= HEIGHT) scroll();
        }
    }

    update_cursor(cursor_row, cursor_col);
}

void VGA_Write(const char* str) {
    if (g_fb_mode == FB_MODE_GRAPHICS && g_splash_lock) {
        return;
    }
    if (g_fb_mode == FB_MODE_GRAPHICS) {
        if (!str) return;
        vga_try_init_fb_console();
        if (!g_fbcon_inited) return;

        while (*str) {
            /* ANSI: ESC[...m */
            if ((uint8_t)str[0] == 0x1B) {
                size_t consumed = 0;
                if (vga_try_parse_ansi_sgr_z(str, &consumed)) {
                    str += consumed;
                    fbcon_set_text_color(&g_fbcon, vga_fg_color, vga_bg_color);
                    continue;
                }
            }

            if (*str == '\\' && *(str + 1)) {
                // Backspace
                if (*(str + 1) == 'b' && (*(str + 2) == '\0' || 
                    (*(str + 2) != 'b' && *(str + 2) != 'r' && *(str + 2) != 'g' && 
                     *(str + 2) != 'y' && *(str + 2) != 'c' && *(str + 2) != 'p' && 
                     *(str + 2) != 'w' && *(str + 2) != 'k' && *(str + 2) != 'l'))) {
                    fbcon_backspace(&g_fbcon);
                    str += 2;
                    continue;
                }

                // Reset colors
                if (*(str + 1) == 'r' && *(str + 2) == 'r') {
                    vga_fg_color = 0x07;
                    vga_bg_color = 0x00;
                    update_vga_color();
                    fbcon_set_text_color(&g_fbcon, vga_fg_color, vga_bg_color);
                    str += 3;
                    continue;
                }

                // Try 3-char color
                uint8_t col;
                bool is_bg;
                if (*(str + 3) && try_parse_color(str + 1, 3, &col, &is_bg)) {
                    if (is_bg) vga_bg_color = col;
                    else vga_fg_color = col;
                    update_vga_color();
                    fbcon_set_text_color(&g_fbcon, vga_fg_color, vga_bg_color);
                    str += 4;
                    continue;
                }

                // Try 2-char color
                if (*(str + 2) && try_parse_color(str + 1, 2, &col, &is_bg)) {
                    if (is_bg) vga_bg_color = col;
                    else vga_fg_color = col;
                    update_vga_color();
                    fbcon_set_text_color(&g_fbcon, vga_fg_color, vga_bg_color);
                    str += 3;
                    continue;
                }
            }

            fbcon_set_text_color(&g_fbcon, vga_fg_color, vga_bg_color);
            fbcon_putc(&g_fbcon, *str++);
        }
        // Keep bootscreen/logo on top
        bootscreen_overlay_redraw();
        return;
    }
    while (*str) {
        /* ANSI: ESC[...m */
        if ((uint8_t)str[0] == 0x1B) {
            size_t consumed = 0;
            if (vga_try_parse_ansi_sgr_z(str, &consumed)) {
                str += consumed;
                continue;
            }
        }

        if (*str == '\\' && *(str + 1)) {
            // Backspace
            if (*(str + 1) == 'b' && (*(str + 2) == '\0' || 
                (*(str + 2) != 'b' && *(str + 2) != 'r' && *(str + 2) != 'g' && 
                 *(str + 2) != 'y' && *(str + 2) != 'c' && *(str + 2) != 'p' && 
                 *(str + 2) != 'w' && *(str + 2) != 'k' && *(str + 2) != 'l'))) {
                VGA_Backspace();
                str += 2;
                continue;
            }

            // Reset colors
            if (*(str + 1) == 'r' && *(str + 2) == 'r') {
                vga_fg_color = 0x07;
                vga_bg_color = 0x00;
                update_vga_color();
                str += 3;
                continue;
            }

            // Try 3-char color
            uint8_t col;
            bool is_bg;
            if (*(str + 3) && try_parse_color(str + 1, 3, &col, &is_bg)) {
                if (is_bg) vga_bg_color = col;
                else vga_fg_color = col;
                update_vga_color();
                str += 4;
                continue;
            }

            // Try 2-char color
            if (*(str + 2) && try_parse_color(str + 1, 2, &col, &is_bg)) {
                if (is_bg) vga_bg_color = col;
                else vga_fg_color = col;
                update_vga_color();
                str += 3;
                continue;
            }
        }
        
        VGA_WriteChar(*str++);
    }
}

void VGA_WriteN(const char *buf, size_t count) {
    if (g_fb_mode == FB_MODE_GRAPHICS && g_splash_lock) {
        return;
    }
    if (g_fb_mode == FB_MODE_GRAPHICS) {
        if (!buf) return;
        vga_try_init_fb_console();
        if (!g_fbcon_inited) return;

        for (size_t i = 0; i < count; i++) {
            char ch = buf[i];

            /* ANSI: ESC[...m */
            if ((uint8_t)ch == 0x1B && (i + 1) < count && buf[i + 1] == '[') {
                size_t consumed = 0;
                if (vga_try_parse_ansi_sgr(&buf[i], count - i, &consumed)) {
                    if (consumed > 0) {
                        i += (consumed - 1);
                        fbcon_set_text_color(&g_fbcon, vga_fg_color, vga_bg_color);
                        continue;
                    }
                }
            }

            if (ch == '\\' && i + 1 < count) {
                // Backspace handling
                if (buf[i+1] == 'b') {
                    if (i + 2 >= count || (buf[i+2] != 'b' && buf[i+2] != 'r' && 
                        buf[i+2] != 'g' && buf[i+2] != 'y' && buf[i+2] != 'c' && 
                        buf[i+2] != 'p' && buf[i+2] != 'w' && buf[i+2] != 'k' && buf[i+2] != 'l')) {
                        fbcon_backspace(&g_fbcon);
                        i += 1;
                        continue;
                    }
                }

                // Reset colors
                if (i + 2 < count && buf[i+1] == 'r' && buf[i+2] == 'r') {
                    vga_fg_color = 0x07;
                    vga_bg_color = 0x00;
                    update_vga_color();
                    fbcon_set_text_color(&g_fbcon, vga_fg_color, vga_bg_color);
                    i += 2;
                    continue;
                }

                // Try color codes
                uint8_t col;
                bool is_bg;
                if (i + 3 < count && try_parse_color(&buf[i+1], 3, &col, &is_bg)) {
                    if (is_bg) vga_bg_color = col;
                    else vga_fg_color = col;
                    update_vga_color();
                    fbcon_set_text_color(&g_fbcon, vga_fg_color, vga_bg_color);
                    i += 3;
                    continue;
                }

                if (i + 2 < count && try_parse_color(&buf[i+1], 2, &col, &is_bg)) {
                    if (is_bg) vga_bg_color = col;
                    else vga_fg_color = col;
                    update_vga_color();
                    fbcon_set_text_color(&g_fbcon, vga_fg_color, vga_bg_color);
                    i += 2;
                    continue;
                }
            }

            fbcon_set_text_color(&g_fbcon, vga_fg_color, vga_bg_color);
            fbcon_putc(&g_fbcon, ch);
        }
        // Keep bootscreen/logo on top
        bootscreen_overlay_redraw();
        return;
    }
    for (size_t i = 0; i < count; i++) {
        char ch = buf[i];

        /* ANSI: ESC[...m */
        if ((uint8_t)ch == 0x1B && (i + 1) < count && buf[i + 1] == '[') {
            size_t consumed = 0;
            if (vga_try_parse_ansi_sgr(&buf[i], count - i, &consumed)) {
                if (consumed > 0) {
                    i += (consumed - 1);
                    continue;
                }
            }
        }

        if (ch == '\\' && i + 1 < count) {
            // Backspace handling
            if (buf[i+1] == 'b') {
                if (i + 2 >= count || (buf[i+2] != 'b' && buf[i+2] != 'r' && 
                    buf[i+2] != 'g' && buf[i+2] != 'y' && buf[i+2] != 'c' && 
                    buf[i+2] != 'p' && buf[i+2] != 'w' && buf[i+2] != 'k' && buf[i+2] != 'l')) {
                    VGA_Backspace();
                    i += 1;
                    continue;
                }
            }

            // Reset colors
            if (i + 2 < count && buf[i+1] == 'r' && buf[i+2] == 'r') {
                vga_fg_color = 0x07;
                vga_bg_color = 0x00;
                update_vga_color();
                i += 2;
                continue;
            }

            // Try color codes
            uint8_t col;
            bool is_bg;
            if (i + 3 < count && try_parse_color(&buf[i+1], 3, &col, &is_bg)) {
                if (is_bg) vga_bg_color = col;
                else vga_fg_color = col;
                update_vga_color();
                i += 3;
                continue;
            }

            if (i + 2 < count && try_parse_color(&buf[i+1], 2, &col, &is_bg)) {
                if (is_bg) vga_bg_color = col;
                else vga_fg_color = col;
                update_vga_color();
                i += 2;
                continue;
            }
        }

        VGA_WriteChar(ch);
    }
}

void VGA_WriteTextAtPosition(int row, int col, const char *str) {
    if (!str) return;
    if (g_fb_mode == FB_MODE_GRAPHICS && g_splash_lock) return;

    /* clamp */
    if (row < 0) row = 0;
    if (col < 0) col = 0;
    if (row >= HEIGHT) row = HEIGHT - 1;
    if (col >= WIDTH) col = WIDTH - 1;

    /* GRAPHICS mode: route to framebuffer console */
    if (g_fb_mode == FB_MODE_GRAPHICS && g_fbcon_inited) {
        fbcon_write_at(&g_fbcon, (uint32_t)row, (uint32_t)col, str);
        return;
    }

    /* TEXT mode: save cursor, write, restore */
    int saved_row = cursor_row;
    int saved_col = cursor_col;

    cursor_row = row;
    cursor_col = col;
    update_cursor(cursor_row, cursor_col);

    VGA_Write(str);

    cursor_row = saved_row;
    cursor_col = saved_col;
    update_cursor(cursor_row, cursor_col);
}

void VGA_Writef(const char* fmt, ...) {
    if (g_fb_mode == FB_MODE_GRAPHICS) {
        if (!fmt) return;
        vga_try_init_fb_console();
        if (!g_fbcon_inited) return;

        va_list args;
        va_start(args, fmt);

        char temp_str[2] = {0};

        for (; *fmt != '\0'; fmt++) {
            if (*fmt == '\\' && *(fmt + 1)) {
                // Handle colors (same logic as VGA_Write)
                if (*(fmt + 1) == 'b' && (*(fmt + 2) == '\0' || 
                    (*(fmt + 2) != 'b' && *(fmt + 2) != 'r' && *(fmt + 2) != 'g' && 
                     *(fmt + 2) != 'y' && *(fmt + 2) != 'c' && *(fmt + 2) != 'p' && 
                     *(fmt + 2) != 'w' && *(fmt + 2) != 'k' && *(fmt + 2) != 'l'))) {
                    fbcon_backspace(&g_fbcon);
                    fmt++;
                    continue;
                }

                if (*(fmt + 1) == 'r' && *(fmt + 2) == 'r') {
                    vga_fg_color = 0x07;
                    vga_bg_color = 0x00;
                    update_vga_color();
                    fbcon_set_text_color(&g_fbcon, vga_fg_color, vga_bg_color);
                    fmt += 2;
                    continue;
                }

                uint8_t col;
                bool is_bg;
                if (*(fmt + 3) && try_parse_color(fmt + 1, 3, &col, &is_bg)) {
                    if (is_bg) vga_bg_color = col;
                    else vga_fg_color = col;
                    update_vga_color();
                    fbcon_set_text_color(&g_fbcon, vga_fg_color, vga_bg_color);
                    fmt += 3;
                    continue;
                }

                if (*(fmt + 2) && try_parse_color(fmt + 1, 2, &col, &is_bg)) {
                    if (is_bg) vga_bg_color = col;
                    else vga_fg_color = col;
                    update_vga_color();
                    fbcon_set_text_color(&g_fbcon, vga_fg_color, vga_bg_color);
                    fmt += 2;
                    continue;
                }

                temp_str[0] = *fmt;
                fbcon_putc(&g_fbcon, temp_str[0]);
                continue;
            }

            if (*fmt == '%') {
                fmt++;
                if (*fmt == '\0') break;

                if (*fmt == '%') {
                    fbcon_putc(&g_fbcon, '%');
                    continue;
                }

                bool zero_pad = false;
                if (*fmt == '0') {
                    zero_pad = true;
                    fmt++;
                }

                int width = 0;
                while (*fmt >= '0' && *fmt <= '9') {
                    width = width * 10 + (*fmt - '0');
                    fmt++;
                }

                if (*fmt == '\0') break;

                /* Use existing helper which calls VGA_Write; we can't, so implement small local printing */
                char buffer[64];
                buffer[0] = 0;

                switch (*fmt) {
                    case 'd': {
                        int num = va_arg(args, int);
                        itoa(num, buffer, 10);
                        break;
                    }
                    case 'u': {
                        unsigned int num = va_arg(args, unsigned int);
                        /* itoa works for non-negative values */
                        itoa((int)num, buffer, 10);
                        break;
                    }
                    case 'x': {
                        unsigned int num = va_arg(args, unsigned int);
                        itoa((int)num, buffer, 16);
                        break;
                    }
                    case 's': {
                        const char* s = va_arg(args, const char*);
                        fbcon_write(&g_fbcon, s ? s : "(null)");
                        buffer[0] = 0;
                        break;
                    }
                    case 'c': {
                        char ch = (char)va_arg(args, int);
                        fbcon_putc(&g_fbcon, ch);
                        buffer[0] = 0;
                        break;
                    }
                    default: {
                        fbcon_putc(&g_fbcon, '%');
                        fbcon_putc(&g_fbcon, *fmt);
                        buffer[0] = 0;
                        break;
                    }
                }

                if (buffer[0]) {
                    /* padding */
                    int len = (int)strlen(buffer);
                    int pad = (width > len) ? (width - len) : 0;
                    for (int i = 0; i < pad; i++) fbcon_putc(&g_fbcon, zero_pad ? '0' : ' ');
                    fbcon_write(&g_fbcon, buffer);
                }
            } else {
                fbcon_putc(&g_fbcon, *fmt);
            }
        }

        va_end(args);
        return;
    }
    va_list args;
    va_start(args, fmt);

    char temp_str[2] = {0};

    for (; *fmt != '\0'; fmt++) {
        if (*fmt == '\\' && *(fmt + 1)) {
            // Handle colors (same logic as VGA_Write)
            if (*(fmt + 1) == 'b' && (*(fmt + 2) == '\0' || 
                (*(fmt + 2) != 'b' && *(fmt + 2) != 'r' && *(fmt + 2) != 'g' && 
                 *(fmt + 2) != 'y' && *(fmt + 2) != 'c' && *(fmt + 2) != 'p' && 
                 *(fmt + 2) != 'w' && *(fmt + 2) != 'k' && *(fmt + 2) != 'l'))) {
                VGA_Backspace();
                fmt++;
                continue;
            }

            if (*(fmt + 1) == 'r' && *(fmt + 2) == 'r') {
                vga_fg_color = 0x07;
                vga_bg_color = 0x00;
                update_vga_color();
                fmt += 2;
                continue;
            }

            uint8_t col;
            bool is_bg;
            if (*(fmt + 3) && try_parse_color(fmt + 1, 3, &col, &is_bg)) {
                if (is_bg) vga_bg_color = col;
                else vga_fg_color = col;
                update_vga_color();
                fmt += 3;
                continue;
            }

            if (*(fmt + 2) && try_parse_color(fmt + 1, 2, &col, &is_bg)) {
                if (is_bg) vga_bg_color = col;
                else vga_fg_color = col;
                update_vga_color();
                fmt += 2;
                continue;
            }

            temp_str[0] = *fmt;
            VGA_Write(temp_str);
            continue;
        }

        if (*fmt == '%') {
            fmt++;
            if (*fmt == '\0') break;

            if (*fmt == '%') {
                fmt++;
                if (*fmt == '\0') {
                    VGA_Write("%");
                    break;
                }
                VGA_Write("%");
                temp_str[0] = *fmt;
                VGA_Write(temp_str);
                continue;
            }

            bool zero_pad = false;
            if (*fmt == '0') {
                zero_pad = true;
                fmt++;
            }

            int width = 0;
            while (*fmt >= '0' && *fmt <= '9') {
                width = width * 10 + (*fmt - '0');
                fmt++;
            }

            if (*fmt == '\0') break;

            switch (*fmt) {
                case 'd': {
                    int num = va_arg(args, int);
                    print_padded_number(num, zero_pad, width, 10, false);
                    break;
                }
                case 'u': {
                    unsigned int num = va_arg(args, unsigned int);
                    print_padded_number((int)num, zero_pad, width, 10, true);
                    break;
                }
                case 'x': {
                    unsigned int num = va_arg(args, unsigned int);
                    VGA_Write("0x");
                    print_padded_number((int)num, zero_pad, width, 16, true);
                    break;
                }
                case 's': {
                    const char* str = va_arg(args, const char*);
                    VGA_Write(str ? str : "(null)");
                    break;
                }
                case 'c': {
                    char ch = (char)va_arg(args, int);
                    temp_str[0] = ch;
                    VGA_Write(temp_str);
                    break;
                }
                case '%': {
                    VGA_Write("%");
                    break;
                }
                default: {
                    VGA_Write("%");
                    temp_str[0] = *fmt;
                    VGA_Write(temp_str);
                    break;
                }
            }
        } else {
            temp_str[0] = *fmt;
            VGA_Write(temp_str);
        }
    }

    va_end(args);
}

void VGA_Backspace(void) {
    if (g_fb_mode == FB_MODE_GRAPHICS) {
        vga_try_init_fb_console();
        if (g_fbcon_inited) fbcon_backspace(&g_fbcon);
        return;
    }
    if (cursor_col == 0 && cursor_row == 0) return;

    if (cursor_col == 0) {
        cursor_row--;
        cursor_col = WIDTH - 1;
    } else {
        cursor_col--;
    }

    VGA_BUFFER[cursor_row * WIDTH + cursor_col] = vga_entry(' ', vga_color);
    update_cursor(cursor_row, cursor_col);
}

void VGA_ScrollUp(int lines) {
    if (scrollback_count == 0) return;
    
    scroll_offset += lines;
    if (scroll_offset > scrollback_count) {
        scroll_offset = scrollback_count;
    }
    
    for (int row = 0; row < HEIGHT; row++) {
        int source_index = scrollback_position - scroll_offset - (HEIGHT - row);
        
        if (source_index >= 0 && source_index < scrollback_position) {
            int buf_row = source_index % SCROLLBACK_LINES;
            for (int col = 0; col < WIDTH; col++) {
                VGA_BUFFER[row * WIDTH + col] = scrollback_buffer[buf_row * WIDTH + col];
            }
        } else if (source_index >= scrollback_position) {
            int current_row = source_index - scrollback_position;
            if (current_row >= HEIGHT) {
                for (int col = 0; col < WIDTH; col++) {
                    VGA_BUFFER[row * WIDTH + col] = vga_entry(' ', vga_color);
                }
            }
        }
    }
}

void VGA_ScrollDown(int lines) {
    if (scroll_offset == 0) return;
    
    scroll_offset -= lines;
    if (scroll_offset < 0) scroll_offset = 0;
    
    if (scroll_offset == 0) return;
    
    for (int row = 0; row < HEIGHT; row++) {
        int source_index = scrollback_position - scroll_offset - (HEIGHT - row);
        
        if (source_index >= 0 && source_index < scrollback_position) {
            int buf_row = source_index % SCROLLBACK_LINES;
            for (int col = 0; col < WIDTH; col++) {
                VGA_BUFFER[row * WIDTH + col] = scrollback_buffer[buf_row * WIDTH + col];
            }
        }
    }
}

void VGA_ScrollReset(void) {
    scroll_offset = 0;
}

int VGA_GetScrollOffset(void) {
    return scroll_offset;
}

void VGA_EnableScrolling(bool enable) {
    vga_scrolling_enabled = enable;
}

bool VGA_IsScrollingEnabled(void) {
    return vga_scrolling_enabled;
}

void VGA_HideCursor(void) {
    if (g_fb_mode == FB_MODE_GRAPHICS) {
        vga_try_init_fb_console();
        if (g_fbcon_inited) fbcon_set_cursor_enabled(&g_fbcon, false);
        return;
    }
    outb(0x3D4, 0x0A);
    outb(0x3D5, 0x20);
}

void VGA_ShowCursor(void) {
    if (g_fb_mode == FB_MODE_GRAPHICS) {
        vga_try_init_fb_console();
        if (g_fbcon_inited) fbcon_set_cursor_enabled(&g_fbcon, true);
        return;
    }
    outb(0x3D4, 0x0A);
    outb(0x3D5, 0x0E);
    outb(0x3D4, 0x0B);
    outb(0x3D5, 0x0F);
    update_cursor(cursor_row, cursor_col);
}