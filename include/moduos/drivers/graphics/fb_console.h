#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "moduos/drivers/graphics/framebuffer.h"
#include "moduos/drivers/graphics/bitmap_font.h"
#include "moduos/drivers/graphics/bmp_font.h"
#include "moduos/drivers/graphics/pf2.h"
#include "moduos/drivers/graphics/fnt_font.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    framebuffer_t fb;
    bool ready;

    /* Cursor in pixels */
    uint32_t x;
    uint32_t y;

    /* Text cursor rendering (framebuffer console cursor) */
    bool cursor_enabled;
    bool cursor_drawn;

    /* Current colors in VGA 0..15 indices */
    uint8_t fg;
    uint8_t bg;

    /* Optional BMP atlas font */
    bmp_font_t bmp_font;
    int bmp_font_ready;

    /* Optional PF2 font (Unicode) - pointer owned by caller */
    const pf2_font_t *pf2_font;
    int pf2_font_ready;

    /* FNT font (custom format) - pointer owned by caller */
    const fnt_font_t *fnt_font;
    int fnt_font_ready;

    /* When using bmp_font: render size (dest) for each glyph, in pixels */
    uint16_t bmp_render_w;
    uint16_t bmp_render_h;

    /* Margins */
    uint16_t margin_top;
    uint16_t margin_left;

    /* Cell size (pixels) */
    uint16_t cell_w;
    uint16_t cell_h;

    /* Incremental UTF-8 decode state for fbcon_putc (so per-byte writes can still render UTF-8). */
    uint8_t utf8_pending_len;   /* expected total bytes in sequence (0 if not in a sequence) */
    uint8_t utf8_pending_used;  /* how many bytes collected so far */
    uint8_t utf8_pending[4];    /* collected bytes */

    /* Flush batching / damage tracking (used to avoid per-glyph GPU flushes). */
    bool batch_flush;
    bool dirty_any;
    uint32_t dirty_x0, dirty_y0;
    uint32_t dirty_x1, dirty_y1; /* exclusive */
} fb_console_t;

/* Initialize console for a given framebuffer using built-in bitmap font. */
int fbcon_init(fb_console_t *c, const framebuffer_t *fb);

/* Attach a BMP atlas font (ModuOSDEF.bmp). bmp_buf must remain valid for lifetime of console. */
int fbcon_set_bmp_font_moduosdef(fb_console_t *c, const void *bmp_buf, size_t bmp_size);

/* Attach a PF2 font (Unicode). font must remain valid for lifetime of console. */
void fbcon_set_pf2_font(fb_console_t *c, const pf2_font_t *font);

/* Attach an FNT font (custom format). font must remain valid for lifetime of console. */
void fbcon_set_fnt_font(fb_console_t *c, const fnt_font_t *font);

void fbcon_set_text_color(fb_console_t *c, uint8_t fg, uint8_t bg);

void fbcon_clear(fb_console_t *c);
void fbcon_putc(fb_console_t *c, char ch);
void fbcon_write(fb_console_t *c, const char *s);
void fbcon_write_n(fb_console_t *c, const char *s, size_t n);
void fbcon_backspace(fb_console_t *c);

/*
 * Write text at a fixed cell position (row/col in text cells) without changing
 * the current fb console cursor.
 */
void fbcon_write_at(fb_console_t *c, uint32_t row, uint32_t col, const char *s);

/* Enable/disable a static cursor (non-blinking). */
void fbcon_set_cursor_enabled(fb_console_t *c, bool enabled);

/* Cursor position in character cells (row/col), derived from internal pixel cursor. */
void fbcon_get_cursor_pos(const fb_console_t *c, uint32_t *row, uint32_t *col);
void fbcon_set_cursor_pos(fb_console_t *c, uint32_t row, uint32_t col);

#ifdef __cplusplus
}
#endif
