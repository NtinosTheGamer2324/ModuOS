#!/usr/bin/env python3
"""
ModuOS ILIB Viewer
Inspect and export images from .ilib image library files.
Requires: Pillow  (pip install Pillow)
"""

import tkinter as tk
from tkinter import ttk, filedialog, messagebox
import struct
import zlib
import os
from PIL import Image, ImageTk
import io

# ─────────────────────────────────────────────
#  THEME
# ─────────────────────────────────────────────
BG_DARK      = "#0a0c12"
BG_PANEL     = "#12151f"
BG_CARD      = "#1c2030"
BG_HOVER     = "#242840"
ACCENT_AMBER = "#ffb830"
ACCENT_CYAN  = "#30e5ff"
ACCENT_GREEN = "#3dffa0"
ACCENT_RED   = "#ff4a6e"
TEXT_PRIMARY = "#e8eaf0"
TEXT_DIM     = "#5a6280"
BORDER_COL   = "#252a3e"

FONT_MONO    = ("Courier New", 10)
FONT_MONO_SM = ("Courier New", 9)
FONT_MONO_LG = ("Courier New", 12, "bold")
FONT_UI      = ("Segoe UI", 10)
FONT_UI_SM   = ("Segoe UI", 9)
FONT_UI_B    = ("Segoe UI", 10, "bold")
FONT_TITLE   = ("Segoe UI", 13, "bold")
FONT_HEAD    = ("Segoe UI", 9, "bold")

MAGIC        = b"ILIB"
HEADER_SIZE  = 6
ENTRY_SIZE   = 18   # id(2)+w(2)+h(2)+raw_sz(4)+cmp_sz(4)+offset(4)


# ─────────────────────────────────────────────
#  PARSER
# ─────────────────────────────────────────────
class ILibEntry:
    __slots__ = ("image_id", "width", "height", "raw_size",
                 "cmp_size", "offset")

    def __init__(self, image_id, width, height, raw_size, cmp_size, offset):
        self.image_id = image_id
        self.width    = width
        self.height   = height
        self.raw_size = raw_size
        self.cmp_size = cmp_size
        self.offset   = offset


def parse_ilib(path: str) -> tuple[list[ILibEntry], bytes]:
    """Return (entries, raw_file_bytes). Raises ValueError on bad format."""
    with open(path, "rb") as f:
        data = f.read()

    if len(data) < HEADER_SIZE:
        raise ValueError("File too small to be a valid .ilib.")
    if data[:4] != MAGIC:
        raise ValueError(f"Bad magic: expected {MAGIC!r}, got {data[:4]!r}")

    count = struct.unpack_from("<H", data, 4)[0]
    entries = []
    table_start = HEADER_SIZE
    for i in range(count):
        base = table_start + i * ENTRY_SIZE
        if base + ENTRY_SIZE > len(data):
            raise ValueError(f"Resource table truncated at entry {i}.")
        img_id, w, h, raw_sz, cmp_sz, offset = struct.unpack_from(
            "<HHHIIII"[:7], data, base  # 7 format chars → wrong, fix below
        )
        # Correct unpack: H H H I I I = 2+2+2+4+4+4 = 18 bytes
        img_id, w, h, raw_sz, cmp_sz, offset = struct.unpack_from(
            "<HHHIII", data, base
        )
        entries.append(ILibEntry(img_id, w, h, raw_sz, cmp_sz, offset))

    return entries, data


def decompress_entry(entry: ILibEntry, raw_data: bytes) -> Image.Image:
    cmp_bytes = raw_data[entry.offset: entry.offset + entry.cmp_size]
    rgba_bytes = zlib.decompress(cmp_bytes)
    return Image.frombytes("RGBA", (entry.width, entry.height), rgba_bytes)


# ─────────────────────────────────────────────
#  APPLICATION
# ─────────────────────────────────────────────
class ILibViewer(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("ModuOS  ·  ILIB Viewer")
        self.configure(bg=BG_DARK)
        self.minsize(960, 640)
        self.resizable(True, True)

        self._ilib_path:  str | None        = None
        self._raw_data:   bytes | None      = None
        self._entries:    list[ILibEntry]   = []
        self._pil_cache:  dict[int, Image.Image] = {}
        self._sel_idx:    int               = -1
        self._preview_pil: Image.Image | None = None
        self._zoom:       float             = 1.0

        self._build_ui()
        self._apply_style()

    # ── UI ───────────────────────────────────

    def _build_ui(self):
        # Title bar
        bar = tk.Frame(self, bg=BG_DARK)
        bar.pack(fill="x", padx=16, pady=(12, 4))

        tk.Label(bar, text="⬡ ILIB", font=("Courier New", 14, "bold"),
                 fg=ACCENT_AMBER, bg=BG_DARK).pack(side="left")
        tk.Label(bar, text=" Viewer", font=FONT_TITLE,
                 fg=TEXT_PRIMARY, bg=BG_DARK).pack(side="left", padx=(4, 0))
        tk.Label(bar, text="for ModuOS", font=FONT_UI_SM,
                 fg=TEXT_DIM, bg=BG_DARK).pack(side="left", padx=8)

        self._open_btn = self._btn(bar, "Open .ilib…", self._open_file,
                                   ACCENT_AMBER, large=True)
        self._open_btn.pack(side="right")

        self.file_label = tk.Label(bar, text="No file loaded",
                                   font=FONT_MONO_SM, fg=TEXT_DIM, bg=BG_DARK)
        self.file_label.pack(side="right", padx=12)

        tk.Frame(self, bg=BORDER_COL, height=1).pack(fill="x", padx=16, pady=(4, 10))

        # Main panes
        content = tk.Frame(self, bg=BG_DARK)
        content.pack(fill="both", expand=True, padx=16, pady=0)
        content.columnconfigure(0, weight=1, minsize=220)
        content.columnconfigure(1, weight=3)
        content.rowconfigure(0, weight=1)

        self._build_sidebar(content)
        self._build_main(content)

        # Status bar
        self._build_status()

    def _build_sidebar(self, parent):
        side = tk.Frame(parent, bg=BG_PANEL, highlightthickness=1,
                        highlightbackground=BORDER_COL)
        side.grid(row=0, column=0, sticky="nsew", padx=(0, 8))
        side.rowconfigure(1, weight=1)
        side.columnconfigure(0, weight=1)

        hdr = tk.Frame(side, bg=BG_CARD, pady=6)
        hdr.grid(row=0, column=0, sticky="ew")
        tk.Label(hdr, text="IMAGES", font=FONT_HEAD,
                 fg=ACCENT_AMBER, bg=BG_CARD, padx=10).pack(side="left")
        self.count_lbl = tk.Label(hdr, text="", font=FONT_MONO_SM,
                                  fg=TEXT_DIM, bg=BG_CARD, padx=4)
        self.count_lbl.pack(side="left")

        # Listbox
        lb_frame = tk.Frame(side, bg=BG_PANEL)
        lb_frame.grid(row=1, column=0, sticky="nsew")
        lb_frame.rowconfigure(0, weight=1)
        lb_frame.columnconfigure(0, weight=1)

        self.listbox = tk.Listbox(
            lb_frame, bg=BG_PANEL, fg=TEXT_PRIMARY,
            selectbackground="#2a3a5e", selectforeground="#ffffff",
            font=FONT_MONO_SM, relief="flat", bd=0,
            highlightthickness=0, activestyle="none",
            exportselection=False
        )
        vsb = tk.Scrollbar(lb_frame, orient="vertical",
                           command=self.listbox.yview, bg=BG_CARD,
                           troughcolor=BG_PANEL, bd=0, relief="flat",
                           highlightthickness=0)
        self.listbox.configure(yscrollcommand=vsb.set)
        self.listbox.grid(row=0, column=0, sticky="nsew")
        vsb.grid(row=0, column=1, sticky="ns")
        self.listbox.bind("<<ListboxSelect>>", self._on_list_select)

    def _build_main(self, parent):
        main = tk.Frame(parent, bg=BG_PANEL, highlightthickness=1,
                        highlightbackground=BORDER_COL)
        main.grid(row=0, column=1, sticky="nsew")
        main.rowconfigure(1, weight=1)
        main.columnconfigure(0, weight=1)

        # Top toolbar
        toolbar = tk.Frame(main, bg=BG_CARD, pady=6)
        toolbar.grid(row=0, column=0, sticky="ew")

        tk.Label(toolbar, text="PREVIEW", font=FONT_HEAD,
                 fg=ACCENT_CYAN, bg=BG_CARD, padx=12).pack(side="left")

        self.img_title = tk.Label(toolbar, text="", font=FONT_MONO_SM,
                                  fg=TEXT_DIM, bg=BG_CARD)
        self.img_title.pack(side="left", padx=4)

        # Zoom controls
        zoom_frame = tk.Frame(toolbar, bg=BG_CARD)
        zoom_frame.pack(side="right", padx=8)

        self._btn(zoom_frame, "−", self._zoom_out, TEXT_DIM, width=3).pack(side="left", padx=1)
        self.zoom_lbl = tk.Label(zoom_frame, text="100%", font=FONT_MONO_SM,
                                 fg=TEXT_PRIMARY, bg=BG_CARD, width=5, anchor="center")
        self.zoom_lbl.pack(side="left")
        self._btn(zoom_frame, "+", self._zoom_in,  TEXT_DIM, width=3).pack(side="left", padx=1)
        self._btn(zoom_frame, "⊡", self._zoom_fit, ACCENT_CYAN, width=3).pack(side="left", padx=(4, 2))

        self._btn(toolbar, "Export PNG…", self._export_png,
                  ACCENT_GREEN).pack(side="right", padx=(0, 8))
        self._btn(toolbar, "Export All…", self._export_all,
                  ACCENT_GREEN).pack(side="right", padx=2)

        # Canvas
        canvas_wrap = tk.Frame(main, bg=BG_DARK)
        canvas_wrap.grid(row=1, column=0, sticky="nsew", padx=1, pady=1)
        canvas_wrap.rowconfigure(0, weight=1)
        canvas_wrap.columnconfigure(0, weight=1)

        self.canvas = tk.Canvas(canvas_wrap, bg=BG_DARK,
                                highlightthickness=0, cursor="crosshair")
        hbar = tk.Scrollbar(canvas_wrap, orient="horizontal",
                            command=self.canvas.xview)
        vbar = tk.Scrollbar(canvas_wrap, orient="vertical",
                            command=self.canvas.yview)
        self.canvas.configure(xscrollcommand=hbar.set,
                              yscrollcommand=vbar.set)
        self.canvas.grid(row=0, column=0, sticky="nsew")
        vbar.grid(row=0, column=1, sticky="ns")
        hbar.grid(row=1, column=0, sticky="ew")

        self.canvas.bind("<Configure>", self._on_canvas_resize)
        self.canvas.bind("<MouseWheel>", self._on_scroll_zoom)   # Windows/macOS
        self.canvas.bind("<Button-4>", self._on_scroll_zoom)     # Linux scroll up
        self.canvas.bind("<Button-5>", self._on_scroll_zoom)     # Linux scroll down

        # Right side: info panel
        info_panel = tk.Frame(main, bg=BG_CARD)
        info_panel.grid(row=2, column=0, sticky="ew")
        self._build_info_panel(info_panel)

    def _build_info_panel(self, parent):
        cols_frame = tk.Frame(parent, bg=BG_CARD)
        cols_frame.pack(fill="x", padx=12, pady=8)

        fields = [
            ("Image ID",     "id"),
            ("Dimensions",   "dims"),
            ("Raw RGBA",     "raw"),
            ("Compressed",   "cmp"),
            ("Ratio",        "ratio"),
            ("File Offset",  "offset"),
        ]
        for i, (label, key) in enumerate(fields):
            col = i % 3
            row = i // 3
            cell = tk.Frame(cols_frame, bg=BG_CARD)
            cell.grid(row=row, column=col, sticky="w", padx=(0, 28), pady=2)
            tk.Label(cell, text=label + ":", font=FONT_UI_SM,
                     fg=TEXT_DIM, bg=BG_CARD).pack(side="left", padx=(0, 4))
            var = tk.StringVar(value="—")
            self.__dict__[f"_info_{key}"] = var
            tk.Label(cell, textvariable=var, font=FONT_MONO_SM,
                     fg=TEXT_PRIMARY, bg=BG_CARD).pack(side="left")

    def _build_status(self):
        bar = tk.Frame(self, bg=BG_CARD, pady=5, highlightthickness=1,
                       highlightbackground=BORDER_COL)
        bar.pack(fill="x", padx=16, pady=(8, 12))
        self.status_var = tk.StringVar(value="Open an .ilib file to begin.")
        tk.Label(bar, textvariable=self.status_var, font=FONT_UI_SM,
                 fg=TEXT_DIM, bg=BG_CARD, anchor="w", padx=12).pack(side="left")

    # ── STYLE ────────────────────────────────

    def _apply_style(self):
        style = ttk.Style(self)
        style.theme_use("clam")
        style.configure("Vertical.TScrollbar",
                        background=BG_CARD, troughcolor=BG_PANEL,
                        borderwidth=0, arrowcolor=TEXT_DIM)
        style.configure("Horizontal.TScrollbar",
                        background=BG_CARD, troughcolor=BG_PANEL,
                        borderwidth=0, arrowcolor=TEXT_DIM)

    def _btn(self, parent, text, cmd, color, width=None, large=False):
        kw = dict(
            text=text, command=cmd,
            font=FONT_UI if not large else FONT_UI_B,
            fg=color, bg=BG_CARD,
            activeforeground="#ffffff", activebackground=BG_HOVER,
            relief="flat", bd=0, cursor="hand2",
            pady=4 if not large else 6,
            padx=8 if not large else 14,
        )
        if width:
            kw["width"] = width
        btn = tk.Button(parent, **kw)
        btn.bind("<Enter>", lambda _: btn.configure(bg=BG_HOVER))
        btn.bind("<Leave>", lambda _: btn.configure(bg=BG_CARD))
        return btn

    # ── FILE LOADING ─────────────────────────

    def _open_file(self):
        path = filedialog.askopenfilename(
            title="Open ILIB File",
            filetypes=[("Image Library", "*.ilib"), ("All Files", "*.*")]
        )
        if not path:
            return
        try:
            entries, raw = parse_ilib(path)
        except Exception as e:
            messagebox.showerror("Parse Error", str(e))
            return

        self._ilib_path = path
        self._raw_data  = raw
        self._entries   = entries
        self._pil_cache = {}
        self._sel_idx   = -1
        self._preview_pil = None

        self.title(f"ModuOS  ·  ILIB Viewer  —  {os.path.basename(path)}")
        self.file_label.configure(text=os.path.basename(path), fg=ACCENT_AMBER)
        self.count_lbl.configure(text=f"({len(entries)})")

        self.listbox.delete(0, "end")
        for e in entries:
            self.listbox.insert("end", f"  #{e.image_id:04d}  {e.width}×{e.height}")

        self._clear_preview()
        self.status_var.set(
            f"Loaded {len(entries)} image(s) · "
            f"File size: {self._fmt_bytes(len(raw))} · "
            f"{os.path.basename(path)}"
        )

        if entries:
            self.listbox.selection_set(0)
            self.listbox.event_generate("<<ListboxSelect>>")

    # ── SELECTION & PREVIEW ──────────────────

    def _on_list_select(self, _=None):
        sel = self.listbox.curselection()
        if not sel:
            return
        idx = sel[0]
        if idx == self._sel_idx:
            return
        self._sel_idx = idx
        entry = self._entries[idx]

        # Decode (cached)
        if idx not in self._pil_cache:
            try:
                self._pil_cache[idx] = decompress_entry(entry, self._raw_data)
            except Exception as e:
                messagebox.showerror("Decode Error", str(e))
                return

        self._preview_pil = self._pil_cache[idx]
        self._zoom = 1.0
        self._zoom_fit(silent=True)

        self.img_title.configure(
            text=f"Image #{entry.image_id:04d}  —  {entry.width}×{entry.height} px"
        )
        # Info panel
        raw_sz = entry.raw_size
        cmp_sz = entry.cmp_size
        ratio  = raw_sz / cmp_sz if cmp_sz else 0
        self._info_id.set(f"{entry.image_id:04d}")
        self._info_dims.set(f"{entry.width} × {entry.height} px")
        self._info_raw.set(self._fmt_bytes(raw_sz))
        self._info_cmp.set(self._fmt_bytes(cmp_sz))
        self._info_ratio.set(f"{ratio:.2f}×")
        self._info_offset.set(f"0x{entry.offset:08X}")

        self.status_var.set(
            f"Image #{entry.image_id:04d} · {entry.width}×{entry.height} · "
            f"Raw {self._fmt_bytes(raw_sz)} → Compressed {self._fmt_bytes(cmp_sz)}"
        )

    def _draw_preview(self):
        if self._preview_pil is None:
            return
        c = self.canvas
        cw = c.winfo_width()
        ch = c.winfo_height()
        if cw < 2 or ch < 2:
            return

        c.delete("all")

        iw, ih = self._preview_pil.size
        nw = max(1, int(iw * self._zoom))
        nh = max(1, int(ih * self._zoom))

        # Checkerboard
        sq = max(8, min(24, int(16 * self._zoom)))
        ox = max(0, (cw - nw) // 2)
        oy = max(0, (ch - nh) // 2)
        for row in range(0, nh, sq):
            for col in range(0, nw, sq):
                color = "#1e2030" if (row // sq + col // sq) % 2 == 0 else "#181a28"
                c.create_rectangle(ox + col, oy + row,
                                   ox + col + sq, oy + row + sq,
                                   fill=color, outline="")

        # Scaled image
        interp = Image.NEAREST if self._zoom >= 2 else Image.LANCZOS
        resized = self._preview_pil.resize((nw, nh), interp)
        tk_img = ImageTk.PhotoImage(resized)
        c._tk_img = tk_img   # keep reference

        c.create_image(ox, oy, anchor="nw", image=tk_img)

        # Border
        c.create_rectangle(ox - 1, oy - 1, ox + nw, oy + nh,
                            outline=BORDER_COL, width=1)

        # Pixel grid overlay when very zoomed in
        if self._zoom >= 8:
            for col in range(0, nw, int(self._zoom)):
                c.create_line(ox + col, oy, ox + col, oy + nh,
                              fill="#2a2a3a", width=1)
            for row in range(0, nh, int(self._zoom)):
                c.create_line(ox, oy + row, ox + nw, oy + row,
                              fill="#2a2a3a", width=1)

        full_w = max(cw, nw + 2 * ox)
        full_h = max(ch, nh + 2 * oy)
        c.configure(scrollregion=(0, 0, full_w, full_h))
        self.zoom_lbl.configure(text=f"{int(self._zoom * 100)}%")

    def _clear_preview(self):
        self.canvas.delete("all")
        self._preview_pil = None
        for key in ("id", "dims", "raw", "cmp", "ratio", "offset"):
            self.__dict__[f"_info_{key}"].set("—")
        self.img_title.configure(text="")

    def _on_canvas_resize(self, _=None):
        self._draw_preview()

    # ── ZOOM ─────────────────────────────────

    def _zoom_in(self):
        self._zoom = min(32.0, self._zoom * 1.5)
        self._draw_preview()

    def _zoom_out(self):
        self._zoom = max(0.05, self._zoom / 1.5)
        self._draw_preview()

    def _zoom_fit(self, silent=False):
        if self._preview_pil is None:
            return
        cw = self.canvas.winfo_width()
        ch = self.canvas.winfo_height()
        if cw < 2 or ch < 2:
            self.after(50, self._zoom_fit)
            return
        iw, ih = self._preview_pil.size
        pad = 24
        self._zoom = min((cw - pad) / iw, (ch - pad) / ih, 1.0)
        self._draw_preview()

    def _on_scroll_zoom(self, event):
        if event.num == 4 or (hasattr(event, "delta") and event.delta > 0):
            self._zoom_in()
        else:
            self._zoom_out()

    # ── EXPORT ───────────────────────────────

    def _export_png(self):
        if self._sel_idx < 0 or self._preview_pil is None:
            messagebox.showinfo("No Selection", "Select an image first.")
            return
        entry = self._entries[self._sel_idx]
        default = f"image_{entry.image_id:04d}.png"
        path = filedialog.asksaveasfilename(
            title="Export Image as PNG",
            defaultextension=".png",
            initialfile=default,
            filetypes=[("PNG", "*.png")]
        )
        if not path:
            return
        try:
            self._preview_pil.save(path)
            self.status_var.set(f"Exported → {path}")
        except Exception as e:
            messagebox.showerror("Export Error", str(e))

    def _export_all(self):
        if not self._entries:
            messagebox.showinfo("Nothing to Export", "Load an .ilib file first.")
            return
        folder = filedialog.askdirectory(title="Select Export Folder")
        if not folder:
            return

        base = os.path.splitext(os.path.basename(self._ilib_path))[0]
        errors = []
        for i, entry in enumerate(self._entries):
            try:
                if i not in self._pil_cache:
                    self._pil_cache[i] = decompress_entry(entry, self._raw_data)
                img = self._pil_cache[i]
                name = f"{base}_{entry.image_id:04d}.png"
                img.save(os.path.join(folder, name))
            except Exception as e:
                errors.append(f"#{entry.image_id}: {e}")

        if errors:
            messagebox.showwarning("Partial Export",
                                   "Some images failed:\n" + "\n".join(errors))
        else:
            messagebox.showinfo("Export Complete",
                                f"Exported {len(self._entries)} image(s) to:\n{folder}")
        self.status_var.set(f"Exported {len(self._entries)} image(s) → {folder}")

    # ── UTILITIES ────────────────────────────

    @staticmethod
    def _fmt_bytes(n: int) -> str:
        if n < 1024:
            return f"{n} B"
        elif n < 1024 ** 2:
            return f"{n / 1024:.1f} KB"
        return f"{n / 1024 ** 2:.2f} MB"


# ─────────────────────────────────────────────
if __name__ == "__main__":
    app = ILibViewer()
    app.mainloop()