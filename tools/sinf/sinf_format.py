"""
sinf_format.py
Core read/write/encode/decode logic for the SINF (Sine-Interpolated Font) format.

Binary layout is documented in FNT-Format.md, section "SINF: Sine-Interpolated
Font Format". Summary:

Header:
    4s   magic "SINF"
    H    version
    H    name_len
    Ns   name (utf-8)
    H    units_per_em
    I    glyph_count

Glyph:
    I    codepoint
    H    advance_width
    B    contour_count
    Contour[contour_count]

Contour:
    H    harmonic_count (K)
    Harmonic[K]

Harmonic (10 bytes):
    h    frequency (signed int16)
    f    real part
    f    imag part
"""

import struct
import numpy as np

MAGIC = b"SINF"
VERSION = 1

DEFAULT_HARMONICS = 24
DEFAULT_UNITS_PER_EM = 1000


# ---------------------------------------------------------------------------
# Data model
# ---------------------------------------------------------------------------

class Harmonic:
    __slots__ = ("freq", "re", "im")

    def __init__(self, freq, re, im):
        self.freq = int(freq)
        self.re = float(re)
        self.im = float(im)


class Contour:
    """A contour is just a list of Harmonic coefficients."""
    def __init__(self, harmonics):
        self.harmonics = harmonics

    def evaluate(self, num_samples):
        """Return an (num_samples, 2) array of (x, y) points tracing the contour."""
        t = np.linspace(0.0, 1.0, num_samples, endpoint=True)
        z = np.zeros(num_samples, dtype=complex)
        for h in self.harmonics:
            c = complex(h.re, h.im)
            z += c * np.exp(1j * 2 * np.pi * h.freq * t)
        return np.stack([z.real, z.imag], axis=1)


class Glyph:
    def __init__(self, codepoint, advance_width, contours):
        self.codepoint = codepoint
        self.advance_width = advance_width
        self.contours = contours  # list[Contour]


class SinfFont:
    def __init__(self, name, units_per_em, glyphs):
        self.name = name
        self.units_per_em = units_per_em
        self.glyphs = {g.codepoint: g for g in glyphs}

    def get_glyph(self, codepoint, fallback_char="?"):
        g = self.glyphs.get(codepoint)
        if g is None:
            g = self.glyphs.get(ord(fallback_char))
        return g


# ---------------------------------------------------------------------------
# Encoding: points -> Fourier coefficients
# ---------------------------------------------------------------------------

def _resample_polyline(points, num_samples):
    """Evenly resample a closed polyline (list of (x, y)) by arc length."""
    pts = np.asarray(points, dtype=float)
    if not np.allclose(pts[0], pts[-1]):
        pts = np.vstack([pts, pts[0]])  # close the loop

    seg = np.diff(pts, axis=0)
    seg_len = np.hypot(seg[:, 0], seg[:, 1])
    cum = np.concatenate([[0.0], np.cumsum(seg_len)])
    total = cum[-1]
    if total == 0:
        return np.repeat(pts[:1], num_samples, axis=0)

    targets = np.linspace(0, total, num_samples, endpoint=False)
    out = np.zeros((num_samples, 2))
    seg_idx = 0
    for i, d in enumerate(targets):
        while seg_idx < len(cum) - 2 and cum[seg_idx + 1] < d:
            seg_idx += 1
        seg_start = cum[seg_idx]
        seg_span = cum[seg_idx + 1] - seg_start
        frac = 0.0 if seg_span == 0 else (d - seg_start) / seg_span
        out[i] = pts[seg_idx] + frac * (pts[seg_idx + 1] - pts[seg_idx])
    return out


def encode_contour(points, num_harmonics=DEFAULT_HARMONICS, num_samples=256):
    """
    points: list of (x, y) in font units (a closed contour, first point
            need not equal last).
    Returns a Contour with the top `num_harmonics` DFT coefficients by
    magnitude.
    """
    resampled = _resample_polyline(points, num_samples)
    z = resampled[:, 0] + 1j * resampled[:, 1]

    X = np.fft.fft(z)                       # length num_samples
    freqs = np.fft.fftfreq(num_samples, d=1.0 / num_samples).astype(int)
    coeffs = X / num_samples                # normalize so IDFT-at-t formula works

    order = np.argsort(-np.abs(coeffs))     # descending magnitude
    keep = order[:num_harmonics]

    harmonics = [Harmonic(freqs[i], coeffs[i].real, coeffs[i].imag) for i in keep]
    return Contour(harmonics)


def encode_glyph(codepoint, advance_width, contour_point_lists,
                  num_harmonics=DEFAULT_HARMONICS, num_samples=256):
    """contour_point_lists: list of point-lists, one per contour."""
    contours = [encode_contour(pts, num_harmonics, num_samples)
                for pts in contour_point_lists]
    return Glyph(codepoint, advance_width, contours)


# ---------------------------------------------------------------------------
# Binary I/O
# ---------------------------------------------------------------------------

def save(font: SinfFont, path: str):
    with open(path, "wb") as f:
        name_bytes = font.name.encode("utf-8")
        f.write(MAGIC)
        f.write(struct.pack("<H", VERSION))
        f.write(struct.pack("<H", len(name_bytes)))
        f.write(name_bytes)
        f.write(struct.pack("<H", font.units_per_em))
        f.write(struct.pack("<I", len(font.glyphs)))

        for cp in sorted(font.glyphs.keys()):
            g = font.glyphs[cp]
            f.write(struct.pack("<I", g.codepoint))
            f.write(struct.pack("<H", g.advance_width))
            f.write(struct.pack("<B", len(g.contours)))
            for contour in g.contours:
                f.write(struct.pack("<H", len(contour.harmonics)))
                for h in contour.harmonics:
                    f.write(struct.pack("<hff", h.freq, h.re, h.im))


def load(path: str) -> SinfFont:
    with open(path, "rb") as f:
        data = f.read()

    off = 0
    magic = data[off:off + 4]; off += 4
    if magic != MAGIC:
        raise ValueError(f"Not a SINF file (magic was {magic!r})")

    version, = struct.unpack_from("<H", data, off); off += 2
    name_len, = struct.unpack_from("<H", data, off); off += 2
    name = data[off:off + name_len].decode("utf-8"); off += name_len
    units_per_em, = struct.unpack_from("<H", data, off); off += 2
    glyph_count, = struct.unpack_from("<I", data, off); off += 4

    glyphs = []
    for _ in range(glyph_count):
        codepoint, = struct.unpack_from("<I", data, off); off += 4
        advance_width, = struct.unpack_from("<H", data, off); off += 2
        contour_count, = struct.unpack_from("<B", data, off); off += 1

        contours = []
        for _ in range(contour_count):
            harmonic_count, = struct.unpack_from("<H", data, off); off += 2
            harmonics = []
            for _ in range(harmonic_count):
                freq, re, im = struct.unpack_from("<hff", data, off); off += 10
                harmonics.append(Harmonic(freq, re, im))
            contours.append(Contour(harmonics))

        glyphs.append(Glyph(codepoint, advance_width, contours))

    return SinfFont(name, units_per_em, glyphs)


# ---------------------------------------------------------------------------
# Rendering helper (shared by viewer, and usable standalone)
# ---------------------------------------------------------------------------

def render_glyph_points(glyph: Glyph, pixel_size, units_per_em, samples_per_contour=120):
    """
    Returns a list of point-lists (one per contour), each scaled from font
    units to pixels for the requested pixel_size, ready to draw as a filled
    polygon per contour.
    """
    scale = pixel_size / units_per_em
    out = []
    for contour in glyph.contours:
        pts = contour.evaluate(samples_per_contour) * scale
        out.append(pts)
    return out


def polygon_area(points):
    """Shoelace formula. Used to tell an outer contour from an inner hole
    (the larger-area contour is assumed to be the outer one)."""
    pts = np.asarray(points, dtype=float)
    if len(pts) < 3:
        return 0.0
    x, y = pts[:, 0], pts[:, 1]
    return 0.5 * abs(np.dot(x, np.roll(y, 1)) - np.dot(y, np.roll(x, 1)))


def glyph_storage_bytes(glyph: Glyph):
    """Approximate on-disk size of a single glyph entry, matching the binary
    layout in the spec (4+2+1 header, then per-contour 2 + 10*K)."""
    total = 4 + 2 + 1  # codepoint + advance_width + contour_count
    for c in glyph.contours:
        total += 2 + 10 * len(c.harmonics)
    return total


def layout_text(font: SinfFont, text, px_size, samples_per_contour=120):
    """
    Lay out a (possibly multi-line) string against a loaded SinfFont.

    Returns (items, total_width) where items is a list of dicts:
        {'char': ch, 'x': pen_x, 'y_offset': line_offset,
         'polygons': [(points_ndarray, is_hole), ...], 'missing': bool}
    or {'newline': True} as a line-break marker.
    Points in 'polygons' are already scaled to pixels but NOT yet
    positioned at a pen origin / baseline — caller adds pen_x, baseline_y.
    """
    scale = px_size / font.units_per_em
    pen_x = 0.0
    line_offset = 0.0
    items = []
    line_height = px_size * 1.3

    space_glyph = font.glyphs.get(32)
    space_advance = (space_glyph.advance_width if space_glyph else font.units_per_em * 0.5) * scale

    for ch in text:
        if ch == '\n':
            items.append({'newline': True})
            pen_x = 0.0
            line_offset += line_height
            continue
        if ch == ' ':
            pen_x += space_advance
            continue

        glyph = font.get_glyph(ord(ch), fallback_char=None)
        if glyph is None:
            items.append({'char': ch, 'x': pen_x, 'y_offset': line_offset,
                          'polygons': [], 'missing': True})
            pen_x += px_size * 0.6
            continue

        contours = [c.evaluate(samples_per_contour) * scale for c in glyph.contours]
        order = sorted(range(len(contours)), key=lambda i: -polygon_area(contours[i]))
        polygons = [(contours[i], rank > 0) for rank, i in enumerate(order)]

        items.append({'char': ch, 'x': pen_x, 'y_offset': line_offset,
                      'polygons': polygons, 'missing': False})
        pen_x += glyph.advance_width * scale

    return items, pen_x