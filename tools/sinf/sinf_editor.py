#!/usr/bin/env python3
"""
SINF Font Editor
Sibling tool to the FNT Font Editor, adapted for SINF's sine-wave/Fourier
contour glyphs instead of pixel bitmaps. Same overall workspace layout so
the two editors feel like the same family of tools:
    - Menu bar with File / Edit / View
    - Left panel: font properties
    - Center panel: glyph editor (draw contours by clicking, with a faded
      reference character to trace over, just like the FNT editor's
      reference-glyph overlay)
    - Right panel: quick Unicode range sets + live text preview
    - Status bar

Where FNT paints pixels, SINF places contour points; where FNT stores a
bitmap, SINF runs a DFT over the points and keeps the top-K sine-wave
coefficients when you save the font.
"""

import tkinter as tk
from tkinter import ttk, messagebox, filedialog
from PIL import Image, ImageDraw, ImageFont, ImageTk

import numpy as np
import sinf_format as sf


def polygon_area(points):
    """Shoelace formula — used to tell outer contours from holes."""
    pts = np.asarray(points, dtype=float)
    if len(pts) < 3:
        return 0.0
    x, y = pts[:, 0], pts[:, 1]
    return 0.5 * abs(np.dot(x, np.roll(y, 1)) - np.dot(y, np.roll(x, 1)))


class ContourEditor(tk.Canvas):
    """Vector glyph editor widget with a faded reference-character overlay,
    the SINF analogue of the FNT editor's pixel-grid GlyphEditor."""

    def __init__(self, parent, canvas_size=440, units_per_em=1000):
        self.canvas_size = canvas_size
        self.units_per_em = units_per_em

        super().__init__(parent, width=canvas_size, height=canvas_size,
                          bg='white', highlightthickness=1, highlightbackground='#cccccc')

        self.contours = []          # finished contours, in FONT UNITS: list[list[(x,y)]]
        self.current_points = []    # in-progress contour, in CANVAS coords
        self.point_ids = []         # canvas item ids for the in-progress contour

        self.baseline_frac = 0.8
        self.current_char = 'A'
        self.show_reference = tk.BooleanVar(value=True)
        self.reference_opacity = 0.15
        self.reference_image = None

        self.bind('<Button-1>', self.on_click)
        self.bind('<Button-3>', lambda e: self.close_contour())  # right-click = close contour

        self.draw_grid()
        self.redraw()

    # ------------------------------------------------------------------
    # Coordinate mapping (canvas pixels <-> font units, y flipped so font
    # coordinates grow upward like a normal font em-square)
    def canvas_to_font(self, cx, cy):
        scale = self.units_per_em / self.canvas_size
        return cx * scale, (self.canvas_size - cy) * scale

    def font_to_canvas(self, fx, fy):
        scale = self.canvas_size / self.units_per_em
        return fx * scale, self.canvas_size - fy * scale

    # ------------------------------------------------------------------
    def draw_grid(self):
        step = self.canvas_size / 20
        for i in range(21):
            x = i * step
            color = '#cccccc' if i % 5 != 0 else '#999999'
            self.create_line(x, 0, x, self.canvas_size, fill=color, tags='grid')
        for i in range(21):
            y = i * step
            color = '#cccccc' if i % 5 != 0 else '#999999'
            self.create_line(0, y, self.canvas_size, y, fill=color, tags='grid')
        self.tag_lower('grid')

    def draw_reference_character(self):
        """Faded reference character in the background, same trick as the
        FNT editor: render with PIL, fade it, drop it behind the grid."""
        if not self.show_reference.get():
            return
        try:
            size = self.canvas_size
            img = Image.new('L', (size, size), 255)
            draw = ImageDraw.Draw(img)

            font_size = int(size * 0.62)
            font = None
            for candidate in ("DejaVuSans.ttf", "Arial.ttf"):
                try:
                    font = ImageFont.truetype(candidate, font_size)
                    break
                except Exception:
                    continue
            if font is None:
                font = ImageFont.load_default()

            bbox = draw.textbbox((0, 0), self.current_char, font=font)
            text_w = bbox[2] - bbox[0]
            text_h = bbox[3] - bbox[1]
            x = (size - text_w) // 2 - bbox[0]
            y = (size - text_h) // 2 - bbox[1]
            draw.text((x, y), self.current_char, fill=0, font=font)

            img = Image.eval(img, lambda v: 255 - v)
            img = Image.eval(img, lambda v: int(v * self.reference_opacity) if v > 0 else 0)
            img = Image.eval(img, lambda v: 255 - v)

            self.reference_image = ImageTk.PhotoImage(img)
            self.create_image(0, 0, anchor=tk.NW, image=self.reference_image, tags='reference')
            self.tag_lower('reference')
            self.tag_lower('grid')
        except Exception:
            pass  # missing fonts etc. shouldn't block the editor

    # ------------------------------------------------------------------
    def on_click(self, event):
        self.current_points.append((event.x, event.y))
        r = 3
        pid = self.create_oval(event.x - r, event.y - r, event.x + r, event.y + r,
                                fill='#2266cc', outline='', tags='live')
        self.point_ids.append(pid)
        if len(self.current_points) > 1:
            x0, y0 = self.current_points[-2]
            x1, y1 = self.current_points[-1]
            lid = self.create_line(x0, y0, x1, y1, fill='#2266cc', width=2, tags='live')
            self.point_ids.append(lid)

    def undo_point(self):
        if not self.current_points:
            return
        self.current_points.pop()
        for _ in range(2 if self.point_ids and len(self.point_ids) >= 2 and self.current_points else 1):
            if self.point_ids:
                self.delete(self.point_ids.pop())

    def close_contour(self):
        if len(self.current_points) < 3:
            return
        x0, y0 = self.current_points[0]
        x1, y1 = self.current_points[-1]
        self.create_line(x1, y1, x0, y0, fill='#2266cc', width=2, dash=(3, 2), tags='live')
        font_pts = [self.canvas_to_font(px, py) for (px, py) in self.current_points]
        self.contours.append(font_pts)
        self.current_points = []
        self.point_ids = []
        self.redraw()

    # ------------------------------------------------------------------
    def set_current_char(self, char):
        self.current_char = char
        self.redraw()

    def set_show_reference(self, show):
        self.show_reference.set(show)
        self.redraw()

    def set_baseline(self, frac):
        self.baseline_frac = frac
        self.redraw()

    # ------------------------------------------------------------------
    def redraw(self):
        self.delete('contour')
        self.delete('baseline')
        self.delete('reference')

        self.draw_reference_character()

        for contour in self.contours:
            canvas_pts = []
            for fx, fy in contour:
                cx, cy = self.font_to_canvas(fx, fy)
                canvas_pts.extend([cx, cy])
            if len(canvas_pts) >= 6:
                self.create_polygon(*canvas_pts, outline='#2a8f4f', fill='', width=2,
                                     tags='contour')

        baseline_y = self.canvas_size * self.baseline_frac
        self.create_line(0, baseline_y, self.canvas_size, baseline_y,
                          fill='red', width=2, tags='baseline')

        self.tag_raise('live')

    def clear(self):
        self.delete('live')
        self.contours = []
        self.current_points = []
        self.point_ids = []
        self.redraw()

    def load_contours(self, contours_font_units):
        """contours_font_units: list[list[(fx, fy)]]"""
        self.clear()
        self.contours = [list(c) for c in contours_font_units]
        self.redraw()

    def get_contours(self):
        """Finished contours plus any in-progress one, auto-closed."""
        contours = [list(c) for c in self.contours]
        if len(self.current_points) >= 3:
            contours.append([self.canvas_to_font(px, py) for (px, py) in self.current_points])
        return contours


class SinfFontEditorApp:
    """Main SINF font editor application — layout mirrors the FNT editor."""

    def __init__(self, root):
        self.root = root
        self.root.title("SINF Font Editor")
        self.root.geometry("1400x800")

        self.font_name = "New Sine Font"
        self.units_per_em = sf.DEFAULT_UNITS_PER_EM
        self.harmonics_per_contour = sf.DEFAULT_HARMONICS
        self.dft_samples = 256

        # codepoint -> {'contours': [[(fx,fy),...], ...], 'advance_width': int}
        self.glyphs = {}
        self.current_codepoint = ord('A')
        self.current_file = None
        self.clipboard_contours = None

        self.setup_ui()
        self.update_glyph_editor()

    # ------------------------------------------------------------------
    def setup_ui(self):
        menubar = tk.Menu(self.root)
        self.root.config(menu=menubar)

        file_menu = tk.Menu(menubar, tearoff=0)
        menubar.add_cascade(label="File", menu=file_menu)
        file_menu.add_command(label="New Font", command=self.new_font, accelerator="Ctrl+N")
        file_menu.add_command(label="Open SINF...", command=self.open_font, accelerator="Ctrl+O")
        file_menu.add_command(label="Save SINF", command=self.save_font, accelerator="Ctrl+S")
        file_menu.add_command(label="Save SINF As...", command=self.save_font_as, accelerator="Ctrl+Shift+S")
        file_menu.add_separator()
        file_menu.add_command(label="Export Preview Image...", command=self.export_preview)
        file_menu.add_separator()
        file_menu.add_command(label="Exit", command=self.root.quit)

        edit_menu = tk.Menu(menubar, tearoff=0)
        menubar.add_cascade(label="Edit", menu=edit_menu)
        edit_menu.add_command(label="Close Contour", command=self.close_contour, accelerator="Ctrl+Return")
        edit_menu.add_command(label="Undo Point", command=self.undo_point, accelerator="Ctrl+Z")
        edit_menu.add_command(label="Clear Glyph", command=self.clear_glyph, accelerator="Ctrl+D")
        edit_menu.add_command(label="Copy Glyph", command=self.copy_glyph, accelerator="Ctrl+C")
        edit_menu.add_command(label="Paste Glyph", command=self.paste_glyph, accelerator="Ctrl+V")

        view_menu = tk.Menu(menubar, tearoff=0)
        menubar.add_cascade(label="View", menu=view_menu)
        self.show_reference_var = tk.BooleanVar(value=True)
        view_menu.add_checkbutton(label="Show reference character", variable=self.show_reference_var,
                                   command=self.toggle_reference)

        self.root.bind('<Control-n>', lambda e: self.new_font())
        self.root.bind('<Control-o>', lambda e: self.open_font())
        self.root.bind('<Control-s>', lambda e: self.save_font())
        self.root.bind('<Control-Shift-S>', lambda e: self.save_font_as())
        self.root.bind('<Control-d>', lambda e: self.clear_glyph())
        self.root.bind('<Control-c>', lambda e: self.copy_glyph())
        self.root.bind('<Control-v>', lambda e: self.paste_glyph())
        self.root.bind('<Control-z>', lambda e: self.undo_point())
        self.root.bind('<Control-Return>', lambda e: self.close_contour())

        main_frame = ttk.Frame(self.root, padding=10)
        main_frame.pack(fill=tk.BOTH, expand=True)

        # ---------------- Left panel: font properties ----------------
        left_panel = ttk.LabelFrame(main_frame, text="Font Properties", padding=10)
        left_panel.pack(side=tk.LEFT, fill=tk.Y, padx=(0, 10))

        ttk.Label(left_panel, text="Font Name:").pack(anchor=tk.W)
        self.font_name_entry = ttk.Entry(left_panel, width=25)
        self.font_name_entry.insert(0, self.font_name)
        self.font_name_entry.pack(fill=tk.X, pady=(0, 10))
        self.font_name_entry.bind('<KeyRelease>', lambda e: self.update_font_name())

        ttk.Label(left_panel, text="Units per Em:").pack(anchor=tk.W, pady=(10, 0))
        self.upm_spinbox = ttk.Spinbox(left_panel, from_=100, to=4000, increment=100, width=8)
        self.upm_spinbox.set(self.units_per_em)
        self.upm_spinbox.pack(anchor=tk.W, pady=5)

        ttk.Label(left_panel, text="Harmonics / Contour:").pack(anchor=tk.W)
        self.harmonics_spinbox = ttk.Spinbox(left_panel, from_=2, to=200, width=8)
        self.harmonics_spinbox.set(self.harmonics_per_contour)
        self.harmonics_spinbox.pack(anchor=tk.W, pady=5)

        ttk.Button(left_panel, text="Apply Settings",
                   command=self.apply_settings).pack(fill=tk.X, pady=5)

        ttk.Separator(left_panel, orient=tk.HORIZONTAL).pack(fill=tk.X, pady=10)

        ttk.Label(left_panel, text="Advance Width (current glyph):").pack(anchor=tk.W)
        self.advance_var = tk.IntVar(value=700)
        ttk.Entry(left_panel, textvariable=self.advance_var, width=10).pack(anchor=tk.W, pady=5)

        ttk.Separator(left_panel, orient=tk.HORIZONTAL).pack(fill=tk.X, pady=10)

        ttk.Label(left_panel, text="Baseline (visual guide):").pack(anchor=tk.W)
        self.baseline_scale = ttk.Scale(left_panel, from_=50, to=95,
                                         orient=tk.HORIZONTAL, command=self.update_baseline)
        self.baseline_scale.set(80)
        self.baseline_scale.pack(fill=tk.X, pady=5)
        self.baseline_label = ttk.Label(left_panel, text="Position: 80%")
        self.baseline_label.pack(anchor=tk.W)

        ttk.Separator(left_panel, orient=tk.HORIZONTAL).pack(fill=tk.X, pady=10)

        ttk.Label(left_panel, text="Reference Guide:").pack(anchor=tk.W, pady=(10, 5))
        ttk.Checkbutton(left_panel, text="Show reference character",
                         variable=self.show_reference_var,
                         command=self.toggle_reference).pack(anchor=tk.W)

        ttk.Separator(left_panel, orient=tk.HORIZONTAL).pack(fill=tk.X, pady=10)

        ttk.Label(left_panel, text="Font Statistics:").pack(anchor=tk.W, pady=(10, 5))
        self.stats_label = ttk.Label(left_panel, text="Glyphs: 0", font=('', 9))
        self.stats_label.pack(anchor=tk.W)

        ttk.Label(left_panel, text="Click: add point\nRight-click: close contour",
                  font=('', 8), foreground='#666').pack(anchor=tk.W, pady=(15, 0))

        # ---------------- Center panel: glyph editor ----------------
        center_panel = ttk.LabelFrame(main_frame, text="Glyph Editor (contours)", padding=10)
        center_panel.pack(side=tk.LEFT, fill=tk.BOTH, expand=True, padx=(0, 10))

        char_frame = ttk.Frame(center_panel)
        char_frame.pack(fill=tk.X, pady=(0, 10))

        ttk.Label(char_frame, text="Character:").pack(side=tk.LEFT)
        self.char_entry = ttk.Entry(char_frame, width=5)
        self.char_entry.insert(0, chr(self.current_codepoint))
        self.char_entry.pack(side=tk.LEFT, padx=5)
        self.char_entry.bind('<Return>', lambda e: self.change_character())
        ttk.Button(char_frame, text="Go", command=self.change_character).pack(side=tk.LEFT)

        ttk.Label(char_frame, text="Unicode:").pack(side=tk.LEFT, padx=(20, 5))
        self.unicode_entry = ttk.Entry(char_frame, width=8)
        self.unicode_entry.insert(0, f"U+{self.current_codepoint:04X}")
        self.unicode_entry.pack(side=tk.LEFT, padx=5)
        self.unicode_entry.bind('<Return>', lambda e: self.change_unicode())
        ttk.Button(char_frame, text="Go", command=self.change_unicode).pack(side=tk.LEFT)

        nav_frame = ttk.Frame(center_panel)
        nav_frame.pack(fill=tk.X, pady=(0, 10))

        ttk.Button(nav_frame, text="◄◄ Prev", command=self.prev_char).pack(side=tk.LEFT, padx=2)
        ttk.Button(nav_frame, text="Next ►►", command=self.next_char).pack(side=tk.LEFT, padx=2)
        ttk.Separator(nav_frame, orient=tk.VERTICAL).pack(side=tk.LEFT, fill=tk.Y, padx=10)
        ttk.Button(nav_frame, text="Close Contour", command=self.close_contour).pack(side=tk.LEFT, padx=2)
        ttk.Button(nav_frame, text="Undo Point", command=self.undo_point).pack(side=tk.LEFT, padx=2)
        ttk.Separator(nav_frame, orient=tk.VERTICAL).pack(side=tk.LEFT, fill=tk.Y, padx=10)
        ttk.Button(nav_frame, text="Clear", command=self.clear_glyph).pack(side=tk.LEFT, padx=2)
        ttk.Button(nav_frame, text="Copy", command=self.copy_glyph).pack(side=tk.LEFT, padx=2)
        ttk.Button(nav_frame, text="Paste", command=self.paste_glyph).pack(side=tk.LEFT, padx=2)

        editor_container = ttk.Frame(center_panel)
        editor_container.pack(fill=tk.BOTH, expand=True)
        self.glyph_editor = ContourEditor(editor_container, canvas_size=440,
                                           units_per_em=self.units_per_em)
        self.glyph_editor.pack()

        # ---------------- Right panel: quick sets + preview ----------------
        right_panel = ttk.Frame(main_frame)
        right_panel.pack(side=tk.LEFT, fill=tk.BOTH)

        sets_frame = ttk.LabelFrame(right_panel, text="Quick Sets", padding=10)
        sets_frame.pack(fill=tk.BOTH, expand=True, pady=(0, 10))

        sets_canvas = tk.Canvas(sets_frame, height=400, width=180)
        sets_scrollbar = ttk.Scrollbar(sets_frame, orient="vertical", command=sets_canvas.yview)
        sets_scrollable = ttk.Frame(sets_canvas)
        sets_scrollable.bind("<Configure>",
                              lambda e: sets_canvas.configure(scrollregion=sets_canvas.bbox("all")))
        sets_canvas.create_window((0, 0), window=sets_scrollable, anchor="nw")
        sets_canvas.configure(yscrollcommand=sets_scrollbar.set)
        sets_canvas.pack(side="left", fill="both", expand=True)
        sets_scrollbar.pack(side="right", fill="y")

        def add_set(label, rng):
            ttk.Button(sets_scrollable, text=label, width=15,
                       command=lambda: self.load_character_set(rng)).pack(pady=1)

        ttk.Label(sets_scrollable, text="Basic Latin:", font=('', 9, 'bold')).pack(anchor=tk.W, pady=(5, 2))
        add_set("A-Z", range(ord('A'), ord('Z') + 1))
        add_set("a-z", range(ord('a'), ord('z') + 1))
        add_set("0-9", range(ord('0'), ord('9') + 1))
        add_set("Punctuation", list(range(33, 48)) + list(range(58, 65)) +
                list(range(91, 97)) + list(range(123, 127)))
        add_set("ASCII (32-126)", range(32, 127))
        add_set("Extended ASCII", range(128, 256))

        ttk.Label(sets_scrollable, text="Greek:", font=('', 9, 'bold')).pack(anchor=tk.W, pady=(10, 2))
        add_set("Greek Uppercase", range(0x0391, 0x03A9 + 1))
        add_set("Greek Lowercase", range(0x03B1, 0x03C9 + 1))
        add_set("Greek Full", range(0x0370, 0x03FF + 1))

        ttk.Label(sets_scrollable, text="Cyrillic:", font=('', 9, 'bold')).pack(anchor=tk.W, pady=(10, 2))
        add_set("Cyrillic Uppercase", range(0x0410, 0x042F + 1))
        add_set("Cyrillic Lowercase", range(0x0430, 0x044F + 1))
        add_set("Cyrillic Full", range(0x0400, 0x04FF + 1))

        ttk.Label(sets_scrollable, text="Extended Latin:", font=('', 9, 'bold')).pack(anchor=tk.W, pady=(10, 2))
        add_set("Latin-1 Suppl.", range(0x00A0, 0x00FF + 1))
        add_set("Latin Extended-A", range(0x0100, 0x017F + 1))
        add_set("Latin Extended-B", range(0x0180, 0x024F + 1))

        ttk.Label(sets_scrollable, text="Symbols:", font=('', 9, 'bold')).pack(anchor=tk.W, pady=(10, 2))
        add_set("Math Operators", range(0x2200, 0x22FF + 1))
        add_set("Arrows", range(0x2190, 0x21FF + 1))
        add_set("Box Drawing", range(0x2500, 0x257F + 1))
        add_set("Geometric Shapes", range(0x25A0, 0x25FF + 1))

        # ---------------- Preview ----------------
        preview_frame = ttk.LabelFrame(right_panel, text="Preview", padding=10)
        preview_frame.pack(fill=tk.BOTH, expand=True)

        ttk.Label(preview_frame, text="Sample Text:").pack(anchor=tk.W)
        self.preview_text = ttk.Entry(preview_frame)
        self.preview_text.insert(0, "Sine")
        self.preview_text.pack(fill=tk.X, pady=(0, 10))
        self.preview_text.bind('<KeyRelease>', lambda e: self.update_preview())

        preview_canvas_frame = ttk.Frame(preview_frame, relief=tk.SUNKEN, borderwidth=1)
        preview_canvas_frame.pack(fill=tk.BOTH, expand=True)
        self.preview_canvas = tk.Canvas(preview_canvas_frame, bg='white', height=100)
        self.preview_canvas.pack(fill=tk.BOTH, expand=True)

        ttk.Button(preview_frame, text="Update Preview",
                   command=self.update_preview).pack(fill=tk.X, pady=(10, 0))
        ttk.Label(preview_frame, text="(preview uses your drawn points directly;\n"
                                       "Save applies the sine-wave compression)",
                  font=('', 8), foreground='#666').pack(anchor=tk.W, pady=(4, 0))

        # ---------------- Status bar ----------------
        self.status_var = tk.StringVar(value="Ready")
        status_bar = ttk.Label(self.root, textvariable=self.status_var,
                                relief=tk.SUNKEN, anchor=tk.W)
        status_bar.pack(side=tk.BOTTOM, fill=tk.X)

    # ------------------------------------------------------------------
    def toggle_reference(self):
        self.glyph_editor.set_show_reference(self.show_reference_var.get())

    def update_font_name(self):
        self.font_name = self.font_name_entry.get()

    def apply_settings(self):
        try:
            new_upm = int(self.upm_spinbox.get())
            new_harm = int(self.harmonics_spinbox.get())
            if new_upm < 100 or new_harm < 2:
                messagebox.showerror("Error", "Units-per-em must be >=100, harmonics >=2")
                return

            if new_upm != self.units_per_em and self.glyphs:
                if not messagebox.askyesno(
                        "Confirm", "Changing units-per-em rescales all existing glyphs. Continue?"):
                    return
                scale = new_upm / self.units_per_em
                for g in self.glyphs.values():
                    g['contours'] = [[(x * scale, y * scale) for (x, y) in c] for c in g['contours']]

            self.units_per_em = new_upm
            self.harmonics_per_contour = new_harm

            self.glyph_editor.destroy()
            editor_container = self.glyph_editor.master
            self.glyph_editor = ContourEditor(editor_container, canvas_size=440,
                                               units_per_em=self.units_per_em)
            self.glyph_editor.pack()
            self.update_baseline(self.baseline_scale.get())
            self.update_glyph_editor()
            self.status_var.set("Settings applied")
        except ValueError:
            messagebox.showerror("Error", "Invalid settings")

    def update_baseline(self, value):
        frac = float(value) / 100.0
        self.baseline_label.config(text=f"Position: {int(float(value))}%")
        self.glyph_editor.set_baseline(frac)

    # ------------------------------------------------------------------
    def save_current_glyph(self):
        contours = self.glyph_editor.get_contours()
        if not contours:
            return
        self.glyphs[self.current_codepoint] = {
            'contours': contours,
            'advance_width': int(self.advance_var.get()),
        }
        self.update_stats()

    def update_glyph_editor(self):
        if self.current_codepoint in self.glyphs:
            g = self.glyphs[self.current_codepoint]
            self.glyph_editor.load_contours(g['contours'])
            self.advance_var.set(g['advance_width'])
        else:
            self.glyph_editor.clear()
            self.advance_var.set(700)

        try:
            char = chr(self.current_codepoint)
            self.glyph_editor.set_current_char(
                char if self.current_codepoint >= 32 and self.current_codepoint != 127 else '\u25a1')
        except Exception:
            self.glyph_editor.set_current_char('?')

        self.char_entry.delete(0, tk.END)
        try:
            char = chr(self.current_codepoint)
            if self.current_codepoint >= 32 and self.current_codepoint != 127 and not (128 <= self.current_codepoint <= 159):
                self.char_entry.insert(0, char)
            else:
                self.char_entry.insert(0, f"[{self.current_codepoint}]")
        except Exception:
            self.char_entry.insert(0, "?")

        self.unicode_entry.delete(0, tk.END)
        self.unicode_entry.insert(0, f"U+{self.current_codepoint:04X}")

        glyph_exists = "Exists" if self.current_codepoint in self.glyphs else "New"
        self.status_var.set(f"Editing U+{self.current_codepoint:04X} - {glyph_exists}")

    # ------------------------------------------------------------------
    def change_character(self):
        self.save_current_glyph()
        char = self.char_entry.get()
        if char:
            self.current_codepoint = ord(char[0])
            self.update_glyph_editor()

    def change_unicode(self):
        self.save_current_glyph()
        unicode_str = self.unicode_entry.get().strip()
        try:
            if unicode_str.lower().startswith('u+'):
                unicode_str = unicode_str[2:]
            self.current_codepoint = int(unicode_str, 16)
            self.update_glyph_editor()
        except ValueError:
            messagebox.showerror("Error", "Invalid Unicode value")

    def prev_char(self):
        self.save_current_glyph()
        self.current_codepoint = max(0, self.current_codepoint - 1)
        self.update_glyph_editor()

    def next_char(self):
        self.save_current_glyph()
        self.current_codepoint = min(0x10FFFF, self.current_codepoint + 1)
        self.update_glyph_editor()

    def close_contour(self):
        self.glyph_editor.close_contour()

    def undo_point(self):
        self.glyph_editor.undo_point()

    def clear_glyph(self):
        self.glyph_editor.clear()
        if self.current_codepoint in self.glyphs:
            del self.glyphs[self.current_codepoint]
            self.update_stats()
        self.status_var.set("Glyph cleared")

    def copy_glyph(self):
        self.clipboard_contours = self.glyph_editor.get_contours()
        self.status_var.set("Glyph copied")

    def paste_glyph(self):
        if self.clipboard_contours:
            self.glyph_editor.load_contours(self.clipboard_contours)
            self.status_var.set("Glyph pasted")
        else:
            messagebox.showinfo("Info", "No glyph in clipboard")

    def load_character_set(self, char_range):
        self.save_current_glyph()
        char_list = list(char_range)
        self.current_codepoint = char_list[0]
        self.update_glyph_editor()
        messagebox.showinfo("Character Set",
                             f"Loaded character set. Use Next/Prev to navigate.\n"
                             f"Range: U+{min(char_list):04X} to U+{max(char_list):04X}\n"
                             f"Total characters: {len(char_list)}")

    # ------------------------------------------------------------------
    def _draw_glyph_on_canvas(self, canvas, contours, origin_x, baseline_y, scale, fill='black'):
        if not contours:
            return
        ordered = sorted(contours, key=polygon_area, reverse=True)
        for i, contour in enumerate(ordered):
            pts = []
            for fx, fy in contour:
                pts.extend([origin_x + fx * scale, baseline_y - fy * scale])
            if len(pts) >= 6:
                color = fill if i == 0 else 'white'
                canvas.create_polygon(*pts, fill=color, outline='')

    def update_preview(self):
        self.save_current_glyph()
        text = self.preview_text.get()
        self.preview_canvas.delete('all')
        if not text or not self.glyphs:
            return

        px_size = 60
        scale = px_size / self.units_per_em
        pen_x = 10
        baseline_y = 80

        for ch in text:
            cp = ord(ch)
            if cp == ord(' '):
                pen_x += px_size * 0.5
                continue
            g = self.glyphs.get(cp)
            if g is None:
                self.preview_canvas.create_rectangle(pen_x, baseline_y - px_size,
                                                       pen_x + px_size * 0.6, baseline_y,
                                                       outline='red')
                pen_x += px_size * 0.6 + 4
                continue
            self._draw_glyph_on_canvas(self.preview_canvas, g['contours'], pen_x, baseline_y, scale)
            pen_x += g['advance_width'] * scale + 2

    def update_stats(self):
        total_contours = sum(len(g['contours']) for g in self.glyphs.values())
        self.stats_label.config(text=f"Glyphs: {len(self.glyphs)}\nContours: {total_contours}")

    # ------------------------------------------------------------------
    def new_font(self):
        if messagebox.askyesno("New Font", "Create new font? Unsaved changes will be lost."):
            self.font_name = "New Sine Font"
            self.glyphs = {}
            self.current_file = None
            self.font_name_entry.delete(0, tk.END)
            self.font_name_entry.insert(0, self.font_name)
            self.update_glyph_editor()
            self.update_stats()
            self.status_var.set("New font created")

    def open_font(self):
        filename = filedialog.askopenfilename(
            title="Open Font", filetypes=[("SINF Files", "*.sinf"), ("All Files", "*.*")])
        if not filename:
            return
        try:
            loaded = sf.load(filename)
            self.font_name = loaded.name
            self.units_per_em = loaded.units_per_em
            self.glyphs = {}
            # Reconstruct editable point-contours from the stored sine coefficients
            for cp, glyph in loaded.glyphs.items():
                contours = [c.evaluate(72).tolist() for c in glyph.contours]
                self.glyphs[cp] = {'contours': contours, 'advance_width': glyph.advance_width}
            self.current_file = filename

            self.font_name_entry.delete(0, tk.END)
            self.font_name_entry.insert(0, self.font_name)
            self.upm_spinbox.set(self.units_per_em)

            self.glyph_editor.destroy()
            editor_container = self.glyph_editor.master
            self.glyph_editor = ContourEditor(editor_container, canvas_size=440,
                                               units_per_em=self.units_per_em)
            self.glyph_editor.pack()
            self.update_baseline(self.baseline_scale.get())

            self.update_glyph_editor()
            self.update_stats()
            self.status_var.set(f"Loaded: {filename} (points reconstructed from sine coefficients)")
        except Exception as e:
            messagebox.showerror("Error", f"Failed to load font:\n{e}")

    def save_font(self):
        if self.current_file:
            self.save_font_to(self.current_file)
        else:
            self.save_font_as()

    def save_font_as(self):
        filename = filedialog.asksaveasfilename(
            title="Save Font As", defaultextension=".sinf",
            filetypes=[("SINF Files", "*.sinf"), ("All Files", "*.*")])
        if filename:
            self.current_file = filename
            self.save_font_to(filename)

    def save_font_to(self, filename):
        self.save_current_glyph()
        self.update_font_name()
        try:
            sf_glyphs = []
            for cp, g in self.glyphs.items():
                sf_glyphs.append(sf.encode_glyph(
                    cp, g['advance_width'], g['contours'],
                    num_harmonics=self.harmonics_per_contour, num_samples=self.dft_samples))
            font = sf.SinfFont(self.font_name, self.units_per_em, sf_glyphs)
            sf.save(font, filename)
            self.status_var.set(f"Saved: {filename}")
            messagebox.showinfo("Success", f"Font saved successfully!\n{len(sf_glyphs)} glyphs "
                                            f"(~{self.harmonics_per_contour} harmonics/contour)")
        except Exception as e:
            messagebox.showerror("Error", f"Failed to save font:\n{e}")

    def export_preview(self):
        filename = filedialog.asksaveasfilename(
            title="Export Preview", defaultextension=".png",
            filetypes=[("PNG Image", "*.png"), ("All Files", "*.*")])
        if not filename:
            return
        self.save_current_glyph()
        try:
            chars_per_row = 16
            cell = 60
            rows = max(1, (len(self.glyphs) + chars_per_row - 1) // chars_per_row)
            img = Image.new('RGB', (chars_per_row * cell, rows * cell), 'white')
            draw = ImageDraw.Draw(img)

            for idx, (cp, g) in enumerate(sorted(self.glyphs.items())):
                row, col = divmod(idx, chars_per_row)
                ox, oy = col * cell + 4, row * cell + cell - 8
                scale = (cell - 16) / self.units_per_em
                ordered = sorted(g['contours'], key=polygon_area, reverse=True)
                for i, contour in enumerate(ordered):
                    pts = [(ox + fx * scale, oy - fy * scale) for fx, fy in contour]
                    if len(pts) >= 3:
                        draw.polygon(pts, fill=('black' if i == 0 else 'white'))

            img.save(filename)
            self.status_var.set(f"Preview exported: {filename}")
            messagebox.showinfo("Success", "Preview image exported successfully!")
        except Exception as e:
            messagebox.showerror("Error", f"Failed to export preview:\n{e}")


def main():
    root = tk.Tk()
    app = SinfFontEditorApp(root)
    root.mainloop()


if __name__ == '__main__':
    main()