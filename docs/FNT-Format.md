# FNT Font Format Specification

## Overview

FNT is a custom bitmap font format designed for ModuOS. It provides full Unicode support with efficient storage and rendering.

## File Format

### Header
```
Offset | Size | Type     | Description
-------|------|----------|---------------------------
0x00   | 4    | char[4]  | Magic: "FNT1"
0x04   | 2    | uint16   | Version (currently 1)
0x06   | 2    | uint16   | Font name length
0x08   | N    | char[]   | Font name (UTF-8)
N+8    | 2    | uint16   | Maximum glyph width
N+10   | 2    | uint16   | Glyph height
N+12   | 2    | uint16   | Baseline position
N+14   | 4    | uint32   | Number of glyphs
```

### Glyph Entry (repeated for each glyph)
```
Offset | Size | Type     | Description
-------|------|----------|---------------------------
0x00   | 4    | uint32   | Unicode codepoint
0x04   | 2    | uint16   | Actual character width (for spacing)
0x06   | 2    | uint16   | Bitmap width
0x08   | 2    | uint16   | Bitmap height
0x0A   | N    | uint8[]  | Packed bitmap data (1 bit per pixel)
```

### Bitmap Data

- **Packed Format**: 8 pixels per byte, MSB first
- **Row Padding**: Each row is padded to byte boundary
- **Bytes per row**: `(bitmap_width + 7) / 8`
- **Total size**: `bytes_per_row * bitmap_height`

## Features

- **Unicode Support**: Store any Unicode codepoint
- **Variable Width**: Each glyph can have different width
- **Efficient Lookup**: Binary search for non-ASCII, direct array for ASCII
- **Baseline**: Proper vertical alignment support
- **Compact**: 1-bit bitmap, minimal overhead

## Usage in ModuOS

### Loading a Font
```c
#include "moduos/drivers/graphics/fnt_font.h"

void *font_data = /* load from file */;
size_t font_size = /* file size */;

fnt_font_t *font = fnt_load_font(font_data, font_size);
if (font) {
    fbcon_set_fnt_font(&console, font);
}
```

### Getting a Glyph
```c
fnt_glyph_t *glyph = fnt_get_glyph(font, 0x0041); // 'A'
if (glyph) {
    // Render glyph->bitmap
}
```

### Rendering
```c
// Manual rendering
for (int y = 0; y < glyph->bitmap_height; y++) {
    for (int x = 0; x < glyph->bitmap_width; x++) {
        if (fnt_get_pixel(glyph, x, y)) {
            // Draw pixel at (x, y)
        }
    }
}

// Or use built-in renderer
fnt_render_glyph(glyph, framebuffer, x, y, pitch, bpp, fg_color, bg_color);
```

## Font Creation

Use the custom FNT font editor (Python/tkinter GUI) to create fonts:
- Visual bitmap glyph editing
- Unicode codepoint support  
- Real-time preview
- Export to .fnt format

## Default Font Location

ModuOS looks for fonts at: `/ModuOS/shared/usr/assets/fonts/Unicode.fnt`

## Advantages over PF2

- **No GNU dependencies**: Custom format, no licensing concerns
- **Simpler parsing**: Straightforward binary format
- **Better performance**: Optimized for OS needs
- **Easier tooling**: Custom editor tailored for ModuOS

## Notes

- All multi-byte values are **little-endian**
- Glyphs should be sorted by codepoint for binary search
- ASCII glyphs (0-127) are cached for fast access
- Missing glyphs fall back to '?' character
