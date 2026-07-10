#!/usr/bin/env python3
"""
TTF/OTF to FNT converter for ModuOS
Renders vector glyphs using freetype-py and packs them into the FNT1 binary format.

Dependencies:
    pip install freetype-py

Usage:
    python ttf_to_fnt.py input.ttf output.fnt [options]

Options:
    --size SIZE          Pixel height to render glyphs at (default: 16)
    --name NAME          Font name to embed (default: derived from filename)
    --range RANGE        Codepoint ranges to include, e.g. "0-127,160-255,0x2500-0x257F"
                         (default: 0-127 i.e. basic ASCII)
    --all-printable      Include all printable glyphs the font supports
    --missing-char CHAR  Fallback character for missing glyphs (default: '?')
    --verbose            Print progress information
"""

import argparse
import os
import struct
import sys
from pathlib import Path


# ---------------------------------------------------------------------------
# FNT1 binary layout helpers
# ---------------------------------------------------------------------------

MAGIC = b"FNT1"
VERSION = 1


def pack_header(font_name: str, max_width: int, glyph_height: int, baseline: int, num_glyphs: int) -> bytes:
    name_bytes = font_name.encode("utf-8")
    name_len = len(name_bytes)
    header = struct.pack(
        "<4sHH",
        MAGIC,
        VERSION,
        name_len,
    )
    header += name_bytes
    header += struct.pack(
        "<HHHI",
        max_width,
        glyph_height,
        baseline,
        num_glyphs,
    )
    return header


def pack_glyph(codepoint: int, char_width: int, bitmap_width: int, bitmap_height: int, bitmap_data: bytes) -> bytes:
    return struct.pack(
        "<IHHH",
        codepoint,
        char_width,
        bitmap_width,
        bitmap_height,
    ) + bitmap_data


def pixels_to_packed_bitmap(pixels: list[list[int]], width: int, height: int) -> bytes:
    """
    Convert a 2-D list of pixel values (0 = off, non-zero = on) into a
    packed 1-bpp bitmap: 8 pixels per byte, MSB first, each row padded to
    a byte boundary.
    """
    bytes_per_row = (width + 7) // 8
    result = bytearray()
    for row in pixels:
        byte_row = bytearray(bytes_per_row)
        for x, val in enumerate(row):
            if val:
                byte_row[x // 8] |= 0x80 >> (x % 8)
        result.extend(byte_row)
    return bytes(result)


# ---------------------------------------------------------------------------
# Codepoint range parser
# ---------------------------------------------------------------------------

def parse_ranges(range_str: str) -> list[int]:
    """Parse a comma-separated list of ranges like '0-127,0x2500-0x257F' into a sorted list of codepoints."""
    codepoints = []
    for part in range_str.split(","):
        part = part.strip()
        if not part:
            continue
        if "-" in part:
            # Could be "0x2500-0x257F" or "32-126"
            # Split on last '-' that follows a digit/hex char
            # Handle negative-safe: split on the dash that is NOT at position 0
            dash_idx = part.rfind("-")
            if dash_idx == 0:
                raise ValueError(f"Invalid range: {part}")
            lo_str = part[:dash_idx]
            hi_str = part[dash_idx + 1:]
            lo = int(lo_str, 16) if lo_str.startswith("0x") or lo_str.startswith("0X") else int(lo_str)
            hi = int(hi_str, 16) if hi_str.startswith("0x") or hi_str.startswith("0X") else int(hi_str)
            codepoints.extend(range(lo, hi + 1))
        else:
            cp = int(part, 16) if part.startswith("0x") or part.startswith("0X") else int(part)
            codepoints.append(cp)
    return sorted(set(codepoints))


# ---------------------------------------------------------------------------
# Freetype rendering
# ---------------------------------------------------------------------------

def render_glyphs_freetype(
    ttf_path: str,
    codepoints: list[int],
    pixel_size: int,
    verbose: bool,
) -> tuple[int, int, int, dict[int, dict]]:
    """
    Render each codepoint using freetype-py.

    Returns:
        (max_width, glyph_height, baseline, glyph_map)
        glyph_map: codepoint -> {char_width, bitmap_width, bitmap_height, pixels}
    """
    try:
        import freetype
    except ImportError:
        print("ERROR: freetype-py is not installed. Run: pip install freetype-py", file=sys.stderr)
        sys.exit(1)

    face = freetype.Face(ttf_path)
    face.set_pixel_sizes(0, pixel_size)

    # Derive global metrics from face metrics
    metrics = face.size
    # ascender / descender are in 26.6 fixed point (pixels * 64)
    ascender = (metrics.ascender + 63) >> 6
    descender = abs(metrics.descender) >> 6
    glyph_height = ascender + descender
    baseline = ascender  # pixels from top to baseline

    glyph_map: dict[int, dict] = {}
    max_width = 0

    for i, cp in enumerate(codepoints):
        if verbose and i % 100 == 0:
            print(f"  Rendering codepoint {i}/{len(codepoints)} (U+{cp:04X})…", flush=True)

        glyph_index = face.get_char_index(cp)
        if glyph_index == 0:
            # Font does not have this glyph
            continue

        try:
            face.load_glyph(glyph_index, freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_MONO)
        except Exception:
            continue

        ft_glyph = face.glyph
        bitmap = ft_glyph.bitmap

        bm_width = bitmap.width
        bm_rows = bitmap.rows
        pitch = bitmap.pitch  # bytes per row (signed; negative = bottom-up)

        # advance is in 26.6 fixed point
        char_width = (ft_glyph.advance.x + 32) >> 6

        # Compute where this glyph sits vertically relative to the cell top
        top = ft_glyph.bitmap_top   # pixels above baseline
        left = ft_glyph.bitmap_left  # pixels to the right of pen position

        # Build a full-height pixel grid (glyph_height rows, char_width cols)
        cell_w = max(char_width, bm_width + max(0, left))
        pixels = [[0] * cell_w for _ in range(glyph_height)]

        # blit the freetype bitmap into the cell
        if bm_width > 0 and bm_rows > 0:
            buf = bitmap.buffer  # flat list of bytes

            # freetype MONO bitmaps are 1-bpp MSB-first, like FNT1
            abs_pitch = abs(pitch)
            for row in range(bm_rows):
                y_cell = baseline - top + row
                if y_cell < 0 or y_cell >= glyph_height:
                    continue
                for col in range(bm_width):
                    byte_idx = row * abs_pitch + col // 8
                    bit = (buf[byte_idx] >> (7 - (col % 8))) & 1
                    x_cell = left + col
                    if 0 <= x_cell < cell_w:
                        pixels[y_cell][x_cell] = bit

        # Crop bitmap to actual ink bounds (keep full height for alignment)
        if bm_width == 0 or bm_rows == 0:
            # Space or invisible glyph
            bitmap_pixels = [[0] * char_width for _ in range(glyph_height)]
            bm_w_final = char_width if char_width > 0 else 1
        else:
            bitmap_pixels = pixels
            bm_w_final = cell_w

        packed = pixels_to_packed_bitmap(bitmap_pixels, bm_w_final, glyph_height)

        glyph_map[cp] = {
            "char_width": char_width,
            "bitmap_width": bm_w_final,
            "bitmap_height": glyph_height,
            "packed": packed,
        }

        if char_width > max_width:
            max_width = char_width

    return max_width, glyph_height, baseline, glyph_map


# ---------------------------------------------------------------------------
# FNT file writer
# ---------------------------------------------------------------------------

def write_fnt(
    output_path: str,
    font_name: str,
    max_width: int,
    glyph_height: int,
    baseline: int,
    glyph_map: dict[int, dict],
    verbose: bool,
) -> None:
    # Sort glyphs by codepoint (required for binary search in ModuOS)
    sorted_cps = sorted(glyph_map.keys())

    header = pack_header(font_name, max_width, glyph_height, baseline, len(sorted_cps))

    glyph_blobs = []
    for cp in sorted_cps:
        g = glyph_map[cp]
        blob = pack_glyph(cp, g["char_width"], g["bitmap_width"], g["bitmap_height"], g["packed"])
        glyph_blobs.append(blob)

    with open(output_path, "wb") as f:
        f.write(header)
        for blob in glyph_blobs:
            f.write(blob)

    total = os.path.getsize(output_path)
    if verbose:
        print(f"\nWrote {len(sorted_cps)} glyphs → {output_path} ({total:,} bytes)")


# ---------------------------------------------------------------------------
# Discover all codepoints supported by the font
# ---------------------------------------------------------------------------

def get_all_supported_codepoints(ttf_path: str) -> list[int]:
    """Return every codepoint the font actually has a glyph for."""
    try:
        import freetype
    except ImportError:
        print("ERROR: freetype-py is not installed. Run: pip install freetype-py", file=sys.stderr)
        sys.exit(1)

    face = freetype.Face(ttf_path)
    cps = []
    cp, idx = face.get_first_char()
    while idx != 0:
        if cp > 0:  # skip null
            cps.append(cp)
        cp, idx = face.get_next_char(cp, idx)
    return sorted(cps)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main() -> None:
    parser = argparse.ArgumentParser(
        description="Convert a TTF/OTF font to ModuOS FNT1 bitmap format.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument("input", help="Input TTF/OTF font file")
    parser.add_argument("output", help="Output .fnt file")
    parser.add_argument("--size", type=int, default=16, metavar="SIZE",
                        help="Render height in pixels (default: 16)")
    parser.add_argument("--name", default="", metavar="NAME",
                        help="Font name to embed (default: filename stem)")
    parser.add_argument("--range", dest="cp_range", default="32-126", metavar="RANGE",
                        help="Codepoint ranges to include (default: 32-126)")
    parser.add_argument("--all-printable", action="store_true",
                        help="Include every printable glyph the font supports (overrides --range)")
    parser.add_argument("--missing-char", default="?", metavar="CHAR",
                        help="ASCII fallback char for missing glyphs (default: '?')")
    parser.add_argument("--verbose", "-v", action="store_true",
                        help="Print progress information")
    args = parser.parse_args()

    ttf_path = args.input
    if not os.path.isfile(ttf_path):
        print(f"ERROR: Input file not found: {ttf_path}", file=sys.stderr)
        sys.exit(1)

    font_name = args.name or Path(ttf_path).stem

    # Determine codepoints to render
    if args.all_printable:
        if args.verbose:
            print("Discovering all supported codepoints…")
        codepoints = get_all_supported_codepoints(ttf_path)
        # Filter to printable Unicode (skip control chars below 0x20 except 0x09/0x0A)
        codepoints = [cp for cp in codepoints if cp >= 0x20 or cp in (0x09, 0x0A)]
        if args.verbose:
            print(f"  Found {len(codepoints)} supported codepoints")
    else:
        codepoints = parse_ranges(args.cp_range)
        if args.verbose:
            print(f"Requested {len(codepoints)} codepoints from range '{args.cp_range}'")

    # Always ensure the missing-char fallback is included
    missing_cp = ord(args.missing_char[0]) if args.missing_char else ord("?")
    if missing_cp not in codepoints:
        codepoints = sorted(set(codepoints) | {missing_cp})

    if args.verbose:
        print(f"Rendering '{font_name}' at {args.size}px …")

    max_width, glyph_height, baseline, glyph_map = render_glyphs_freetype(
        ttf_path, codepoints, args.size, args.verbose
    )

    if not glyph_map:
        print("ERROR: No glyphs could be rendered.", file=sys.stderr)
        sys.exit(1)

    if args.verbose:
        print(f"Rendered {len(glyph_map)} glyphs  |  cell: {max_width}×{glyph_height}  |  baseline: {baseline}px from top")

    write_fnt(args.output, font_name, max_width, glyph_height, baseline, glyph_map, args.verbose)

    if not args.verbose:
        print(f"Done: {len(glyph_map)} glyphs → {args.output}")


if __name__ == "__main__":
    main()