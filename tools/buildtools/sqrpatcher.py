#!/usr/bin/env python3
"""sqrpatcher - convert PIE ELF binaries into SQR (Squirrel) format.

No third-party dependencies. Pure stdlib. Uses a thread pool to parallelize
per-symbol / per-segment work on large inputs.

Matches SQR-Format.md (0x98-byte header, GOT-backed IAT, interpreter string).
"""
import sys
import os
import struct
import argparse
from concurrent.futures import ThreadPoolExecutor

MAGIC = b"SQR\0"
VERSION = 1

ARCH_MAP = {0x3E: 1, 0xB7: 2, 0xF3: 3}  # EM_X86_64, EM_AARCH64, EM_RISCV -> SQR arch

SQR_FLAG_EXEC = 0x1
SQR_FLAG_LIB = 0x2
SQR_FLAG_HAS_TLS = 0x4
SQR_FLAG_SORTED_EXPORTS = 0x8

HEADER_SIZE = 0x98
SEG_ENTRY_SIZE = 0x20
IMPORT_ENTRY_SIZE = 0x18
EXPORT_ENTRY_SIZE = 0x18

DEFAULT_INTERP = b"ld-moduos.sqr"

ET_DYN = 3
PT_LOAD = 1
PT_TLS = 7
PT_DYNAMIC = 2

SHT_DYNSYM = 11
SHT_RELA = 4
SHT_REL = 9

STB_GLOBAL = 1
STB_WEAK = 2
STT_FUNC = 2
STT_OBJECT = 1

R_X86_64_JUMP_SLOT = 7
R_X86_64_GLOB_DAT = 6
R_X86_64_RELATIVE = 8
R_X86_64_TLS_TPOFF64 = 18
R_X86_64_TLS_DTPMOD64 = 16
R_X86_64_TLS_DTPOFF64 = 17
R_X86_64_IRELATIVE = 37

ACCEPT_RELA = {R_X86_64_RELATIVE, R_X86_64_JUMP_SLOT, R_X86_64_GLOB_DAT,
               R_X86_64_TLS_TPOFF64, R_X86_64_TLS_DTPMOD64, R_X86_64_TLS_DTPOFF64,
               R_X86_64_IRELATIVE}

IMPORT_RELOC_TYPES = (R_X86_64_JUMP_SLOT, R_X86_64_GLOB_DAT)


def fnv1a(data: bytes) -> int:
    h = 0xcbf29ce484222325
    for b in data:
        h ^= b
        h = (h * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


class ElfError(Exception):
    pass


class ElfFile:
    __slots__ = ("buf", "is64", "le", "e_type", "e_machine", "e_entry",
                 "phoff", "phnum", "phentsize", "shoff", "shnum", "shentsize",
                 "shstrndx")

    def __init__(self, buf: bytes):
        self.buf = buf
        if buf[:4] != b"\x7fELF":
            raise ElfError("not an ELF file")
        ei_class = buf[4]
        ei_data = buf[5]
        if ei_class != 2:
            raise ElfError("only 64-bit ELF supported")
        if ei_data != 1:
            raise ElfError("only little-endian ELF supported")
        self.is64 = True
        self.le = True
        (self.e_type, self.e_machine, _e_version, self.e_entry,
         phoff, shoff, _flags, _ehsize,
         self.phentsize, self.phnum, self.shentsize, self.shnum,
         self.shstrndx) = struct.unpack_from("<HHIQQQIHHHHHH", buf, 16)
        self.phoff = phoff
        self.shoff = shoff

    def program_headers(self):
        for i in range(self.phnum):
            off = self.phoff + i * self.phentsize
            p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align = \
                struct.unpack_from("<IIQQQQQQ", self.buf, off)
            yield dict(type=p_type, flags=p_flags, offset=p_offset, vaddr=p_vaddr,
                       filesz=p_filesz, memsz=p_memsz, align=p_align)

    def section_headers(self):
        for i in range(self.shnum):
            off = self.shoff + i * self.shentsize
            (sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size,
             sh_link, sh_info, sh_addralign, sh_entsize) = \
                struct.unpack_from("<IIQQQQIIQQ", self.buf, off)
            yield dict(name=sh_name, type=sh_type, flags=sh_flags, addr=sh_addr,
                       offset=sh_offset, size=sh_size, link=sh_link, info=sh_info,
                       entsize=sh_entsize)

    def shstrtab(self):
        secs = list(self.section_headers())
        s = secs[self.shstrndx]
        return self.buf[s["offset"]:s["offset"] + s["size"]]

    def get_str(self, strtab: bytes, off: int) -> bytes:
        end = strtab.find(b"\0", off)
        return strtab[off:end]


def parse_dynsym(elf: ElfFile):
    """Return (symbols list, dynstr bytes) using section headers."""
    secs = list(elf.section_headers())
    dynsym = None
    for s in secs:
        if s["type"] == SHT_DYNSYM:
            dynsym = s
    if dynsym is None:
        return [], b""
    dynstr = secs[dynsym["link"]]
    strtab = elf.buf[dynstr["offset"]:dynstr["offset"] + dynstr["size"]]
    entsize = dynsym["entsize"] or 24
    count = dynsym["size"] // entsize
    syms = []
    base = dynsym["offset"]
    for i in range(count):
        off = base + i * entsize
        st_name, st_info, st_other, st_shndx, st_value, st_size = \
            struct.unpack_from("<IBBHQQ", elf.buf, off)
        bind = st_info >> 4
        typ = st_info & 0xF
        name = elf.get_str(strtab, st_name)
        syms.append(dict(idx=i, name=name, bind=bind, type=typ,
                          shndx=st_shndx, value=st_value, size=st_size))
    return syms, strtab


def parse_rela_sections(elf: ElfFile):
    """Yield relocation entries (secname, r_offset, r_sym, r_type, r_addend)
    from all RELA/REL sections."""
    secs = list(elf.section_headers())
    shstr = elf.shstrtab()
    relocs = []
    for s in secs:
        name = elf.get_str(shstr, s["name"])
        if s["type"] == SHT_RELA:
            entsize = s["entsize"] or 24
            count = s["size"] // entsize
            base = s["offset"]
            for i in range(count):
                off = base + i * entsize
                r_offset, r_info, r_addend = struct.unpack_from("<QQq", elf.buf, off)
                r_sym = r_info >> 32
                r_type = r_info & 0xFFFFFFFF
                relocs.append((name, r_offset, r_sym, r_type, r_addend))
        elif s["type"] == SHT_REL:
            entsize = s["entsize"] or 16
            count = s["size"] // entsize
            base = s["offset"]
            for i in range(count):
                off = base + i * entsize
                r_offset, r_info = struct.unpack_from("<QQ", elf.buf, off)
                r_sym = r_info >> 32
                r_type = r_info & 0xFFFFFFFF
                relocs.append((name, r_offset, r_sym, r_type, 0))
    return relocs


class StringTableBuilder:
    """Deduplicating string table; safe to build in one thread, symbols computed in
    parallel first."""

    def __init__(self):
        self.buf = bytearray(b"\0")  # offset 0 = empty string
        self.cache = {b"": 0}

    def add(self, s: bytes) -> int:
        if s in self.cache:
            return self.cache[s]
        off = len(self.buf)
        self.buf += s + b"\0"
        self.cache[s] = off
        return off

    def bytes(self) -> bytes:
        return bytes(self.buf)


def extract_needed(elf: ElfFile):
    """Best-effort DT_NEEDED extraction from PT_DYNAMIC/.dynamic + .dynstr.
    Only useful if the input was linked with real DT_NEEDED entries; when apps
    are linked with --unresolved-symbols=ignore-all against no real .so, this
    will be empty and the caller should fall back to --default-module."""
    secs = list(elf.section_headers())
    shstr = elf.shstrtab()
    dynamic = None
    dynstr_sec = None
    for s in secs:
        name = elf.get_str(shstr, s["name"])
        if name == b".dynamic":
            dynamic = s
        elif name == b".dynstr":
            dynstr_sec = s
    if dynamic is None or dynstr_sec is None:
        return []
    dynstr = elf.buf[dynstr_sec["offset"]:dynstr_sec["offset"] + dynstr_sec["size"]]
    needed = []
    off = dynamic["offset"]
    end = off + dynamic["size"]
    DT_NEEDED = 1
    DT_NULL = 0
    while off < end:
        d_tag, d_val = struct.unpack_from("<qQ", elf.buf, off)
        if d_tag == DT_NULL:
            break
        if d_tag == DT_NEEDED:
            s = elf.get_str(dynstr, d_val).decode("utf-8", "replace")
            needed.append(s)
        off += 16
    return needed


def convert(input_path: str, output_path: str, module_id_hint: int, workers: int,
            verbose: bool, default_module: bytes, interp: bytes, force_interp: bool):
    with open(input_path, "rb") as f:
        buf = f.read()

    elf = ElfFile(buf)

    # SQR is PIC-only, no exceptions (SQR-Format.md "Design Goals" / sqrpatcher step 1).
    if elf.e_type != ET_DYN:
        raise ElfError(
            f"input is not ET_DYN (got e_type={elf.e_type}); SQR requires every image "
            f"to be built -fpic -pie. Statically-addressed (ET_EXEC) binaries are not "
            f"accepted — recompile/relink with -fpic -pie."
        )

    if elf.e_machine not in ARCH_MAP:
        raise ElfError(f"unsupported e_machine {elf.e_machine:#x}")
    sqr_arch = ARCH_MAP[elf.e_machine]

    phdrs = list(elf.program_headers())
    load_segs = [p for p in phdrs if p["type"] == PT_LOAD]
    tls_seg = next((p for p in phdrs if p["type"] == PT_TLS), None)

    relocs = parse_rela_sections(elf)

    # --- Verify PIC-ness: every RELA/REL entry must be an accepted relocation type ---
    def check_reloc(entry):
        _, _, _, r_type, _ = entry
        return r_type in ACCEPT_RELA

    if relocs:
        with ThreadPoolExecutor(max_workers=workers) as ex:
            results = list(ex.map(check_reloc, relocs))
        if not all(results):
            bad = [relocs[i] for i, ok in enumerate(results) if not ok][:5]
            raise ElfError(f"non-PIC relocation types found (not fully -fpic): {bad}")

    syms, dynstr = parse_dynsym(elf)

    # --- Map sym_idx -> real GOT slot vaddr (r_offset) for every JUMP_SLOT/GLOB_DAT
    # relocation. This vaddr IS the IAT slot location -- see "Import Address Table"
    # in SQR-Format.md. We do NOT invent a fresh sequential index; the whole point
    # is that the loader writes into the same memory the compiled code already
    # reads through, so the index must match reality. ---
    import_syms = {}  # sym_idx -> r_offset
    for (_secname, r_offset, r_sym, r_type, _addend) in relocs:
        if r_type in IMPORT_RELOC_TYPES and r_sym != 0:
            import_syms[r_sym] = r_offset

    def classify(sym):
        if sym["name"] == b"":
            return None
        if sym["idx"] in import_syms and sym["shndx"] == 0:
            kind = 0 if sym["type"] == STT_FUNC else 1
            return ("import", sym["name"], kind, sym["idx"])
        if sym["shndx"] != 0 and sym["bind"] in (STB_GLOBAL, STB_WEAK):
            kind = 0 if sym["type"] == STT_FUNC else 1
            return ("export", sym["name"], kind, sym["value"])
        return None

    with ThreadPoolExecutor(max_workers=workers) as ex:
        classified = list(ex.map(classify, syms))

    strtab = StringTableBuilder()

    # Resolve real needed module name via DT_NEEDED if present (best-effort).
    # Real app builds normally won't have DT_NEEDED (linked with
    # --unresolved-symbols=ignore-all against no actual .so), so this falls back
    # to --default-module (default: "libc").
    needed_names = extract_needed(elf)
    default_mod = needed_names[0].encode() if needed_names else default_module
    default_mod_off = strtab.add(default_mod)

    imports = []
    exports = []
    import_r_offsets = []  # real GOT vaddr per import, parallel to `imports`

    for c in classified:
        if c is None:
            continue
        kind_tag, name, kind, extra = c
        if kind_tag == "import":
            sym_idx = extra
            r_offset = import_syms[sym_idx]
            sym_off = strtab.add(name)
            imports.append(dict(module_off=default_mod_off, symbol_off=sym_off,
                                 kind=kind, min_version=(1 << 16), r_offset=r_offset))
            import_r_offsets.append(r_offset)
        else:
            sym_off = strtab.add(name)
            exports.append(dict(symbol_off=sym_off, kind=kind, value=extra,
                                 version=(1 << 16), name=name))

    # --- Derive iat_base / per-import slot index from real GOT vaddrs. ---
    if imports:
        iat_base = min(import_r_offsets)
        iat_top = max(import_r_offsets)
        for imp in imports:
            delta = imp["r_offset"] - iat_base
            if delta % 8 != 0:
                raise ElfError(
                    f"GOT slot at {imp['r_offset']:#x} is not 8-byte aligned relative "
                    f"to iat_base {iat_base:#x} -- unexpected GOT layout, cannot "
                    f"derive a slot index safely"
                )
            imp["iat_slot"] = delta // 8
        iat_count = (iat_top - iat_base) // 8 + 1
    else:
        iat_base = 0
        iat_count = 0

    # sort exports by (fnv1a_hash(name) << 32 | version)
    def sort_key(e):
        return (fnv1a(e["name"]) << 32) | (e["version"] & 0xFFFFFFFF)

    with ThreadPoolExecutor(max_workers=workers) as ex:
        keys = list(ex.map(sort_key, exports))
    exports_sorted = [e for _, e in sorted(zip(keys, exports), key=lambda t: t[0])]

    # --- Interpreter string: only needed if this image has imports to resolve. ---
    interp_off = 0
    if imports and (interp or force_interp):
        interp_off = strtab.add(interp or DEFAULT_INTERP)

    # --- lay out file ---
    seg_table_off = HEADER_SIZE
    seg_table_size = len(load_segs) * SEG_ENTRY_SIZE

    import_table_off = seg_table_off + seg_table_size
    import_table_size = len(imports) * IMPORT_ENTRY_SIZE

    export_table_off = import_table_off + import_table_size
    export_table_size = len(exports_sorted) * EXPORT_ENTRY_SIZE

    # NOTE: no separate IAT file region -- iat_base is a *vaddr into a segment*,
    # not a file offset. Nothing is emitted here for it.
    strtab_bytes = strtab.bytes()
    strtab_off = export_table_off + export_table_size
    strtab_size = len(strtab_bytes)

    tls_off = 0
    tls_size = 0
    tls_data = b""
    if tls_seg is not None:
        tls_data = buf[tls_seg["offset"]:tls_seg["offset"] + tls_seg["filesz"]]
        tls_off = strtab_off + strtab_size
        tls_size = tls_seg["memsz"]

    seg_data_start = (tls_off + len(tls_data)) if tls_seg is not None else (strtab_off + strtab_size)
    seg_data_start = (seg_data_start + 7) & ~7

    flags = SQR_FLAG_LIB  # every PIC image may in principle be imported from
    entry_off = 0
    if elf.e_entry != 0:
        flags |= SQR_FLAG_EXEC
        entry_off = elf.e_entry
    if tls_seg is not None:
        flags |= SQR_FLAG_HAS_TLS
    flags |= SQR_FLAG_SORTED_EXPORTS

    out = bytearray()
    out += MAGIC
    out += struct.pack("<HH", VERSION, sqr_arch)
    out += struct.pack("<I", flags)
    out += struct.pack("<I", 0)
    out += struct.pack("<Q", entry_off)
    out += struct.pack("<I", module_id_hint)
    out += struct.pack("<I", 0)
    out += struct.pack("<I", len(load_segs))
    out += struct.pack("<I", 0)
    out += struct.pack("<Q", seg_table_off)
    out += struct.pack("<I", len(imports))
    out += struct.pack("<I", 0)
    out += struct.pack("<Q", import_table_off)
    out += struct.pack("<I", len(exports_sorted))
    out += struct.pack("<I", 0)
    out += struct.pack("<Q", export_table_off)
    out += struct.pack("<I", iat_count)
    out += struct.pack("<I", 0)
    out += struct.pack("<Q", iat_base)                # vaddr, not file offset
    out += struct.pack("<Q", strtab_off)
    out += struct.pack("<Q", strtab_size)
    out += struct.pack("<Q", tls_off if tls_seg is not None else 0)
    out += struct.pack("<Q", tls_size)
    out += struct.pack("<Q", 0)  # init offset (TODO: walk .init_array)
    out += struct.pack("<Q", 0)  # fini offset
    out += struct.pack("<I", interp_off)
    out += struct.pack("<I", 0)
    assert len(out) == HEADER_SIZE, len(out)

    seg_file_offsets = []
    cur = seg_data_start
    for seg in load_segs:
        seg_file_offsets.append(cur)
        cur += seg["filesz"]
        cur = (cur + 7) & ~7

    for seg, foff in zip(load_segs, seg_file_offsets):
        perms = 0
        if seg["flags"] & 0x4:
            perms |= 0x1  # R
        if seg["flags"] & 0x2:
            perms |= 0x2  # W
        if seg["flags"] & 0x1:
            perms |= 0x4  # X
        out += struct.pack("<QQQII", seg["vaddr"], seg["filesz"], seg["memsz"], perms, seg["align"])

    for imp in imports:
        out += struct.pack("<IIIIII", imp["module_off"], imp["symbol_off"], imp["iat_slot"],
                            imp["kind"], imp["min_version"], 0)

    for exp in exports_sorted:
        out += struct.pack("<IIQII", exp["symbol_off"], exp["kind"], exp["value"], exp["version"], 0)

    out += strtab_bytes

    if tls_seg is not None:
        out += tls_data
        pad = tls_off + len(tls_data)
        pad_needed = seg_data_start - pad
        if pad_needed > 0:
            out += b"\0" * pad_needed
    else:
        pad_needed = seg_data_start - len(out)
        if pad_needed > 0:
            out += b"\0" * pad_needed

    for seg, foff in zip(load_segs, seg_file_offsets):
        data = buf[seg["offset"]:seg["offset"] + seg["filesz"]]
        cur_len = len(out)
        if cur_len < foff:
            out += b"\0" * (foff - cur_len)
        out += data
        pad = (8 - (len(out) % 8)) % 8
        out += b"\0" * pad

    with open(output_path, "wb") as f:
        f.write(out)

    if verbose:
        sys.stderr.write(
            f"sqrpatcher: {input_path} -> {output_path} "
            f"({len(load_segs)} segs, {len(imports)} imports [iat_base={iat_base:#x}, "
            f"iat_count={iat_count}], {len(exports_sorted)} exports, "
            f"interp={'(none)' if not interp_off else (interp or DEFAULT_INTERP).decode()}, "
            f"{len(out)} bytes)\n"
        )


def main():
    ap = argparse.ArgumentParser(prog="sqrpatcher", description="Convert PIE ELF -> SQR")
    ap.add_argument("input", help="input ELF file (must be ET_DYN, -fpic -pie)")
    ap.add_argument("-o", "--output", required=True, help="output .sqr file")
    ap.add_argument("--module-id", type=lambda x: int(x, 0), default=0,
                     help="advisory module ID hint (uint32)")
    ap.add_argument("--default-module", default="libc",
                     help="module name to attribute unresolved imports to when the "
                          "ELF has no DT_NEEDED entries (default: libc)")
    ap.add_argument("--interp", default=None,
                     help="interpreter module name to embed if this image has imports "
                          "(default: ld-moduos.sqr). Pass an empty string to force no "
                          "interpreter even with imports (advanced/embedding use only).")
    ap.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 4,
                     help="worker threads")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    interp_bytes = None
    force_interp = True
    if args.interp is not None:
        if args.interp == "":
            force_interp = False
        else:
            interp_bytes = args.interp.encode()

    try:
        convert(args.input, args.output, args.module_id, max(1, args.jobs), args.verbose,
                args.default_module.encode(), interp_bytes, force_interp)
    except ElfError as e:
        sys.stderr.write(f"sqrpatcher: error: {e}\n")
        sys.exit(1)
    except FileNotFoundError as e:
        sys.stderr.write(f"sqrpatcher: error: {e}\n")
        sys.exit(1)


if __name__ == "__main__":
    main()