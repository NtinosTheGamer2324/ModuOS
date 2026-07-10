#!/usr/bin/env python3
"""
fnt_viewer.py — ModuOS FNT font viewer
Usage: python3 fnt_viewer.py [font.fnt]
"""

import sys
import struct
import tkinter as tk
from tkinter import ttk, filedialog, messagebox

# ── FNT parser ────────────────────────────────────────────────────────────

def parse_fnt(data):
    pos = 0

    magic = data[pos:pos+4]
    if magic != b"FNT1":
        raise ValueError(f"Not an FNT file (magic={magic!r})")
    pos += 4

    version  = struct.unpack_from("<H", data, pos)[0]; pos += 2
    name_len = struct.unpack_from("<H", data, pos)[0]; pos += 2
    name     = data[pos:pos+name_len].decode("utf-8", errors="replace"); pos += name_len
    max_w    = struct.unpack_from("<H", data, pos)[0]; pos += 2
    height   = struct.unpack_from("<H", data, pos)[0]; pos += 2
    baseline = struct.unpack_from("<H", data, pos)[0]; pos += 2
    n_glyphs = struct.unpack_from("<I", data, pos)[0]; pos += 4

    glyphs = []
    for _ in range(n_glyphs):
        if pos + 10 > len(data): break
        cp    = struct.unpack_from("<I", data, pos)[0]; pos += 4
        dev_w = struct.unpack_from("<H", data, pos)[0]; pos += 2
        bmp_w = struct.unpack_from("<H", data, pos)[0]; pos += 2
        bmp_h = struct.unpack_from("<H", data, pos)[0]; pos += 2

        bytes_per_row = (bmp_w + 7) // 8
        total = bytes_per_row * bmp_h
        raw   = data[pos:pos+total]; pos += total

        # Unpack bitmap
        rows = []
        for row in range(bmp_h):
            cols = []
            for byte_i in range(bytes_per_row):
                idx = row * bytes_per_row + byte_i
                b   = raw[idx] if idx < len(raw) else 0
                for bit_i in range(8):
                    cols.append((b >> (7 - bit_i)) & 1)
            rows.append(cols[:bmp_w])

        glyphs.append({
            "codepoint": cp,
            "dev_width": dev_w,
            "bmp_width": bmp_w,
            "bmp_height": bmp_h,
            "bitmap": rows,
        })

    return {
        "version":  version,
        "name":     name,
        "max_width": max_w,
        "height":   height,
        "baseline": baseline,
        "glyphs":   glyphs,
    }


# ── App ───────────────────────────────────────────────────────────────────

BG       = "#0d1117"
BG2      = "#161b22"
BG3      = "#21262d"
BORDER   = "#30363d"
FG       = "#e6edf3"
FG_DIM   = "#8b949e"
ACCENT   = "#58a6ff"
ACCENT2  = "#3fb950"
PIXEL_ON = "#58a6ff"
PIXEL_OFF= "#1c2128"
BASELINE_COL = "#f85149"

class FntViewer(tk.Tk):
    def __init__(self, path=None):
        super().__init__()
        self.title("FNT Font Viewer — ModuOS")
        self.configure(bg=BG)
        self.geometry("1100x720")
        self.minsize(900, 600)

        self.font_data   = None
        self.glyphs      = []
        self.filtered    = []
        self.selected_idx = None
        self.zoom        = 12
        self.show_baseline = tk.BooleanVar(value=True)
        self.show_grid     = tk.BooleanVar(value=True)

        self._build_ui()
        self._style()

        if path:
            self._load_file(path)

    def _style(self):
        style = ttk.Style(self)
        style.theme_use("clam")
        style.configure("TScrollbar",
            background=BG3, troughcolor=BG2, bordercolor=BORDER,
            arrowcolor=FG_DIM, relief="flat")
        style.configure("Search.TEntry",
            fieldbackground=BG3, foreground=FG, insertcolor=FG,
            bordercolor=BORDER, relief="flat")

    def _build_ui(self):
        # ── Top bar ───────────────────────────────────────────────────
        topbar = tk.Frame(self, bg=BG2, pady=8, padx=12)
        topbar.pack(fill="x", side="top")

        tk.Button(topbar, text="Open FNT…", command=self._open_dialog,
            bg=ACCENT, fg="#0d1117", font=("Segoe UI", 9, "bold"),
            relief="flat", padx=12, pady=4, cursor="hand2",
            activebackground="#388bfd", activeforeground="#0d1117"
        ).pack(side="left")

        self.title_lbl = tk.Label(topbar, text="No file loaded",
            bg=BG2, fg=FG_DIM, font=("Segoe UI", 10))
        self.title_lbl.pack(side="left", padx=16)

        self.info_lbl = tk.Label(topbar, text="",
            bg=BG2, fg=FG_DIM, font=("Segoe UI", 9))
        self.info_lbl.pack(side="right", padx=8)

        # ── Main area ─────────────────────────────────────────────────
        main = tk.Frame(self, bg=BG)
        main.pack(fill="both", expand=True)

        # Left panel: glyph list
        left = tk.Frame(main, bg=BG2, width=260, bd=0)
        left.pack(side="left", fill="y")
        left.pack_propagate(False)

        # Search
        search_frame = tk.Frame(left, bg=BG2, pady=8, padx=8)
        search_frame.pack(fill="x")
        tk.Label(search_frame, text="Search", bg=BG2, fg=FG_DIM,
            font=("Segoe UI", 8)).pack(anchor="w")
        self.search_var = tk.StringVar()
        self.search_var.trace_add("write", lambda *_: self._filter_glyphs())
        search_entry = tk.Entry(search_frame, textvariable=self.search_var,
            bg=BG3, fg=FG, insertbackground=FG,
            relief="flat", font=("Consolas", 10),
            highlightthickness=1, highlightcolor=ACCENT,
            highlightbackground=BORDER)
        search_entry.pack(fill="x", ipady=4)
        tk.Label(search_frame, text="char, U+XXXX, or codepoint",
            bg=BG2, fg=FG_DIM, font=("Segoe UI", 7)).pack(anchor="w")

        # Count label
        self.count_lbl = tk.Label(left, text="0 glyphs",
            bg=BG2, fg=FG_DIM, font=("Segoe UI", 8))
        self.count_lbl.pack(anchor="w", padx=8)

        # Listbox
        list_frame = tk.Frame(left, bg=BG2)
        list_frame.pack(fill="both", expand=True, padx=4, pady=4)

        scrollbar = ttk.Scrollbar(list_frame, orient="vertical", style="TScrollbar")
        self.listbox = tk.Listbox(list_frame,
            bg=BG3, fg=FG, selectbackground=ACCENT, selectforeground="#0d1117",
            font=("Consolas", 10), relief="flat", bd=0,
            highlightthickness=0, activestyle="none",
            yscrollcommand=scrollbar.set)
        scrollbar.config(command=self.listbox.yview)
        scrollbar.pack(side="right", fill="y")
        self.listbox.pack(fill="both", expand=True)
        self.listbox.bind("<<ListboxSelect>>", self._on_select)

        # ── Right panel: viewer + info ────────────────────────────────
        right = tk.Frame(main, bg=BG)
        right.pack(side="left", fill="both", expand=True)

        # Controls bar
        ctrl = tk.Frame(right, bg=BG2, pady=6, padx=12)
        ctrl.pack(fill="x")

        tk.Label(ctrl, text="Zoom", bg=BG2, fg=FG_DIM,
            font=("Segoe UI", 9)).pack(side="left")

        tk.Button(ctrl, text="−", command=lambda: self._set_zoom(self.zoom - 2),
            bg=BG3, fg=FG, font=("Segoe UI", 10, "bold"),
            relief="flat", padx=8, pady=2, cursor="hand2",
            activebackground=BORDER).pack(side="left", padx=(4,0))
        self.zoom_lbl = tk.Label(ctrl, text=f"{self.zoom}×",
            bg=BG2, fg=ACCENT, font=("Segoe UI", 9, "bold"), width=4)
        self.zoom_lbl.pack(side="left")
        tk.Button(ctrl, text="+", command=lambda: self._set_zoom(self.zoom + 2),
            bg=BG3, fg=FG, font=("Segoe UI", 10, "bold"),
            relief="flat", padx=8, pady=2, cursor="hand2",
            activebackground=BORDER).pack(side="left")

        ttk.Separator(ctrl, orient="vertical").pack(side="left", padx=12, fill="y")

        tk.Checkbutton(ctrl, text="Baseline", variable=self.show_baseline,
            command=self._redraw, bg=BG2, fg=FG_DIM, selectcolor=BG3,
            activebackground=BG2, activeforeground=FG,
            font=("Segoe UI", 9)).pack(side="left", padx=4)
        tk.Checkbutton(ctrl, text="Grid", variable=self.show_grid,
            command=self._redraw, bg=BG2, fg=FG_DIM, selectcolor=BG3,
            activebackground=BG2, activeforeground=FG,
            font=("Segoe UI", 9)).pack(side="left", padx=4)

        # Navigation
        tk.Button(ctrl, text="◀", command=self._prev_glyph,
            bg=BG3, fg=FG, relief="flat", padx=8, pady=2, cursor="hand2",
            font=("Segoe UI", 9), activebackground=BORDER).pack(side="right")
        tk.Button(ctrl, text="▶", command=self._next_glyph,
            bg=BG3, fg=FG, relief="flat", padx=8, pady=2, cursor="hand2",
            font=("Segoe UI", 9), activebackground=BORDER).pack(side="right", padx=2)

        # Canvas area
        canvas_frame = tk.Frame(right, bg=BG, padx=24, pady=24)
        canvas_frame.pack(fill="both", expand=True)

        self.canvas = tk.Canvas(canvas_frame, bg=BG, highlightthickness=0)
        self.canvas.pack(fill="both", expand=True)

        # Glyph info bar
        info_bar = tk.Frame(right, bg=BG2, pady=8, padx=16)
        info_bar.pack(fill="x", side="bottom")

        self.glyph_char_lbl = tk.Label(info_bar, text="",
            bg=BG2, fg=FG, font=("Segoe UI", 24))
        self.glyph_char_lbl.pack(side="left", padx=(0,16))

        meta_frame = tk.Frame(info_bar, bg=BG2)
        meta_frame.pack(side="left")

        self.meta_cp  = tk.Label(meta_frame, text="Codepoint: —",
            bg=BG2, fg=FG, font=("Consolas", 10))
        self.meta_cp.pack(anchor="w")
        self.meta_sz  = tk.Label(meta_frame, text="Bitmap: —",
            bg=BG2, fg=FG_DIM, font=("Consolas", 10))
        self.meta_sz.pack(anchor="w")
        self.meta_dw  = tk.Label(meta_frame, text="Advance width: —",
            bg=BG2, fg=FG_DIM, font=("Consolas", 10))
        self.meta_dw.pack(anchor="w")

        # Keyboard
        self.bind("<Left>",  lambda e: self._prev_glyph())
        self.bind("<Right>", lambda e: self._next_glyph())
        self.bind("<plus>",  lambda e: self._set_zoom(self.zoom + 2))
        self.bind("<minus>", lambda e: self._set_zoom(self.zoom - 2))
        self.canvas.bind("<Configure>", lambda e: self._redraw())

    # ── File loading ──────────────────────────────────────────────────────

    def _open_dialog(self):
        path = filedialog.askopenfilename(
            title="Open FNT font",
            filetypes=[("FNT fonts", "*.fnt"), ("All files", "*.*")])
        if path:
            self._load_file(path)

    def _load_file(self, path):
        try:
            with open(path, "rb") as f:
                data = f.read()
            self.font_data = parse_fnt(data)
        except Exception as e:
            messagebox.showerror("Error", f"Failed to load font:\n{e}")
            return

        fd = self.font_data
        self.glyphs = fd["glyphs"]
        self.title(f"FNT Viewer — {fd['name']}")
        self.title_lbl.config(text=f"{fd['name']}  v{fd['version']}")
        self.info_lbl.config(
            text=f"{fd['max_width']}×{fd['height']}px  •  baseline {fd['baseline']}  •  {len(self.glyphs)} glyphs")

        self._filter_glyphs()
        if self.filtered:
            self.listbox.select_set(0)
            self._on_select(None)

    # ── Filtering ─────────────────────────────────────────────────────────

    def _filter_glyphs(self):
        q = self.search_var.get().strip()
        if not q:
            self.filtered = list(self.glyphs)
        else:
            result = []
            # Try as single char
            if len(q) == 1:
                cp = ord(q)
                result = [g for g in self.glyphs if g["codepoint"] == cp]
            # Try U+XXXX
            if not result and q.upper().startswith("U+"):
                try:
                    cp = int(q[2:], 16)
                    result = [g for g in self.glyphs if g["codepoint"] == cp]
                except ValueError:
                    pass
            # Try decimal
            if not result:
                try:
                    cp = int(q)
                    result = [g for g in self.glyphs if g["codepoint"] == cp]
                except ValueError:
                    pass
            # Try hex
            if not result:
                try:
                    cp = int(q, 16)
                    result = [g for g in self.glyphs if g["codepoint"] == cp]
                except ValueError:
                    pass
            self.filtered = result if result else []

        self.listbox.delete(0, "end")
        for g in self.filtered:
            cp = g["codepoint"]
            try:
                ch = chr(cp) if cp < 0x10FFFF else "?"
                if cp < 0x20 or (0x7F <= cp < 0xA0): ch = "·"
            except Exception:
                ch = "?"
            self.listbox.insert("end", f"  {ch}  U+{cp:04X}  ({cp})")

        self.count_lbl.config(text=f"{len(self.filtered)} glyph{'s' if len(self.filtered)!=1 else ''}")

    # ── Selection ─────────────────────────────────────────────────────────

    def _on_select(self, event):
        sel = self.listbox.curselection()
        if not sel: return
        self.selected_idx = sel[0]
        self._redraw()

    def _prev_glyph(self):
        if self.selected_idx is None or not self.filtered: return
        idx = max(0, self.selected_idx - 1)
        self.listbox.selection_clear(0, "end")
        self.listbox.select_set(idx)
        self.listbox.see(idx)
        self.selected_idx = idx
        self._redraw()

    def _next_glyph(self):
        if self.selected_idx is None or not self.filtered: return
        idx = min(len(self.filtered) - 1, self.selected_idx + 1)
        self.listbox.selection_clear(0, "end")
        self.listbox.select_set(idx)
        self.listbox.see(idx)
        self.selected_idx = idx
        self._redraw()

    def _set_zoom(self, z):
        self.zoom = max(4, min(32, z))
        self.zoom_lbl.config(text=f"{self.zoom}×")
        self._redraw()

    # ── Drawing ───────────────────────────────────────────────────────────

    def _redraw(self):
        self.canvas.delete("all")
        if self.selected_idx is None or not self.filtered: return
        if self.selected_idx >= len(self.filtered): return

        g  = self.filtered[self.selected_idx]
        z  = self.zoom
        bw = g["bmp_width"]
        bh = g["bmp_height"]

        cw = self.canvas.winfo_width()  or 400
        ch = self.canvas.winfo_height() or 400

        # Center the glyph
        ox = (cw - bw * z) // 2
        oy = (ch - bh * z) // 2

        # Grid background
        if self.show_grid.get():
            for row in range(bh):
                for col in range(bw):
                    x0 = ox + col * z
                    y0 = oy + row * z
                    self.canvas.create_rectangle(
                        x0, y0, x0 + z, y0 + z,
                        fill=PIXEL_OFF, outline=BG, width=1)

        # Pixels
        bitmap = g["bitmap"]
        for row in range(bh):
            for col in range(bw):
                if row < len(bitmap) and col < len(bitmap[row]) and bitmap[row][col]:
                    x0 = ox + col * z
                    y0 = oy + row * z
                    self.canvas.create_rectangle(
                        x0, y0, x0 + z, y0 + z,
                        fill=PIXEL_ON, outline="", width=0)

        # Baseline line
        if self.show_baseline.get() and self.font_data:
            baseline = self.font_data["baseline"]
            by = oy + baseline * z
            self.canvas.create_line(
                ox - 8, by, ox + bw * z + 8, by,
                fill=BASELINE_COL, width=1, dash=(4, 3))
            self.canvas.create_text(
                ox - 12, by, text="▶", fill=BASELINE_COL,
                font=("Segoe UI", 7), anchor="e")

        # Outer border
        self.canvas.create_rectangle(
            ox - 1, oy - 1,
            ox + bw * z + 1, oy + bh * z + 1,
            outline=BORDER, width=1, fill="")

        # Advance width marker
        dw = g["dev_width"]
        if dw > 0:
            ax = ox + dw * z
            self.canvas.create_line(
                ax, oy - 12, ax, oy + bh * z + 12,
                fill=ACCENT2, width=1, dash=(3, 4))
            self.canvas.create_text(
                ax + 4, oy - 16, text=f"adv={dw}",
                fill=ACCENT2, font=("Consolas", 8), anchor="w")

        # Update info bar
        cp = g["codepoint"]
        try:
            ch = chr(cp)
            if cp < 0x20 or (0x7F <= cp < 0xA0): ch = "—"
        except Exception:
            ch = "—"

        self.glyph_char_lbl.config(text=ch)
        self.meta_cp.config(text=f"Codepoint: U+{cp:04X}  ({cp})  '{ch}'")
        self.meta_sz.config(text=f"Bitmap: {bw} × {bh} px")
        self.meta_dw.config(text=f"Advance width: {dw} px")


# ── Entry point ───────────────────────────────────────────────────────────

if __name__ == "__main__":
    path = sys.argv[1] if len(sys.argv) > 1 else None
    app  = FntViewer(path)
    app.mainloop()