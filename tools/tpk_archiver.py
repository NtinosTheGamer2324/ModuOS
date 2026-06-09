import struct, os, sys, threading
import tkinter as tk
from tkinter import ttk, filedialog, messagebox

# ─── TPK core ───────────────────────────────────────────────────────────────

MAGIC        = 0x214B5054
ENTRY_SIZE   = 264
HEADER_SIZE  = 8
FILENAME_LEN = 256


def tpk_pack(output_path: str, input_paths: list) -> str:
    entries = []
    for path in input_paths:
        name = os.path.basename(path)
        if len(name.encode()) > FILENAME_LEN - 1:
            raise ValueError(f"Filename too long: {name}")
        entries.append((name, path))

    data_start = HEADER_SIZE + ENTRY_SIZE * len(entries)
    offsets, cursor = [], data_start
    for _, path in entries:
        size = os.path.getsize(path)
        offsets.append((cursor, size))
        cursor += size

    with open(output_path, "wb") as f:
        f.write(struct.pack("<II", MAGIC, len(entries)))
        for (name, _), (offset, size) in zip(entries, offsets):
            nb = name.encode("utf-8")[: FILENAME_LEN - 1]
            f.write(nb + b"\x00" * (FILENAME_LEN - len(nb)))
            f.write(struct.pack("<II", offset, size))
        for _, path in entries:
            with open(path, "rb") as src:
                f.write(src.read())

    total = os.path.getsize(output_path)
    return f"Packed {len(entries)} file(s) → {output_path}  ({total:,} bytes)"


def tpk_read(archive_path: str):
    with open(archive_path, "rb") as f:
        hdr = f.read(HEADER_SIZE)
        if len(hdr) < HEADER_SIZE:
            raise ValueError("File too small.")
        magic, count = struct.unpack("<II", hdr)
        if magic != MAGIC:
            raise ValueError(f"Not a TPK file (magic 0x{magic:08X})")
        entries = []
        for _ in range(count):
            raw = f.read(ENTRY_SIZE)
            nb  = raw[:FILENAME_LEN]
            nul = nb.find(b"\x00")
            name = nb[:nul].decode("utf-8", errors="replace")
            offset, size = struct.unpack("<II", raw[256:264])
            entries.append({"name": name, "offset": offset, "size": size})
    return entries


def tpk_extract(archive_path: str, entries: list, out_dir: str, names=None):
    targets = [e for e in entries if names is None or e["name"] in names]
    with open(archive_path, "rb") as f:
        for e in targets:
            f.seek(e["offset"])
            data = f.read(e["size"])
            dest = os.path.join(out_dir, e["name"])
            with open(dest, "wb") as out:
                out.write(data)
    return len(targets)


# ─── Palette / style ─────────────────────────────────────────────────────────

BG      = "#1a1a1a"
SURFACE = "#242424"
CARD    = "#2e2e2e"
BORDER  = "#3a3a3a"
ACCENT  = "#4fc08d"          # teal-ish green
ACCENT2 = "#f0975e"          # amber for warnings
FG      = "#e8e6e0"
FG2     = "#888780"
FONT    = ("Consolas", 10)
FONTB   = ("Consolas", 10, "bold")
FONTH   = ("Consolas", 12, "bold")
FONTS   = ("Consolas", 9)


def styled_btn(parent, text, command, accent=False, danger=False, **kw):
    bg = ACCENT if accent else ("#c0392b" if danger else CARD)
    fg = "#0a1a12" if accent else FG
    hbg = "#3aad78" if accent else ("#a93226" if danger else "#3a3a3a")
    b = tk.Button(
        parent, text=text, command=command,
        bg=bg, fg=fg, activebackground=hbg, activeforeground=fg,
        relief="flat", bd=0, padx=14, pady=6,
        font=FONTB, cursor="hand2", **kw
    )
    b.bind("<Enter>", lambda e: b.config(bg=hbg))
    b.bind("<Leave>", lambda e: b.config(bg=bg))
    return b


def separator(parent, **kw):
    return tk.Frame(parent, bg=BORDER, height=1, **kw)


# ─── Archiver Tab ────────────────────────────────────────────────────────────

class ArchiverTab(tk.Frame):
    def __init__(self, master):
        super().__init__(master, bg=BG)
        self._files = []
        self._build()

    def _build(self):
        # Output path
        hdr = tk.Frame(self, bg=BG)
        hdr.pack(fill="x", padx=20, pady=(18, 0))
        tk.Label(hdr, text="OUTPUT ARCHIVE", font=FONTS, fg=FG2, bg=BG).pack(anchor="w")

        row = tk.Frame(self, bg=BG)
        row.pack(fill="x", padx=20, pady=(4, 0))
        self._out_var = tk.StringVar()
        tk.Entry(row, textvariable=self._out_var, font=FONT,
                 bg=CARD, fg=FG, insertbackground=FG,
                 relief="flat", bd=0, highlightthickness=1,
                 highlightbackground=BORDER, highlightcolor=ACCENT
                 ).pack(side="left", fill="x", expand=True, ipady=6, padx=(0, 8))
        styled_btn(row, "Browse…", self._browse_out).pack(side="left")

        separator(self).pack(fill="x", padx=20, pady=14)

        # File list header
        fhdr = tk.Frame(self, bg=BG)
        fhdr.pack(fill="x", padx=20)
        tk.Label(fhdr, text="FILES TO PACK", font=FONTS, fg=FG2, bg=BG).pack(side="left", anchor="w")
        self._count_lbl = tk.Label(fhdr, text="0 files", font=FONTS, fg=ACCENT, bg=BG)
        self._count_lbl.pack(side="right")

        # Listbox
        lf = tk.Frame(self, bg=CARD, bd=0, highlightthickness=1,
                      highlightbackground=BORDER)
        lf.pack(fill="both", expand=True, padx=20, pady=(6, 0))
        self._lb = tk.Listbox(lf, font=FONT, bg=CARD, fg=FG,
                               selectbackground=ACCENT, selectforeground="#0a1a12",
                               relief="flat", bd=0, activestyle="none",
                               highlightthickness=0)
        sb = tk.Scrollbar(lf, orient="vertical", command=self._lb.yview,
                          bg=CARD, troughcolor=CARD, width=8, relief="flat")
        self._lb.config(yscrollcommand=sb.set)
        self._lb.pack(side="left", fill="both", expand=True)
        sb.pack(side="right", fill="y")

        # Drag hint
        self._lb.bind("<Button-1>", lambda e: None)

        # Buttons row
        br = tk.Frame(self, bg=BG)
        br.pack(fill="x", padx=20, pady=10)
        styled_btn(br, "+ Add Files", self._add_files, accent=True).pack(side="left", padx=(0, 8))
        styled_btn(br, "Remove Selected", self._remove_sel).pack(side="left", padx=(0, 8))
        styled_btn(br, "Clear All", self._clear).pack(side="left")

        separator(self).pack(fill="x", padx=20, pady=(6, 0))

        # Pack button + log
        pb = tk.Frame(self, bg=BG)
        pb.pack(fill="x", padx=20, pady=10)
        styled_btn(pb, "▶  Pack Archive", self._pack, accent=True).pack(side="left")
        self._status = tk.Label(pb, text="", font=FONT, fg=ACCENT, bg=BG)
        self._status.pack(side="left", padx=14)

        # Log
        loglf = tk.Frame(self, bg=CARD, bd=0, highlightthickness=1,
                         highlightbackground=BORDER)
        loglf.pack(fill="x", padx=20, pady=(0, 18))
        self._log = tk.Text(loglf, font=FONTS, bg=CARD, fg=FG2,
                            state="disabled", height=4, relief="flat",
                            bd=0, highlightthickness=0, wrap="word")
        self._log.pack(fill="x", padx=8, pady=6)

    # helpers
    def _log_write(self, msg, color=None):
        self._log.config(state="normal")
        tag = f"c{id(msg)}"
        self._log.insert("end", msg + "\n", tag)
        if color:
            self._log.tag_config(tag, foreground=color)
        self._log.see("end")
        self._log.config(state="disabled")

    def _update_count(self):
        n = len(self._files)
        self._count_lbl.config(text=f"{n} file{'s' if n != 1 else ''}")

    def _browse_out(self):
        p = filedialog.asksaveasfilename(
            defaultextension=".tpk",
            filetypes=[("TPK Archive", "*.tpk"), ("All Files", "*.*")],
            title="Save TPK Archive As"
        )
        if p:
            self._out_var.set(p)

    def _add_files(self):
        paths = filedialog.askopenfilenames(title="Select Files to Pack")
        for p in paths:
            if p not in self._files:
                self._files.append(p)
                self._lb.insert("end", f"  {os.path.basename(p):<40}  {os.path.getsize(p):>10,} B   {p}")
        self._update_count()

    def _remove_sel(self):
        for i in reversed(self._lb.curselection()):
            self._files.pop(i)
            self._lb.delete(i)
        self._update_count()

    def _clear(self):
        self._files.clear()
        self._lb.delete(0, "end")
        self._update_count()

    def _pack(self):
        out = self._out_var.get().strip()
        if not out:
            messagebox.showwarning("No output", "Please choose an output .tpk path.")
            return
        if not self._files:
            messagebox.showwarning("No files", "Add at least one file to pack.")
            return
        self._status.config(text="Packing…", fg=FG2)
        self.update_idletasks()

        def do():
            try:
                msg = tpk_pack(out, self._files)
                self.after(0, lambda: self._status.config(text="Done ✓", fg=ACCENT))
                self.after(0, lambda: self._log_write(msg, ACCENT))
            except Exception as e:
                self.after(0, lambda: self._status.config(text="Error", fg=ACCENT2))
                self.after(0, lambda: self._log_write(f"Error: {e}", ACCENT2))

        threading.Thread(target=do, daemon=True).start()


# ─── Viewer Tab ──────────────────────────────────────────────────────────────

class ViewerTab(tk.Frame):
    def __init__(self, master):
        super().__init__(master, bg=BG)
        self._entries = []
        self._arc_path = ""
        self._build()

    def _build(self):
        # Archive path
        hdr = tk.Frame(self, bg=BG)
        hdr.pack(fill="x", padx=20, pady=(18, 0))
        tk.Label(hdr, text="ARCHIVE TO OPEN", font=FONTS, fg=FG2, bg=BG).pack(anchor="w")

        row = tk.Frame(self, bg=BG)
        row.pack(fill="x", padx=20, pady=(4, 0))
        self._arc_var = tk.StringVar()
        tk.Entry(row, textvariable=self._arc_var, font=FONT,
                 bg=CARD, fg=FG, insertbackground=FG,
                 relief="flat", bd=0, highlightthickness=1,
                 highlightbackground=BORDER, highlightcolor=ACCENT
                 ).pack(side="left", fill="x", expand=True, ipady=6, padx=(0, 8))
        styled_btn(row, "Open…", self._browse_arc, accent=True).pack(side="left")
        styled_btn(row, "Reload", self._load).pack(side="left", padx=(6, 0))

        separator(self).pack(fill="x", padx=20, pady=14)

        # Info bar
        self._info_var = tk.StringVar(value="No archive loaded.")
        tk.Label(self, textvariable=self._info_var, font=FONTB,
                 fg=ACCENT, bg=BG, anchor="w").pack(fill="x", padx=20)

        # File table
        cols = ("name", "offset", "size")
        tf = tk.Frame(self, bg=CARD, bd=0, highlightthickness=1,
                      highlightbackground=BORDER)
        tf.pack(fill="both", expand=True, padx=20, pady=(8, 0))

        style = ttk.Style()
        style.theme_use("clam")
        style.configure("Tpk.Treeview",
                        background=CARD, foreground=FG,
                        fieldbackground=CARD, rowheight=24,
                        font=FONT, borderwidth=0)
        style.configure("Tpk.Treeview.Heading",
                        background=SURFACE, foreground=FG2,
                        font=FONTS, relief="flat", borderwidth=0)
        style.map("Tpk.Treeview",
                  background=[("selected", ACCENT)],
                  foreground=[("selected", "#0a1a12")])
        style.configure("Tpk.Vertical.TScrollbar",
                        background=CARD, troughcolor=CARD,
                        arrowcolor=FG2, borderwidth=0, relief="flat")

        self._tree = ttk.Treeview(tf, columns=cols, show="headings",
                                   style="Tpk.Treeview", selectmode="extended")
        vsb = ttk.Scrollbar(tf, orient="vertical",
                            command=self._tree.yview, style="Tpk.Vertical.TScrollbar")
        self._tree.configure(yscrollcommand=vsb.set)

        self._tree.heading("name",   text="Filename",    anchor="w")
        self._tree.heading("offset", text="Offset",      anchor="e")
        self._tree.heading("size",   text="Size (bytes)", anchor="e")
        self._tree.column("name",   width=340, anchor="w")
        self._tree.column("offset", width=120, anchor="e")
        self._tree.column("size",   width=130, anchor="e")

        self._tree.pack(side="left", fill="both", expand=True)
        vsb.pack(side="right", fill="y")

        # Action row
        ar = tk.Frame(self, bg=BG)
        ar.pack(fill="x", padx=20, pady=10)
        styled_btn(ar, "Extract Selected", self._extract_sel, accent=True).pack(side="left", padx=(0, 8))
        styled_btn(ar, "Extract All", self._extract_all).pack(side="left", padx=(0, 8))
        self._sel_lbl = tk.Label(ar, text="", font=FONTS, fg=FG2, bg=BG)
        self._sel_lbl.pack(side="right")

        self._tree.bind("<<TreeviewSelect>>", self._on_select)

        # Status log
        loglf = tk.Frame(self, bg=CARD, bd=0, highlightthickness=1,
                         highlightbackground=BORDER)
        loglf.pack(fill="x", padx=20, pady=(0, 18))
        self._log = tk.Text(loglf, font=FONTS, bg=CARD, fg=FG2,
                            state="disabled", height=4, relief="flat",
                            bd=0, highlightthickness=0, wrap="word")
        self._log.pack(fill="x", padx=8, pady=6)

    def _log_write(self, msg, color=None):
        self._log.config(state="normal")
        tag = f"c{id(msg)}"
        self._log.insert("end", msg + "\n", tag)
        if color:
            self._log.tag_config(tag, foreground=color)
        self._log.see("end")
        self._log.config(state="disabled")

    def _on_select(self, _=None):
        n = len(self._tree.selection())
        self._sel_lbl.config(text=f"{n} selected" if n else "")

    def _browse_arc(self):
        p = filedialog.askopenfilename(
            filetypes=[("TPK Archive", "*.tpk"), ("All Files", "*.*")],
            title="Open TPK Archive"
        )
        if p:
            self._arc_var.set(p)
            self._load()

    def _load(self):
        path = self._arc_var.get().strip()
        if not path:
            return
        try:
            entries = tpk_read(path)
            self._entries  = entries
            self._arc_path = path
            self._tree.delete(*self._tree.get_children())
            total = 0
            for e in entries:
                self._tree.insert("", "end",
                                  values=(e["name"], f"0x{e['offset']:08X}", f"{e['size']:,}"))
                total += e["size"]
            arc_size = os.path.getsize(path)
            self._info_var.set(
                f"{os.path.basename(path)}   ·   {len(entries)} file(s)   ·   "
                f"archive {arc_size:,} B   ·   data {total:,} B"
            )
            self._log_write(f"Loaded: {path}", ACCENT)
        except Exception as ex:
            self._info_var.set("Failed to load archive.")
            self._log_write(f"Error: {ex}", ACCENT2)

    def _extract_sel(self):
        sel = self._tree.selection()
        if not sel:
            messagebox.showinfo("Nothing selected", "Select files in the list first.")
            return
        names = {self._tree.item(s)["values"][0] for s in sel}
        self._do_extract(names)

    def _extract_all(self):
        if not self._entries:
            messagebox.showinfo("Empty", "Load an archive first.")
            return
        self._do_extract(None)

    def _do_extract(self, names):
        out_dir = filedialog.askdirectory(title="Extract To…")
        if not out_dir:
            return
        try:
            n = tpk_extract(self._arc_path, self._entries, out_dir, names)
            self._log_write(f"Extracted {n} file(s) → {out_dir}", ACCENT)
        except Exception as ex:
            self._log_write(f"Error: {ex}", ACCENT2)


# ─── Main window ─────────────────────────────────────────────────────────────

class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("TPK — Tiny Package Archive")
        self.geometry("740x600")
        self.minsize(620, 480)
        self.configure(bg=BG)
        self._build()

    def _build(self):
        # Title bar
        tb = tk.Frame(self, bg=SURFACE, height=48)
        tb.pack(fill="x")
        tk.Label(tb, text="TPK!", font=("Consolas", 16, "bold"),
                 fg=ACCENT, bg=SURFACE).pack(side="left", padx=18, pady=10)
        tk.Label(tb, text="Tiny Package Archive",
                 font=FONT, fg=FG2, bg=SURFACE).pack(side="left")

        separator(self).pack(fill="x")

        # Tab switcher
        ts = tk.Frame(self, bg=SURFACE)
        ts.pack(fill="x")
        self._tab_btns = {}
        self._tabs     = {}
        for name, label in [("archiver", "▾  Archiver"), ("viewer", "▸  Viewer")]:
            b = tk.Label(ts, text=label, font=FONTB, fg=FG2, bg=SURFACE,
                         padx=20, pady=10, cursor="hand2")
            b.pack(side="left")
            b.bind("<Button-1>", lambda e, n=name: self._switch(n))
            self._tab_btns[name] = b

        separator(self).pack(fill="x")

        # Frames
        self._tabs["archiver"] = ArchiverTab(self)
        self._tabs["viewer"]   = ViewerTab(self)

        self._switch("archiver")

    def _switch(self, name):
        for n, f in self._tabs.items():
            if n == name:
                f.pack(fill="both", expand=True)
                self._tab_btns[n].config(fg=ACCENT,
                    font=("Consolas", 10, "bold"))
            else:
                f.pack_forget()
                self._tab_btns[n].config(fg=FG2,
                    font=("Consolas", 10))


if __name__ == "__main__":
    app = App()
    app.mainloop()