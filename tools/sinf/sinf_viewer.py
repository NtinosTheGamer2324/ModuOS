#!/usr/bin/env python3
"""
SINF Font Viewer
Sibling tool to the SINF Font Editor / FNT Font Editor — same overall
workspace shape (menu bar, left settings panel, big center canvas, right
inspector panel, status bar) but read-only: load a .sinf file, type text,
and watch it render at any size from the same stored sine-wave coefficients.

New in this version, vs. the original single-window viewer:
    - Menu bar (File / View) instead of everything crammed into toolbars
    - A right-hand Glyph Inspector: pick any glyph from the loaded font and
      see it zoomed large, its harmonics as an actual table (frequency,
      magnitude, phase), and its approximate on-disk size in bytes
    - A live "construction" animation (play the rotating sine terms/
      epicycles that sum to the curve) for either the whole rendered text
      or the single inspected glyph
    - PNG export that doesn't depend on Ghostscript (renders through PIL
      directly from the same coefficients, not a canvas screenshot)
    - Adjustable text color / background, baseline toggle, multi-line text

Requires: numpy, tkinter, Pillow.
"""

import sys
import math
import tkinter as tk
from tkinter import ttk, filedialog, messagebox, colorchooser

import numpy as np
from PIL import Image, ImageDraw

import sinf_format as sf

CANVAS_W, CANVAS_H = 760, 420
INSPECTOR_SIZE = 260


class SinfViewerApp:
    def __init__(self, root, path=None):
        self.root = root
        root.title("SINF Font Viewer")
        root.geometry("1400x800")

        self.font = None
        self.text_var = tk.StringVar(value="Sine Fonts\nScale for free")
        self.size_var = tk.DoubleVar(value=90)
        self.samples_var = tk.IntVar(value=100)
        self.show_construction = tk.BooleanVar(value=False)
        self.show_baseline = tk.BooleanVar(value=True)
        self.text_color = "#111111"
        self.bg_color = "#ffffff"

        self.selected_codepoint = None
        self.anim_running = False
        self.anim_t = 0.0
        self.anim_job = None

        self._build_ui()

        if path:
            self._load(path)

    # ------------------------------------------------------------------
    def _build_ui(self):
        menubar = tk.Menu(self.root)
        self.root.config(menu=menubar)

        file_menu = tk.Menu(menubar, tearoff=0)
        menubar.add_cascade(label="File", menu=file_menu)
        file_menu.add_command(label="Load SINF...", command=self.load_dialog, accelerator="Ctrl+O")
        file_menu.add_command(label="Export PNG...", command=self.export_png, accelerator="Ctrl+E")
        file_menu.add_separator()
        file_menu.add_command(label="Exit", command=self.root.quit)

        view_menu = tk.Menu(menubar, tearoff=0)
        menubar.add_cascade(label="View", menu=view_menu)
        view_menu.add_checkbutton(label="Show construction (epicycles)",
                                   variable=self.show_construction, command=self.redraw)
        view_menu.add_checkbutton(label="Show baseline", variable=self.show_baseline,
                                   command=self.redraw)

        self.root.bind('<Control-o>', lambda e: self.load_dialog())
        self.root.bind('<Control-e>', lambda e: self.export_png())

        main_frame = ttk.Frame(self.root, padding=10)
        main_frame.pack(fill=tk.BOTH, expand=True)

        # ---------------- Left panel: render settings ----------------
        left_panel = ttk.LabelFrame(main_frame, text="Render Settings", padding=10)
        left_panel.pack(side=tk.LEFT, fill=tk.Y, padx=(0, 10))

        ttk.Button(left_panel, text="Load .sinf...", command=self.load_dialog).pack(fill=tk.X)
        self.font_label = ttk.Label(left_panel, text="No font loaded", wraplength=200,
                                     font=('', 9, 'italic'))
        self.font_label.pack(anchor=tk.W, pady=(6, 12))

        ttk.Label(left_panel, text="Text:").pack(anchor=tk.W)
        self.text_box = tk.Text(left_panel, width=24, height=3)
        self.text_box.insert("1.0", self.text_var.get())
        self.text_box.pack(pady=(0, 10))
        self.text_box.bind('<KeyRelease>', self._on_text_change)

        ttk.Label(left_panel, text="Size (px):").pack(anchor=tk.W)
        ttk.Scale(left_panel, from_=8, to=800, variable=self.size_var,
                  orient=tk.HORIZONTAL, command=lambda v: self.redraw()).pack(fill=tk.X)
        self.size_readout = ttk.Label(left_panel, text="90px")
        self.size_readout.pack(anchor=tk.W, pady=(0, 10))

        ttk.Label(left_panel, text="Curve samples/contour:").pack(anchor=tk.W)
        ttk.Scale(left_panel, from_=4, to=400, variable=self.samples_var,
                  orient=tk.HORIZONTAL, command=lambda v: self.redraw()).pack(fill=tk.X)
        self.samples_readout = ttk.Label(left_panel, text="100 samples")
        self.samples_readout.pack(anchor=tk.W, pady=(0, 10))

        ttk.Separator(left_panel, orient=tk.HORIZONTAL).pack(fill=tk.X, pady=8)

        ttk.Checkbutton(left_panel, text="Show construction (epicycles)",
                         variable=self.show_construction, command=self.redraw).pack(anchor=tk.W)
        ttk.Checkbutton(left_panel, text="Show baseline",
                         variable=self.show_baseline, command=self.redraw).pack(anchor=tk.W)

        anim_frame = ttk.Frame(left_panel)
        anim_frame.pack(fill=tk.X, pady=(6, 10))
        self.anim_button = ttk.Button(anim_frame, text="▶ Animate", command=self.toggle_animation)
        self.anim_button.pack(fill=tk.X)

        ttk.Separator(left_panel, orient=tk.HORIZONTAL).pack(fill=tk.X, pady=8)

        ttk.Button(left_panel, text="Text Color...", command=self.pick_text_color).pack(fill=tk.X, pady=2)
        ttk.Button(left_panel, text="Background Color...", command=self.pick_bg_color).pack(fill=tk.X, pady=2)

        ttk.Separator(left_panel, orient=tk.HORIZONTAL).pack(fill=tk.X, pady=8)
        ttk.Button(left_panel, text="Export PNG...", command=self.export_png).pack(fill=tk.X)

        # ---------------- Center: canvas ----------------
        center_panel = ttk.LabelFrame(main_frame, text="Rendered Text", padding=10)
        center_panel.pack(side=tk.LEFT, fill=tk.BOTH, expand=True, padx=(0, 10))

        self.canvas = tk.Canvas(center_panel, width=CANVAS_W, height=CANVAS_H, bg=self.bg_color,
                                 highlightthickness=1, highlightbackground="#cccccc")
        self.canvas.pack(fill=tk.BOTH, expand=True)

        self.render_stats_var = tk.StringVar(value="")
        ttk.Label(center_panel, textvariable=self.render_stats_var, font=('', 9)).pack(
            anchor=tk.W, pady=(6, 0))

        # ---------------- Right panel: glyph inspector ----------------
        right_panel = ttk.LabelFrame(main_frame, text="Glyph Inspector", padding=10)
        right_panel.pack(side=tk.LEFT, fill=tk.BOTH)
        right_panel.configure(width=300)

        self.glyph_list = tk.Listbox(right_panel, height=12, width=32, exportselection=False)
        self.glyph_list.pack(fill=tk.X)
        self.glyph_list.bind('<<ListboxSelect>>', self.on_glyph_select)

        self.inspector_canvas = tk.Canvas(right_panel, width=INSPECTOR_SIZE, height=INSPECTOR_SIZE,
                                           bg='white', highlightthickness=1,
                                           highlightbackground="#cccccc")
        self.inspector_canvas.pack(pady=10)

        self.inspector_label = ttk.Label(right_panel, text="Select a glyph", font=('', 9))
        self.inspector_label.pack(anchor=tk.W)

        ttk.Label(right_panel, text="Top harmonics (this glyph):", font=('', 9, 'bold')).pack(
            anchor=tk.W, pady=(10, 2))
        columns = ("freq", "mag", "phase")
        self.harmonics_table = ttk.Treeview(right_panel, columns=columns, show="headings", height=8)
        self.harmonics_table.heading("freq", text="Freq (k)")
        self.harmonics_table.heading("mag", text="Magnitude")
        self.harmonics_table.heading("phase", text="Phase (°)")
        for c in columns:
            self.harmonics_table.column(c, width=90, anchor=tk.CENTER)
        self.harmonics_table.pack(fill=tk.X)

        # ---------------- Status bar ----------------
        self.status_var = tk.StringVar(value="Load a .sinf file to begin.")
        ttk.Label(self.root, textvariable=self.status_var, relief=tk.SUNKEN, anchor=tk.W).pack(
            side=tk.BOTTOM, fill=tk.X)

    # ------------------------------------------------------------------
    def _on_text_change(self, event=None):
        self.text_var.set(self.text_box.get("1.0", "end-1c"))
        self.redraw()

    def pick_text_color(self):
        color = colorchooser.askcolor(color=self.text_color)[1]
        if color:
            self.text_color = color
            self.redraw()

    def pick_bg_color(self):
        color = colorchooser.askcolor(color=self.bg_color)[1]
        if color:
            self.bg_color = color
            self.canvas.configure(bg=color)
            self.redraw()

    # ------------------------------------------------------------------
    def load_dialog(self):
        path = filedialog.askopenfilename(filetypes=[("SINF font", "*.sinf")])
        if path:
            self._load(path)

    def _load(self, path):
        try:
            self.font = sf.load(path)
        except Exception as e:
            messagebox.showerror("SINF Viewer", f"Failed to load: {e}")
            return

        total_bytes = sum(sf.glyph_storage_bytes(g) for g in self.font.glyphs.values())
        self.font_label.config(
            text=f"{self.font.name}\n{len(self.font.glyphs)} glyphs, "
                 f"{self.font.units_per_em} units/em\n"
                 f"~{total_bytes:,} bytes total glyph data")

        self.glyph_list.delete(0, tk.END)
        for cp in sorted(self.font.glyphs):
            g = self.font.glyphs[cp]
            ch = chr(cp) if cp < 0x110000 and cp >= 32 else '\u25a1'
            size = sf.glyph_storage_bytes(g)
            self.glyph_list.insert(tk.END, f"U+{cp:04X} '{ch}'   ~{size}B   {len(g.contours)}c")

        self.status_var.set(f"Loaded {path}")
        self.redraw()

    # ------------------------------------------------------------------
    def redraw(self, *_):
        if self.anim_running:
            t = self.anim_t
        else:
            t = None
        self.size_readout.config(text=f"{int(self.size_var.get())}px")
        self.samples_readout.config(text=f"{int(self.samples_var.get())} samples")

        self.canvas.delete("all")
        if not self.font:
            self.status_var.set("Load a .sinf file to begin.")
            return

        text = self.text_var.get()
        px_size = self.size_var.get()
        samples = max(4, int(self.samples_var.get()))

        items, total_width = sf.layout_text(self.font, text, px_size, samples)

        origin_x = 16
        baseline_y = px_size * 1.1
        total_harmonics = 0
        missing = []

        for it in items:
            if it.get('newline'):
                continue
            x = origin_x + it['x']
            y = baseline_y + it['y_offset']

            if it['missing']:
                missing.append(it['char'])
                self.canvas.create_rectangle(x, y - px_size, x + px_size * 0.5, y,
                                              outline="#cc4444", dash=(3, 2))
                continue

            for pts, is_hole in it['polygons']:
                flat = []
                for fx, fy in pts:
                    flat.extend([x + fx, y - fy])
                if len(flat) >= 6:
                    color = self.bg_color if is_hole else self.text_color
                    self.canvas.create_polygon(*flat, fill=color, outline="")

            if self.show_construction.get():
                glyph = self.font.get_glyph(ord(it['char']))
                if glyph is not None:
                    t_anim = t if t is not None else 0.3
                    for contour in glyph.contours:
                        total_harmonics += len(contour.harmonics)
                        self._draw_epicycles(contour, x, y, px_size / self.font.units_per_em, t_anim)

        if self.show_baseline.get():
            for line in set(it.get('y_offset', 0) for it in items if not it.get('newline')):
                by = baseline_y + line
                self.canvas.create_line(0, by, CANVAS_W, by, fill="#ff8888", dash=(4, 2))

        note = ""
        if missing:
            note = f"  Missing: {''.join(missing)}"
        self.render_stats_var.set(
            f"{px_size:.0f}px  •  {samples} samples/contour  •  "
            f"total advance {total_width:.0f}u{note}")

    def _draw_epicycles(self, contour, origin_x, baseline_y, scale, t):
        cx, cy = 0.0, 0.0
        harmonics_sorted = sorted(contour.harmonics, key=lambda h: -abs(complex(h.re, h.im)))
        for h in harmonics_sorted:
            c = complex(h.re, h.im)
            vec = c * np.exp(1j * 2 * np.pi * h.freq * t)
            nx, ny = cx + vec.real, cy + vec.imag
            r = abs(c) * scale
            px0, py0 = origin_x + cx * scale, baseline_y - cy * scale
            if r > 0.5:
                self.canvas.create_oval(px0 - r, py0 - r, px0 + r, py0 + r, outline="#aac", width=1)
            px1, py1 = origin_x + nx * scale, baseline_y - ny * scale
            self.canvas.create_line(px0, py0, px1, py1, fill="#88a", width=1)
            cx, cy = nx, ny

    # ------------------------------------------------------------------
    def toggle_animation(self):
        if self.anim_running:
            self.anim_running = False
            self.anim_button.config(text="▶ Animate")
            if self.anim_job:
                self.root.after_cancel(self.anim_job)
                self.anim_job = None
        else:
            if not self.show_construction.get():
                self.show_construction.set(True)
            self.anim_running = True
            self.anim_button.config(text="■ Stop")
            self._animate_step()

    def _animate_step(self):
        if not self.anim_running:
            return
        self.anim_t = (self.anim_t + 0.006) % 1.0
        self.redraw()
        self.anim_job = self.root.after(30, self._animate_step)

    # ------------------------------------------------------------------
    def on_glyph_select(self, event):
        sel = self.glyph_list.curselection()
        if not sel or not self.font:
            return
        cp = sorted(self.font.glyphs)[sel[0]]
        self.selected_codepoint = cp
        glyph = self.font.glyphs[cp]

        ch = chr(cp) if cp < 0x110000 and cp >= 32 else '\u25a1'
        size_bytes = sf.glyph_storage_bytes(glyph)
        self.inspector_label.config(
            text=f"U+{cp:04X}  '{ch}'\ncontours: {len(glyph.contours)}   "
                 f"advance: {glyph.advance_width}u\n~{size_bytes} bytes on disk")

        self._draw_inspector(glyph)
        self._populate_harmonics_table(glyph)

    def _draw_inspector(self, glyph):
        self.inspector_canvas.delete("all")
        upm = self.font.units_per_em
        pad = 20
        scale = (INSPECTOR_SIZE - 2 * pad) / upm
        origin_x = INSPECTOR_SIZE / 2
        baseline_y = INSPECTOR_SIZE - pad

        contours = [c.evaluate(150) for c in glyph.contours]
        order = sorted(range(len(contours)), key=lambda i: -sf.polygon_area(contours[i]))
        # center horizontally using bounding box of the outer contour
        all_x = np.concatenate([c[:, 0] for c in contours]) if contours else np.array([0])
        mid_x = (all_x.min() + all_x.max()) / 2 if len(all_x) else 0

        for rank, i in enumerate(order):
            pts = contours[i]
            flat = []
            for fx, fy in pts:
                cx = origin_x + (fx - mid_x) * scale
                cy = baseline_y - fy * scale
                flat.extend([cx, cy])
            if len(flat) >= 6:
                color = 'white' if rank > 0 else self.text_color
                self.inspector_canvas.create_polygon(*flat, fill=color, outline="")

        self.inspector_canvas.create_line(0, baseline_y, INSPECTOR_SIZE, baseline_y,
                                           fill="#ff8888", dash=(3, 2))

    def _populate_harmonics_table(self, glyph):
        for row in self.harmonics_table.get_children():
            self.harmonics_table.delete(row)
        all_harmonics = []
        for contour in glyph.contours:
            all_harmonics.extend(contour.harmonics)
        all_harmonics.sort(key=lambda h: -abs(complex(h.re, h.im)))
        for h in all_harmonics[:12]:
            mag = abs(complex(h.re, h.im))
            phase_deg = math.degrees(math.atan2(h.im, h.re))
            self.harmonics_table.insert("", tk.END, values=(h.freq, f"{mag:.1f}", f"{phase_deg:.0f}"))

    # ------------------------------------------------------------------
    def export_png(self):
        if not self.font:
            messagebox.showinfo("SINF Viewer", "Load a font first.")
            return
        path = filedialog.asksaveasfilename(defaultextension=".png",
                                             filetypes=[("PNG image", "*.png")])
        if not path:
            return

        text = self.text_var.get()
        px_size = self.size_var.get()
        samples = max(4, int(self.samples_var.get()))
        items, total_width = sf.layout_text(self.font, text, px_size, samples)

        num_lines = 1 + sum(1 for it in items if it.get('newline'))
        img_w = int(max(total_width, 100)) + 40
        img_h = int(px_size * 1.5 * num_lines) + 40
        img = Image.new('RGB', (img_w, img_h), self.bg_color)
        draw = ImageDraw.Draw(img)

        origin_x = 16
        baseline_y = px_size * 1.1
        for it in items:
            if it.get('newline') or it['missing']:
                continue
            x = origin_x + it['x']
            y = baseline_y + it['y_offset']
            for pts, is_hole in it['polygons']:
                poly = [(x + fx, y - fy) for fx, fy in pts]
                if len(poly) >= 3:
                    draw.polygon(poly, fill=(self.bg_color if is_hole else self.text_color))

        img.save(path)
        self.status_var.set(f"Exported PNG: {path}")
        messagebox.showinfo("SINF Viewer", f"Saved {img_w}x{img_h} PNG to\n{path}")


def main():
    root = tk.Tk()
    path = sys.argv[1] if len(sys.argv) > 1 else None
    SinfViewerApp(root, path)
    root.mainloop()


if __name__ == "__main__":
    main()