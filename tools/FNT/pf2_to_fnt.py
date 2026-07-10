#!/usr/bin/env python3
"""
pf2_to_fnt.py — GUI for converting GRUB PF2 fonts to ModuOS FNT format
Requires: Python 3.x + tkinter (stdlib)
"""

import sys
import struct
import os
import threading
import tkinter as tk
from tkinter import ttk, filedialog, messagebox

# ── PF2 parser ────────────────────────────────────────────────────────────

PF2_MAGIC = b"FILE\x00\x04PFF2"

def read_pf2_sections(data):
    if data[:9] != PF2_MAGIC:
        raise ValueError(f"Not a PF2 file (magic={data[:9]!r})")
    pos = 9
    sections = {}
    while pos < len(data):
        if pos + 8 > len(data): break
        name   = data[pos:pos+4].decode("ascii", errors="replace"); pos += 4
        length = struct.unpack_from(">I", data, pos)[0];            pos += 4
        sections[name] = data[pos:pos+length];                      pos += length
    return sections

def parse_pf2(data, progress_cb=None):
    sections = read_pf2_sections(data)
    def s(n, d=b""): return sections.get(n, d)

    font = {}
    font["name"]       = s("NAME").rstrip(b"\x00").decode("utf-8", errors="replace")
    font["family"]     = s("FAMI").rstrip(b"\x00").decode("utf-8", errors="replace")
    ptsz = s("PTSZ"); font["point_size"] = struct.unpack_from(">H", ptsz)[0] if len(ptsz)>=2 else 0
    maxw = s("MAXW"); font["max_width"]  = struct.unpack_from(">H", maxw)[0] if len(maxw)>=2 else 0
    maxh = s("MAXH"); font["max_height"] = struct.unpack_from(">H", maxh)[0] if len(maxh)>=2 else 0
    asce = s("ASCE"); font["ascent"]     = struct.unpack_from(">H", asce)[0] if len(asce)>=2 else 0
    font["baseline"] = font["ascent"]

    chix = s("CHIX")
    char_index = []
    for i in range(0, len(chix) - 8, 9):
        cp     = struct.unpack_from(">I", chix, i)[0]
        flags  = chix[i+4]
        offset = struct.unpack_from(">I", chix, i+5)[0]
        char_index.append((cp, flags, offset))

    bdat   = s("BDAT")
    glyphs = []
    total  = len(char_index)

    for idx, (cp, flags, offset) in enumerate(char_index):
        if progress_cb and idx % 50 == 0:
            progress_cb(idx, total)
        if offset + 10 > len(bdat): continue

        width        = struct.unpack_from(">H", bdat, offset)[0]; offset += 2
        height       = struct.unpack_from(">H", bdat, offset)[0]; offset += 2
        x_offset     = struct.unpack_from(">h", bdat, offset)[0]; offset += 2
        y_offset     = struct.unpack_from(">h", bdat, offset)[0]; offset += 2
        device_width = struct.unpack_from(">H", bdat, offset)[0]; offset += 2

        bytes_per_row = (width + 7) // 8
        bitmap_rows   = []
        for row in range(height):
            row_bits = []
            for col in range(width):
                bi = offset + row * bytes_per_row + col // 8
                bt = 7 - (col % 8)
                row_bits.append((bdat[bi] >> bt) & 1 if bi < len(bdat) else 0)
            bitmap_rows.append(row_bits)
        offset += bytes_per_row * height

        glyphs.append({
            "codepoint":    cp,
            "width":        width,
            "height":       height,
            "device_width": device_width,
            "bitmap":       bitmap_rows,
        })

    if progress_cb: progress_cb(total, total)
    font["glyphs"] = sorted(glyphs, key=lambda g: g["codepoint"])
    return font

def write_fnt(font, out_path):
    glyphs   = font["glyphs"]
    name_enc = font["name"].encode("utf-8")
    with open(out_path, "wb") as f:
        f.write(b"FNT1")
        f.write(struct.pack("<H", 1))
        f.write(struct.pack("<H", len(name_enc)))
        f.write(name_enc)
        f.write(struct.pack("<H", font["max_width"]))
        f.write(struct.pack("<H", font["max_height"]))
        f.write(struct.pack("<H", font["baseline"]))
        f.write(struct.pack("<I", len(glyphs)))
        for g in glyphs:
            bmp_w  = g["width"]
            bmp_h  = g["height"]
            bitmap = g["bitmap"]
            f.write(struct.pack("<I", g["codepoint"]))
            f.write(struct.pack("<H", g["device_width"]))
            f.write(struct.pack("<H", bmp_w))
            f.write(struct.pack("<H", bmp_h))
            bytes_per_row = (bmp_w + 7) // 8
            for row in bitmap:
                for byte_i in range(bytes_per_row):
                    bv = 0
                    for bit_i in range(8):
                        col = byte_i * 8 + bit_i
                        if col < len(row) and row[col]:
                            bv |= (1 << (7 - bit_i))
                    f.write(bytes([bv]))
    return os.path.getsize(out_path)


# ── Theme ─────────────────────────────────────────────────────────────────

BG      = "#0d1117"
BG2     = "#161b22"
BG3     = "#21262d"
BORDER  = "#30363d"
FG      = "#e6edf3"
FG_DIM  = "#8b949e"
ACCENT  = "#58a6ff"
ACCENT2 = "#3fb950"
WARN    = "#d29922"
ERR     = "#f85149"

def sbtn(parent, text, command, color=ACCENT, **kw):
    fg = "#0d1117" if color in (ACCENT, ACCENT2) else FG
    return tk.Button(parent, text=text, command=command,
        bg=color, fg=fg, font=("Segoe UI", 9, "bold"),
        relief="flat", padx=14, pady=5, cursor="hand2",
        activebackground=color, activeforeground=fg, **kw)

def slbl(parent, text, color=FG, size=9, bold=False, **kw):
    return tk.Label(parent, text=text, bg=parent["bg"], fg=color,
        font=("Segoe UI", size, "bold" if bold else "normal"), **kw)

def sentry(parent, textvariable=None, width=40):
    return tk.Entry(parent, textvariable=textvariable, width=width,
        bg=BG3, fg=FG, insertbackground=FG, relief="flat",
        font=("Consolas", 10),
        highlightthickness=1, highlightcolor=ACCENT, highlightbackground=BORDER)


# ── App ───────────────────────────────────────────────────────────────────

class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("PF2 → FNT Converter — ModuOS Tools")
        self.configure(bg=BG)
        self.geometry("640x540")
        self.resizable(False, False)
        self.in_path   = tk.StringVar()
        self.out_path  = tk.StringVar()
        self.font_info = None
        self._build()

    def _build(self):
        # Header
        hdr = tk.Frame(self, bg=BG2, pady=16, padx=24)
        hdr.pack(fill="x")
        slbl(hdr, "PF2  →  FNT", color=FG, size=16, bold=True).pack(anchor="w")
        slbl(hdr, "Convert GRUB PF2 fonts to ModuOS FNT format",
            color=FG_DIM).pack(anchor="w", pady=(2,0))
        ttk.Separator(self).pack(fill="x")

        body = tk.Frame(self, bg=BG, padx=28, pady=20)
        body.pack(fill="both", expand=True)

        # Input
        self._sec(body, "Input")
        r = tk.Frame(body, bg=BG); r.pack(fill="x", pady=(4,12))
        sentry(r, self.in_path, 52).pack(side="left", ipady=5)
        sbtn(r, "Browse…", self._browse_in, BG3).pack(side="left", padx=(8,0))
        self.in_path.trace_add("write", lambda *_: self._on_in_change())

        # Output
        self._sec(body, "Output")
        r2 = tk.Frame(body, bg=BG); r2.pack(fill="x", pady=(4,12))
        sentry(r2, self.out_path, 52).pack(side="left", ipady=5)
        sbtn(r2, "Browse…", self._browse_out, BG3).pack(side="left", padx=(8,0))

        # Info card
        self._sec(body, "Font Info")
        self.info_card = tk.Frame(body, bg=BG3, padx=16, pady=12,
            highlightthickness=1, highlightbackground=BORDER)
        self.info_card.pack(fill="x", pady=(4,16))
        self.info_lbl = slbl(self.info_card,
            "No file loaded — select a .pf2 file above", FG_DIM)
        self.info_lbl.pack(anchor="w")

        # Progress
        self._sec(body, "Progress")
        pf = tk.Frame(body, bg=BG); pf.pack(fill="x", pady=(4,0))
        self.progress = ttk.Progressbar(pf, mode="determinate", length=580)
        st = ttk.Style(self); st.theme_use("clam")
        st.configure("TProgressbar",
            troughcolor=BG3, background=ACCENT,
            bordercolor=BORDER, lightcolor=ACCENT, darkcolor=ACCENT)
        self.progress.pack(fill="x")
        self.prog_lbl = slbl(body, "", FG_DIM, 8); self.prog_lbl.pack(anchor="w", pady=(2,0))

        # Bottom bar
        ttk.Separator(self).pack(fill="x", side="bottom")
        bot = tk.Frame(self, bg=BG2, pady=12, padx=24); bot.pack(fill="x", side="bottom")
        self.status_lbl = slbl(bot, "Ready", FG_DIM); self.status_lbl.pack(side="left")
        self.conv_btn = sbtn(bot, "Convert  →", self._start, ACCENT2)
        self.conv_btn.pack(side="right")
        self.conv_btn.config(state="disabled")
        sbtn(bot, "Open in Viewer", self._open_viewer, BG3).pack(side="right", padx=(0,8))

    def _sec(self, parent, text):
        f = tk.Frame(parent, bg=BG); f.pack(fill="x", pady=(4,0))
        slbl(f, text.upper(), FG_DIM, 8, True).pack(side="left")
        tk.Frame(f, bg=BORDER, height=1).pack(side="left", fill="x", expand=True, padx=(8,0))

    def _browse_in(self):
        p = filedialog.askopenfilename(
            title="Select PF2 font",
            filetypes=[("PF2 fonts", "*.pf2"), ("All files", "*.*")])
        if p:
            self.in_path.set(p)
            self.out_path.set(os.path.splitext(p)[0] + ".fnt")

    def _browse_out(self):
        p = filedialog.asksaveasfilename(
            title="Save FNT font", defaultextension=".fnt",
            filetypes=[("FNT fonts", "*.fnt"), ("All files", "*.*")])
        if p:
            self.out_path.set(p)

    def _on_in_change(self):
        p = self.in_path.get().strip()
        if not p or not os.path.isfile(p):
            self._set_info(None)
            self.conv_btn.config(state="disabled")
            return
        threading.Thread(target=self._load_info, args=(p,), daemon=True).start()

    def _load_info(self, path):
        try:
            with open(path, "rb") as f:
                data = f.read()
            font    = parse_pf2(data)
            sz      = os.path.getsize(path)
            self.after(0, lambda fo=font, s=sz: self._set_info(fo, s))
        except Exception as ex:
            msg = str(ex)
            self.after(0, lambda m=msg: self._set_info(None, error=m))

    def _set_info(self, font, in_size=0, error=None):
        if error:
            self.info_lbl.config(text=f"Error: {error}", fg=ERR)
            self.conv_btn.config(state="disabled")
            self.font_info = None
            return
        if font is None:
            self.info_lbl.config(
                text="No file loaded — select a .pf2 file above", fg=FG_DIM)
            self.conv_btn.config(state="disabled")
            self.font_info = None
            return
        self.font_info = font
        lines = [
            f"Name:        {font['name']}",
            f"Family:      {font['family']}",
            f"Point size:  {font['point_size']}pt    "
            f"Max size: {font['max_width']}x{font['max_height']}px    "
            f"Baseline: {font['baseline']}px",
            f"Glyphs:      {len(font['glyphs'])}    "
            f"Input: {in_size/1024:.1f} KB",
        ]
        self.info_lbl.config(text="\n".join(lines), fg=FG)
        self.conv_btn.config(state="normal")
        self._status(f"Loaded — {len(font['glyphs'])} glyphs", ACCENT2)

    def _start(self):
        in_p  = self.in_path.get().strip()
        out_p = self.out_path.get().strip()
        if not in_p or not os.path.isfile(in_p):
            messagebox.showerror("Error", "Input file not found."); return
        if not out_p:
            messagebox.showerror("Error", "Please specify an output path."); return
        self.conv_btn.config(state="disabled")
        self.progress["value"] = 0
        self._status("Converting…", WARN)
        threading.Thread(target=self._do_convert, args=(in_p, out_p), daemon=True).start()

    def _do_convert(self, in_p, out_p):
        try:
            with open(in_p, "rb") as f:
                data = f.read()

            def pcb(done, total):
                pct = int(done / total * 80) if total else 0
                msg = f"Parsing glyphs… {done}/{total}"
                self.after(0, lambda p=pct, m=msg: self._upd_prog(p, m))

            font     = parse_pf2(data, pcb)
            self.after(0, lambda: self._upd_prog(85, "Writing FNT…"))
            out_size = write_fnt(font, out_p)
            in_size  = os.path.getsize(in_p)
            self.after(0, lambda op=out_p, i=in_size, o=out_size: self._done(op, i, o))
        except Exception as ex:
            msg = str(ex)
            self.after(0, lambda m=msg: self._err(m))

    def _upd_prog(self, pct, msg):
        self.progress["value"] = pct
        self.prog_lbl.config(text=msg)

    def _done(self, out_p, in_size, out_size):
        self.progress["value"] = 100
        ratio = out_size / in_size * 100 if in_size else 0
        self.prog_lbl.config(
            text=f"Done!  {in_size/1024:.1f} KB  ->  {out_size/1024:.1f} KB  ({ratio:.0f}%)")
        self._status(f"Saved: {os.path.basename(out_p)}", ACCENT2)
        self.conv_btn.config(state="normal")
        n = len(self.font_info["glyphs"]) if self.font_info else "?"
        messagebox.showinfo("Done",
            f"Conversion complete!\n\nOutput: {out_p}\nSize:   {out_size/1024:.1f} KB  ({n} glyphs)")

    def _err(self, msg):
        self.progress["value"] = 0
        self.prog_lbl.config(text="")
        self._status(f"Error: {msg}", ERR)
        self.conv_btn.config(state="normal")
        messagebox.showerror("Conversion failed", msg)

    def _status(self, msg, color=FG_DIM):
        self.status_lbl.config(text=msg, fg=color)

    def _open_viewer(self):
        out_p = self.out_path.get().strip()
        if not out_p or not os.path.isfile(out_p):
            messagebox.showwarning("No output", "Convert a font first."); return
        try:
            import subprocess
            viewer = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fnt_viewer.py")
            if os.path.isfile(viewer):
                subprocess.Popen([sys.executable, viewer, out_p])
            else:
                messagebox.showinfo("Viewer not found",
                    "Place fnt_viewer.py in the same folder.")
        except Exception as ex:
            messagebox.showerror("Error", str(ex))


if __name__ == "__main__":
    App().mainloop()