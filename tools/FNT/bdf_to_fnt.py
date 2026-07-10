#!/usr/bin/env python3
"""
bdf_to_fnt.py — Convert BDF bitmap fonts to ModuOS FNT format
Tested with Terminus and other standard BDF fonts.

Usage:
    python3 bdf_to_fnt.py input.bdf output.fnt
    python3 bdf_to_fnt.py *.bdf output.fnt        (merge multiple BDF files)
"""

import sys
import struct
import os
import glob

# ── BDF parser ────────────────────────────────────────────────────────────

def parse_bdf(data):
    """
    Parse a BDF font file.
    Returns a dict with font metadata and a list of glyph dicts.
    """
    lines = data.decode("latin-1").splitlines()
    font  = {
        "name":      "",
        "point_size": 0,
        "max_width":  0,
        "max_height": 0,
        "ascent":     0,
        "descent":    0,
        "glyphs":     [],
    }

    i        = 0
    n        = len(lines)
    in_char  = False
    in_bitmap= False
    cur      = {}
    bitmap_rows = []

    while i < n:
        line = lines[i].strip()
        i += 1

        if not line:
            continue

        parts = line.split(None, 1)
        key   = parts[0].upper()
        val   = parts[1] if len(parts) > 1 else ""

        if key == "FONT":
            font["name"] = val.strip()
        elif key == "SIZE":
            tokens = val.split()
            if tokens: font["point_size"] = int(tokens[0])
        elif key == "FONTBOUNDINGBOX":
            tokens = val.split()
            if len(tokens) >= 2:
                font["max_width"]  = int(tokens[0])
                font["max_height"] = int(tokens[1])
        elif key == "FONT_ASCENT":
            font["ascent"] = int(val)
        elif key == "FONT_DESCENT":
            font["descent"] = int(val)

        elif key == "STARTCHAR":
            in_char     = True
            cur         = {"name": val.strip(), "encoding": -1,
                           "dwidth": 0, "bbx_w": 0, "bbx_h": 0,
                           "bbx_x": 0, "bbx_y": 0}
            bitmap_rows = []
            in_bitmap   = False

        elif in_char:
            if key == "ENCODING":
                cur["encoding"] = int(val)
            elif key == "DWIDTH":
                tokens = val.split()
                cur["dwidth"] = int(tokens[0]) if tokens else 0
            elif key == "BBX":
                tokens = val.split()
                if len(tokens) >= 4:
                    cur["bbx_w"] = int(tokens[0])
                    cur["bbx_h"] = int(tokens[1])
                    cur["bbx_x"] = int(tokens[2])
                    cur["bbx_y"] = int(tokens[3])
            elif key == "BITMAP":
                in_bitmap = True
            elif key == "ENDCHAR":
                if cur["encoding"] >= 0 and cur["bbx_w"] > 0 and cur["bbx_h"] > 0:
                    # Convert hex rows to bit arrays
                    rows = []
                    for hexrow in bitmap_rows:
                        bits = []
                        # Each hex row encodes bbx_w pixels
                        # Bytes needed: ceil(bbx_w / 8)
                        needed_bytes = (cur["bbx_w"] + 7) // 8
                        # Pad/trim hex string to needed bytes
                        hex_str = hexrow.ljust(needed_bytes * 2, '0')
                        for b_idx in range(needed_bytes):
                            byte_val = int(hex_str[b_idx*2 : b_idx*2+2], 16)
                            for bit in range(8):
                                bits.append((byte_val >> (7 - bit)) & 1)
                        rows.append(bits[:cur["bbx_w"]])
                    cur["bitmap"] = rows
                    font["glyphs"].append(cur)
                in_char   = False
                in_bitmap = False
                cur       = {}
                bitmap_rows = []
            elif in_bitmap:
                bitmap_rows.append(line.strip())

    font["glyphs"].sort(key=lambda g: g["encoding"])
    font["baseline"] = font["ascent"]
    return font


def merge_fonts(fonts):
    """Merge multiple parsed BDF fonts, deduplicating by codepoint (last wins)."""
    if not fonts:
        return None
    base = dict(fonts[0])
    glyph_map = {g["encoding"]: g for g in base["glyphs"]}
    for font in fonts[1:]:
        for g in font["glyphs"]:
            glyph_map[g["encoding"]] = g
        # Take larger metrics
        base["max_width"]  = max(base["max_width"],  font["max_width"])
        base["max_height"] = max(base["max_height"], font["max_height"])
        base["ascent"]     = max(base["ascent"],     font["ascent"])
    base["glyphs"]   = sorted(glyph_map.values(), key=lambda g: g["encoding"])
    base["baseline"] = base["ascent"]
    return base


# ── FNT writer ────────────────────────────────────────────────────────────

def write_fnt(font, out_path):
    glyphs   = font["glyphs"]
    name_str = font["name"]
    # Shorten XLFD name to something readable
    if name_str.startswith("-"):
        parts = name_str.split("-")
        if len(parts) >= 3:
            name_str = f"{parts[2]} {parts[3]}".strip()
    name_enc = name_str.encode("utf-8")[:255]

    n_glyphs = len(glyphs)
    max_w    = font["max_width"]
    max_h    = font["max_height"]
    baseline = font["baseline"]

    with open(out_path, "wb") as f:
        # FNT1 header
        f.write(b"FNT1")
        f.write(struct.pack("<H", 1))                   # version
        f.write(struct.pack("<H", len(name_enc)))       # name length
        f.write(name_enc)                               # name
        f.write(struct.pack("<H", max_w))               # max glyph width
        f.write(struct.pack("<H", max_h))               # glyph height
        f.write(struct.pack("<H", baseline))            # baseline
        f.write(struct.pack("<I", n_glyphs))            # glyph count

        for g in glyphs:
            bmp_w   = g["bbx_w"]
            bmp_h   = g["bbx_h"]
            dev_w   = g["dwidth"]
            bitmap  = g["bitmap"]
            cp      = g["encoding"]

            f.write(struct.pack("<I", cp))              # codepoint
            f.write(struct.pack("<H", dev_w))           # advance width
            f.write(struct.pack("<H", bmp_w))           # bitmap width
            f.write(struct.pack("<H", bmp_h))           # bitmap height

            # Pack bitmap: 1bpp, MSB first, rows padded to byte boundary
            bytes_per_row = (bmp_w + 7) // 8
            for row_idx in range(bmp_h):
                row = bitmap[row_idx] if row_idx < len(bitmap) else []
                for byte_i in range(bytes_per_row):
                    bv = 0
                    for bit_i in range(8):
                        col = byte_i * 8 + bit_i
                        if col < len(row) and row[col]:
                            bv |= (1 << (7 - bit_i))
                    f.write(bytes([bv]))

    return os.path.getsize(out_path)


# ── Entry point ───────────────────────────────────────────────────────────

def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)

    in_patterns = sys.argv[1:-1]
    out_path    = sys.argv[-1]

    # Expand globs
    in_files = []
    for pat in in_patterns:
        expanded = glob.glob(pat)
        if expanded:
            in_files.extend(sorted(expanded))
        elif os.path.isfile(pat):
            in_files.append(pat)

    if not in_files:
        print(f"Error: no input files found for: {in_patterns}")
        sys.exit(1)

    fonts = []
    for path in in_files:
        print(f"Reading {path} ...")
        with open(path, "rb") as f:
            data = f.read()
        font = parse_bdf(data)
        print(f"  Name:    {font['name']}")
        print(f"  Size:    {font['max_width']}x{font['max_height']}  ascent={font['ascent']}  descent={font['descent']}")
        print(f"  Glyphs:  {len(font['glyphs'])}")
        fonts.append(font)

    if len(fonts) == 1:
        merged = fonts[0]
    else:
        print(f"\nMerging {len(fonts)} fonts ...")
        merged = merge_fonts(fonts)
        print(f"  Total glyphs after merge: {len(merged['glyphs'])}")

    print(f"\nWriting FNT to {out_path} ...")
    size = write_fnt(merged, out_path)
    print(f"  Done — {size/1024:.1f} KB  ({len(merged['glyphs'])} glyphs)")
    print(f"\nFNT Info:")
    print(f"  Name:     {merged['name']}")
    print(f"  Size:     {merged['max_width']}x{merged['max_height']}")
    print(f"  Baseline: {merged['baseline']}")
    print(f"  Glyphs:   {len(merged['glyphs'])}")

if __name__ == "__main__":
    main()