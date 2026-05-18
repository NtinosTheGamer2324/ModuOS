#!/usr/bin/env python3
"""
ModuOS ILIB Compiler
A GUI tool for packing PNG images into the .ilib custom image library format.
Requires: Pillow (pip install Pillow)
"""

import tkinter as tk
from tkinter import ttk, filedialog, messagebox
import struct
import zlib
import os
from PIL import Image, ImageTk


# ─────────────────────────────────────────────
#  CONSTANTS & THEME
# ─────────────────────────────────────────────
BG_DARK      = "#0f1117"
BG_PANEL     = "#1a1d27"
BG_CARD      = "#22263a"
ACCENT_BLUE  = "#4a9eff"
ACCENT_GREEN = "#3dffa0"
ACCENT_RED   = "#ff4a6e"
TEXT_PRIMARY = "#e8eaf0"
TEXT_DIM     = "#6b7280"
BORDER_COL   = "#2e3350"

FONT_MONO    = ("Courier New", 10)
FONT_MONO_SM = ("Courier New", 9)
FONT_UI      = ("Segoe UI", 10)
FONT_UI_SM   = ("Segoe UI", 9)
FONT_TITLE   = ("Segoe UI", 13, "bold")
FONT_HEAD    = ("Segoe UI", 9, "bold")

MAGIC        = b"ILIB"
HEADER_SIZE  = 6           # magic(4) + count(2)
ENTRY_SIZE   = 18          # id(2)+w(2)+h(2)+raw_sz(4)+cmp_sz(4)+offset(4)  = 18


# ─────────────────────────────────────────────
#  MAIN APPLICATION
# ─────────────────────────────────────────────
class ILibCompiler(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("ModuOS  ·  ILIB Image Library Compiler")
        self.configure(bg=BG_DARK)
        self.minsize(900, 620)
        self.resizable(True, True)

        # State
        self.images: list[dict] = []   # {path, name, width, height, pil_img, tk_thumb}
        self.selected_index: int = -1
        self._preview_pil: Image.Image | None = None

        self._build_ui()
        self._apply_treeview_style()

    # ── UI CONSTRUCTION ──────────────────────

    def _build_ui(self):
        # ── Top title bar ──
        title_bar = tk.Frame(self, bg=BG_DARK, pady=0)
        title_bar.pack(fill="x", padx=16, pady=(14, 4))

        tk.Label(title_bar, text="⬡ ILIB", font=("Courier New", 15, "bold"),
                 fg=ACCENT_BLUE, bg=BG_DARK).pack(side="left")
        tk.Label(title_bar, text=" Image Library Compiler", font=FONT_TITLE,
                 fg=TEXT_PRIMARY, bg=BG_DARK).pack(side="left", padx=(4, 0))
        tk.Label(title_bar, text="for ModuOS", font=FONT_UI_SM,
                 fg=TEXT_DIM, bg=BG_DARK).pack(side="left", padx=(8, 0))

        sep = tk.Frame(self, bg=BORDER_COL, height=1)
        sep.pack(fill="x", padx=16, pady=(6, 10))

        # ── Main content (left list + right preview) ──
        content = tk.Frame(self, bg=BG_DARK)
        content.pack(fill="both", expand=True, padx=16, pady=0)
        content.columnconfigure(0, weight=3)
        content.columnconfigure(1, weight=2)
        content.rowconfigure(0, weight=1)

        self._build_list_panel(content)
        self._build_preview_panel(content)

        # ── Bottom toolbar ──
        self._build_bottom_bar()

    def _build_list_panel(self, parent):
        panel = tk.Frame(parent, bg=BG_PANEL, bd=0, highlightthickness=1,
                         highlightbackground=BORDER_COL)
        panel.grid(row=0, column=0, sticky="nsew", padx=(0, 8))
        panel.rowconfigure(1, weight=1)
        panel.columnconfigure(0, weight=1)

        # Panel header + buttons
        hdr = tk.Frame(panel, bg=BG_CARD, pady=6)
        hdr.grid(row=0, column=0, sticky="ew")

        tk.Label(hdr, text="IMAGE LIST", font=FONT_HEAD,
                 fg=ACCENT_BLUE, bg=BG_CARD, padx=12).pack(side="left")

        btn_frame = tk.Frame(hdr, bg=BG_CARD)
        btn_frame.pack(side="right", padx=8)

        self._btn(btn_frame, "＋ Add Images", self._add_images,
                  ACCENT_BLUE).pack(side="left", padx=2)
        self._btn(btn_frame, "✕ Remove", self._remove_selected,
                  ACCENT_RED).pack(side="left", padx=2)
        self._btn(btn_frame, "▲", self._move_up,   "#7c8cff", width=3).pack(side="left", padx=2)
        self._btn(btn_frame, "▼", self._move_down, "#7c8cff", width=3).pack(side="left", padx=2)

        # Treeview
        tree_frame = tk.Frame(panel, bg=BG_PANEL)
        tree_frame.grid(row=1, column=0, sticky="nsew", padx=1, pady=1)
        tree_frame.rowconfigure(0, weight=1)
        tree_frame.columnconfigure(0, weight=1)

        cols = ("id", "filename", "format", "dimensions", "size")
        self.tree = ttk.Treeview(tree_frame, columns=cols, show="headings",
                                 selectmode="browse")

        self.tree.heading("id",         text="ID",         anchor="center")
        self.tree.heading("filename",   text="Filename",   anchor="w")
        self.tree.heading("format",     text="Fmt",        anchor="center")
        self.tree.heading("dimensions", text="Dimensions", anchor="center")
        self.tree.heading("size",       text="Raw Size",   anchor="center")

        self.tree.column("id",         width=44,  minwidth=40,  anchor="center")
        self.tree.column("filename",   width=200, minwidth=120, anchor="w")
        self.tree.column("format",     width=46,  minwidth=40,  anchor="center")
        self.tree.column("dimensions", width=100, minwidth=80,  anchor="center")
        self.tree.column("size",       width=90,  minwidth=70,  anchor="center")

        vsb = ttk.Scrollbar(tree_frame, orient="vertical", command=self.tree.yview)
        self.tree.configure(yscrollcommand=vsb.set)

        self.tree.grid(row=0, column=0, sticky="nsew")
        vsb.grid(row=0, column=1, sticky="ns")

        self.tree.bind("<<TreeviewSelect>>", self._on_select)

        # Status bar
        self.list_status = tk.StringVar(value="No images loaded.")
        tk.Label(panel, textvariable=self.list_status, font=FONT_UI_SM,
                 fg=TEXT_DIM, bg=BG_CARD, anchor="w", padx=10, pady=4
                 ).grid(row=2, column=0, sticky="ew")

    def _build_preview_panel(self, parent):
        panel = tk.Frame(parent, bg=BG_PANEL, bd=0, highlightthickness=1,
                         highlightbackground=BORDER_COL)
        panel.grid(row=0, column=1, sticky="nsew")
        panel.rowconfigure(1, weight=1)
        panel.columnconfigure(0, weight=1)

        hdr = tk.Frame(panel, bg=BG_CARD, pady=6)
        hdr.grid(row=0, column=0, sticky="ew")
        tk.Label(hdr, text="PREVIEW", font=FONT_HEAD,
                 fg=ACCENT_GREEN, bg=BG_CARD, padx=12).pack(side="left")
        self.preview_name = tk.Label(hdr, text="", font=FONT_MONO_SM,
                                     fg=TEXT_DIM, bg=BG_CARD, padx=6)
        self.preview_name.pack(side="left")

        # Canvas for image preview (checkerboard bg for transparency)
        canvas_frame = tk.Frame(panel, bg=BG_PANEL)
        canvas_frame.grid(row=1, column=0, sticky="nsew", padx=8, pady=8)
        canvas_frame.rowconfigure(0, weight=1)
        canvas_frame.columnconfigure(0, weight=1)

        self.preview_canvas = tk.Canvas(canvas_frame, bg=BG_CARD,
                                        highlightthickness=1,
                                        highlightbackground=BORDER_COL,
                                        cursor="crosshair")
        self.preview_canvas.grid(row=0, column=0, sticky="nsew")
        self.preview_canvas.bind("<Configure>", self._redraw_preview)

        # Info labels
        info_frame = tk.Frame(panel, bg=BG_CARD)
        info_frame.grid(row=2, column=0, sticky="ew", padx=0, pady=0)

        self.info_vars = {}
        rows = [("Image ID", "id"), ("Dimensions", "dims"),
                ("Raw RGBA", "raw"), ("Est. Compressed", "cmp")]
        for label, key in rows:
            row = tk.Frame(info_frame, bg=BG_CARD)
            row.pack(fill="x", padx=12, pady=1)
            tk.Label(row, text=f"{label}:", font=FONT_UI_SM,
                     fg=TEXT_DIM, bg=BG_CARD, width=16, anchor="w").pack(side="left")
            var = tk.StringVar(value="—")
            self.info_vars[key] = var
            tk.Label(row, textvariable=var, font=FONT_MONO_SM,
                     fg=TEXT_PRIMARY, bg=BG_CARD, anchor="w").pack(side="left")

        tk.Frame(info_frame, bg=BG_CARD, height=8).pack()

    def _build_bottom_bar(self):
        bar = tk.Frame(self, bg=BG_CARD, pady=10, bd=0,
                       highlightthickness=1, highlightbackground=BORDER_COL)
        bar.pack(fill="x", padx=16, pady=(10, 14))

        tk.Label(bar, text="Output File:", font=FONT_UI,
                 fg=TEXT_DIM, bg=BG_CARD, padx=12).pack(side="left")

        self.output_var = tk.StringVar(value="resources.ilib")
        entry = tk.Entry(bar, textvariable=self.output_var,
                         font=FONT_MONO, fg=TEXT_PRIMARY, bg=BG_DARK,
                         insertbackground=ACCENT_BLUE, relief="flat",
                         bd=0, highlightthickness=1,
                         highlightbackground=BORDER_COL,
                         highlightcolor=ACCENT_BLUE, width=28)
        entry.pack(side="left", ipady=5, padx=(0, 6))

        self._btn(bar, "Browse…", self._browse_output,
                  TEXT_DIM).pack(side="left", padx=(0, 20))

        self.compile_btn = self._btn(bar, "⬡  Compile Library",
                                     self._compile, ACCENT_GREEN, large=True)
        self.compile_btn.pack(side="right", padx=12)

        self.compile_status = tk.Label(bar, text="", font=FONT_UI_SM,
                                       fg=ACCENT_GREEN, bg=BG_CARD, padx=8)
        self.compile_status.pack(side="right")

    # ── WIDGET HELPERS ───────────────────────

    def _btn(self, parent, text, cmd, color, width=None, large=False):
        kw = dict(text=text, command=cmd, font=FONT_UI if not large else ("Segoe UI", 10, "bold"),
                  fg=color, bg=BG_CARD, activeforeground="#ffffff",
                  activebackground=BG_DARK, relief="flat", bd=0,
                  cursor="hand2", pady=4 if not large else 6,
                  padx=8 if not large else 18)
        if width:
            kw["width"] = width
        btn = tk.Button(parent, **kw)

        def on_enter(_):
            btn.configure(bg="#2a2f4a")
        def on_leave(_):
            btn.configure(bg=BG_CARD)

        btn.bind("<Enter>", on_enter)
        btn.bind("<Leave>", on_leave)
        return btn

    def _apply_treeview_style(self):
        style = ttk.Style(self)
        style.theme_use("clam")
        style.configure("Treeview",
                         background=BG_PANEL, foreground=TEXT_PRIMARY,
                         fieldbackground=BG_PANEL, borderwidth=0,
                         rowheight=24, font=FONT_MONO_SM)
        style.configure("Treeview.Heading",
                         background=BG_CARD, foreground=ACCENT_BLUE,
                         font=FONT_HEAD, borderwidth=0, relief="flat")
        style.map("Treeview",
                  background=[("selected", "#2a3a5e")],
                  foreground=[("selected", "#ffffff")])
        style.map("Treeview.Heading", background=[("active", BG_CARD)])
        style.configure("Vertical.TScrollbar",
                         background=BG_CARD, troughcolor=BG_PANEL,
                         borderwidth=0, arrowcolor=TEXT_DIM)

    # ── IMAGE LIST OPERATIONS ────────────────

    def _add_images(self):
        paths = filedialog.askopenfilenames(
            title="Select PNG or BMP Images",
            filetypes=[
                ("Supported Images", "*.png *.bmp"),
                ("PNG Images",       "*.png"),
                ("BMP Images",       "*.bmp"),
                ("All Files",        "*.*"),
            ]
        )
        for path in paths:
            try:
                img = Image.open(path).convert("RGBA")
                ext = os.path.splitext(path)[1].upper().lstrip(".")
                self.images.append({
                    "path":    path,
                    "name":    os.path.basename(path),
                    "format":  ext,          # "PNG" or "BMP"
                    "width":   img.width,
                    "height":  img.height,
                    "pil_img": img,
                    "tk_thumb": None,
                })
            except Exception as e:
                messagebox.showerror("Load Error", f"Could not open:\n{path}\n\n{e}")
        self._refresh_tree()

    def _remove_selected(self):
        sel = self.tree.selection()
        if not sel:
            return
        idx = self.tree.index(sel[0])
        del self.images[idx]
        self.selected_index = -1
        self._clear_preview()
        self._refresh_tree()

    def _move_up(self):
        sel = self.tree.selection()
        if not sel:
            return
        idx = self.tree.index(sel[0])
        if idx == 0:
            return
        self.images[idx - 1], self.images[idx] = self.images[idx], self.images[idx - 1]
        self._refresh_tree(select=idx - 1)

    def _move_down(self):
        sel = self.tree.selection()
        if not sel:
            return
        idx = self.tree.index(sel[0])
        if idx >= len(self.images) - 1:
            return
        self.images[idx], self.images[idx + 1] = self.images[idx + 1], self.images[idx]
        self._refresh_tree(select=idx + 1)

    def _refresh_tree(self, select: int = -1):
        self.tree.delete(*self.tree.get_children())
        for i, img in enumerate(self.images):
            raw_sz = img["width"] * img["height"] * 4
            self.tree.insert("", "end", iid=str(i), values=(
                f"{i:04d}",
                img["name"],
                img.get("format", "PNG"),
                f"{img['width']} × {img['height']}",
                self._fmt_bytes(raw_sz),
            ))
        count = len(self.images)
        self.list_status.set(
            f"{count} image{'s' if count != 1 else ''} loaded." if count
            else "No images loaded."
        )
        if select >= 0 and select < len(self.images):
            iid = str(select)
            self.tree.selection_set(iid)
            self.tree.see(iid)

    # ── PREVIEW ──────────────────────────────

    def _on_select(self, _event=None):
        sel = self.tree.selection()
        if not sel:
            self._clear_preview()
            return
        idx = self.tree.index(sel[0])
        self.selected_index = idx
        img_data = self.images[idx]
        self._preview_pil = img_data["pil_img"]
        self.preview_name.configure(text=img_data["name"])
        self._redraw_preview()
        # Info
        raw_sz = img_data["width"] * img_data["height"] * 4
        raw_bytes = self._preview_pil.tobytes()
        cmp_sz = len(zlib.compress(raw_bytes, level=6))
        self.info_vars["id"].set(f"{idx:04d}")
        self.info_vars["dims"].set(f"{img_data['width']} × {img_data['height']} px")
        self.info_vars["raw"].set(self._fmt_bytes(raw_sz))
        self.info_vars["cmp"].set(self._fmt_bytes(cmp_sz))

    def _redraw_preview(self, _event=None):
        if self._preview_pil is None:
            return
        c = self.preview_canvas
        cw = c.winfo_width()
        ch = c.winfo_height()
        if cw < 2 or ch < 2:
            return
        c.delete("all")
        # Checkerboard background
        sq = 12
        for row in range(0, ch, sq):
            for col in range(0, cw, sq):
                color = "#2a2a2a" if (row // sq + col // sq) % 2 == 0 else "#1e1e1e"
                c.create_rectangle(col, row, col + sq, row + sq,
                                   fill=color, outline="")
        # Scale image to fit
        iw, ih = self._preview_pil.size
        scale = min((cw - 8) / iw, (ch - 8) / ih, 1.0)
        nw, nh = max(1, int(iw * scale)), max(1, int(ih * scale))
        resized = self._preview_pil.resize((nw, nh), Image.NEAREST if scale >= 1
                                           else Image.LANCZOS)
        tk_img = ImageTk.PhotoImage(resized)
        # Keep reference
        c._tk_img = tk_img
        x = (cw - nw) // 2
        y = (ch - nh) // 2
        c.create_image(x, y, anchor="nw", image=tk_img)
        # Border around image
        c.create_rectangle(x - 1, y - 1, x + nw, y + nh,
                            outline=BORDER_COL, width=1)

    def _clear_preview(self):
        self._preview_pil = None
        self.preview_canvas.delete("all")
        self.preview_name.configure(text="")
        for var in self.info_vars.values():
            var.set("—")

    # ── OUTPUT & COMPILE ─────────────────────

    def _browse_output(self):
        path = filedialog.asksaveasfilename(
            title="Save ILIB File",
            defaultextension=".ilib",
            initialfile=self.output_var.get(),
            filetypes=[("Image Library", "*.ilib"), ("All Files", "*.*")]
        )
        if path:
            self.output_var.set(path)

    def _compile(self):
        if not self.images:
            messagebox.showwarning("Nothing to Compile",
                                   "Please add at least one image before compiling.")
            return

        out_path = self.output_var.get().strip()
        if not out_path:
            messagebox.showwarning("No Output Path",
                                   "Please specify an output filename.")
            return

        self.compile_status.configure(text="Compiling…", fg=ACCENT_BLUE)
        self.update_idletasks()

        try:
            self._write_ilib(out_path)
        except Exception as e:
            self.compile_status.configure(text="Error.", fg=ACCENT_RED)
            messagebox.showerror("Compile Error", str(e))
            return

        self.compile_status.configure(text="✓ Compiled!", fg=ACCENT_GREEN)
        total_sz = os.path.getsize(out_path)
        messagebox.showinfo(
            "Success",
            f"Library compiled successfully!\n\n"
            f"  File:    {os.path.basename(out_path)}\n"
            f"  Images:  {len(self.images)}\n"
            f"  Size:    {self._fmt_bytes(total_sz)}\n\n"
            f"Path: {out_path}"
        )

    def _write_ilib(self, out_path: str):
        """
        Binary layout:
          [Container Header]          6 bytes
          [Resource Table]            N × 18 bytes
          [Data Payloads]             variable
        """
        count = len(self.images)

        # ── Pass 1: compress all images, record sizes ──
        payloads: list[bytes] = []
        for entry in self.images:
            raw = entry["pil_img"].tobytes()          # RGBA top-down
            compressed = zlib.compress(raw, level=6)
            payloads.append(compressed)

        # ── Calculate absolute offsets ──
        table_offset  = HEADER_SIZE                    # where resource table begins
        payload_start = table_offset + count * ENTRY_SIZE
        offsets: list[int] = []
        cursor = payload_start
        for cmp in payloads:
            offsets.append(cursor)
            cursor += len(cmp)

        # ── Write file ──
        with open(out_path, "wb") as f:
            # Container Header
            f.write(MAGIC)                             # 4 bytes
            f.write(struct.pack("<H", count))          # 2 bytes  uint16_t

            # Resource Table
            for i, (entry, cmp, offset) in enumerate(
                    zip(self.images, payloads, offsets)):
                raw_sz = entry["width"] * entry["height"] * 4
                f.write(struct.pack("<H", i))                   # Image ID
                f.write(struct.pack("<H", entry["width"]))      # Width
                f.write(struct.pack("<H", entry["height"]))     # Height
                f.write(struct.pack("<I", raw_sz))              # Uncompressed size
                f.write(struct.pack("<I", len(cmp)))            # Compressed size
                f.write(struct.pack("<I", offset))              # Abs. file offset

            # Data Payloads
            for cmp in payloads:
                f.write(cmp)

    # ── UTILITIES ────────────────────────────

    @staticmethod
    def _fmt_bytes(n: int) -> str:
        if n < 1024:
            return f"{n} B"
        elif n < 1024 ** 2:
            return f"{n / 1024:.1f} KB"
        else:
            return f"{n / 1024 ** 2:.2f} MB"


# ─────────────────────────────────────────────
#  ENTRY POINT
# ─────────────────────────────────────────────
if __name__ == "__main__":
    app = ILibCompiler()
    app.mainloop()