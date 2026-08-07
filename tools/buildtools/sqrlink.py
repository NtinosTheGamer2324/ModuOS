#!/usr/bin/env python3
"""sqrlink - patch imports into/out of an existing SQR file, and validate
import->export resolution against other SQR modules on disk.

Does NOT touch code. Only ever *appends* new IAT slots for new imports and
*removes import-table entries* (never renumbers existing slots) so code
already compiled against the old IAT layout keeps working.

No third-party dependencies.
"""
import sys
import struct
import argparse
import os

MAGIC = b"SQR\0"
HEADER_FMT = "<4sHHIIQIIIIQIIQIIQIIQQQQQQQ"
HEADER_SIZE = 0x90
SEG_FMT = "<QQQII"
SEG_SIZE = 0x20
IMP_FMT = "<IIIIII"
IMP_SIZE = 0x18
EXP_FMT = "<IIQII"
EXP_SIZE = 0x18

KIND_NAMES = {0: "func", 1: "data"}
KIND_VALS = {"func": 0, "data": 1, "FUNC": 0, "DATA": 1}


def cstr(buf, off):
    end = buf.find(b"\0", off)
    return buf[off:end]


class StrBuilder:
    def __init__(self):
        self.buf = bytearray(b"\0")
        self.cache = {b"": 0}

    def add(self, s: bytes) -> int:
        if s in self.cache:
            return self.cache[s]
        off = len(self.buf)
        self.buf += s + b"\0"
        self.cache[s] = off
        return off

    def bytes(self):
        return bytes(self.buf)


def parse_version(s):
    if isinstance(s, int):
        return s
    major, _, minor = s.partition(".")
    return (int(major) << 16) | (int(minor or 0) & 0xFFFF)


def ver_str(v):
    return f"{v >> 16}.{v & 0xffff}"


class SqrImage:
    """Full in-memory representation of an SQR file, round-trippable."""

    def __init__(self, path):
        with open(path, "rb") as f:
            buf = f.read()
        if buf[:4] != MAGIC:
            raise ValueError(f"{path}: not an SQR file")
        (magic, version, arch, flags, _r0, entry_off, mod_hint, _r1,
         seg_count, _r2, seg_off, imp_count, _r3, imp_off,
         exp_count, _r4, exp_off, iat_count, _r5, iat_off,
         str_off, str_size, tls_off, tls_size, init_off, fini_off) = \
            struct.unpack_from(HEADER_FMT, buf, 0)

        self.version, self.arch, self.flags = version, arch, flags
        self.entry_off, self.mod_hint = entry_off, mod_hint
        self.init_off, self.fini_off = init_off, fini_off

        strtab = buf[str_off:str_off + str_size]

        self.segs = []
        for i in range(seg_count):
            o = seg_off + i * SEG_SIZE
            vaddr, filesz, memsz, perms, align = struct.unpack_from(SEG_FMT, buf, o)
            self.segs.append(dict(vaddr=vaddr, filesz=filesz, memsz=memsz,
                                   perms=perms, align=align))

        self.imports = []
        for i in range(imp_count):
            o = imp_off + i * IMP_SIZE
            mod_o, sym_o, slot, kind, minver, _r = struct.unpack_from(IMP_FMT, buf, o)
            self.imports.append(dict(module=cstr(strtab, mod_o), symbol=cstr(strtab, sym_o),
                                      slot=slot, kind=kind, min_version=minver))

        self.exports = []
        for i in range(exp_count):
            o = exp_off + i * EXP_SIZE
            sym_o, kind, value, ver, _r = struct.unpack_from(EXP_FMT, buf, o)
            self.exports.append(dict(symbol=cstr(strtab, sym_o), kind=kind, value=value, version=ver))

        self.iat_count = iat_count
        self.tls_data = buf[tls_off:tls_off + len(buf[tls_off:])] if tls_off else b""
        # tls template stored size is tls_size (memsz); file bytes may be shorter if bss-tail
        if tls_off:
            self.tls_data = buf[tls_off:tls_off + tls_size]
        self.tls_size = tls_size
        self.has_tls = bool(tls_off)

        # segment file data, recovered by re-deriving the same layout sqrpatcher used:
        # segments are stored last, back-to-back, 8-byte aligned, in header order.
        # We stored seg_off table already; grab raw bytes from wherever the *data*
        # actually lives by re-walking file offsets the same way sqrpatcher laid them out.
        # Simplest robust method: segment data starts right after strtab/tls, 8-aligned,
        # and each segment's filesz bytes follow in order, individually 8-aligned after.
        data_start = str_off + str_size
        if tls_off:
            data_start = tls_off + tls_size
        data_start = (data_start + 7) & ~7
        cur = data_start
        self.seg_data = []
        for seg in self.segs:
            self.seg_data.append(buf[cur:cur + seg["filesz"]])
            cur += seg["filesz"]
            cur = (cur + 7) & ~7

    def max_slot(self):
        return max((i["slot"] for i in self.imports), default=-1)

    def write(self, path):
        strb = StrBuilder()
        header_size = HEADER_SIZE
        seg_table_off = header_size
        seg_table_size = len(self.segs) * SEG_SIZE

        imp_table_off = seg_table_off + seg_table_size
        imp_table_size = len(self.imports) * IMP_SIZE

        exp_table_off = imp_table_off + imp_table_size
        exp_table_size = len(self.exports) * EXP_SIZE

        iat_off = exp_table_off + exp_table_size
        iat_size = self.iat_count * 8

        # pre-register strings
        imp_encoded = []
        for imp in self.imports:
            mo = strb.add(imp["module"])
            so = strb.add(imp["symbol"])
            imp_encoded.append((mo, so, imp["slot"], imp["kind"], imp["min_version"]))

        exp_encoded = []
        for exp in self.exports:
            so = strb.add(exp["symbol"])
            exp_encoded.append((so, exp["kind"], exp["value"], exp["version"]))

        strtab_bytes = strb.bytes()
        str_off = iat_off + iat_size
        str_size = len(strtab_bytes)

        tls_off = 0
        if self.has_tls:
            tls_off = str_off + str_size

        data_start = (tls_off + self.tls_size) if self.has_tls else (str_off + str_size)
        data_start = (data_start + 7) & ~7

        flags = self.flags
        out = bytearray()
        out += MAGIC
        out += struct.pack("<HH", self.version, self.arch)
        out += struct.pack("<I", flags)
        out += struct.pack("<I", 0)
        out += struct.pack("<Q", self.entry_off)
        out += struct.pack("<I", self.mod_hint)
        out += struct.pack("<I", 0)
        out += struct.pack("<I", len(self.segs))
        out += struct.pack("<I", 0)
        out += struct.pack("<Q", seg_table_off)
        out += struct.pack("<I", len(self.imports))
        out += struct.pack("<I", 0)
        out += struct.pack("<Q", imp_table_off)
        out += struct.pack("<I", len(self.exports))
        out += struct.pack("<I", 0)
        out += struct.pack("<Q", exp_table_off)
        out += struct.pack("<I", self.iat_count)
        out += struct.pack("<I", 0)
        out += struct.pack("<Q", iat_off)
        out += struct.pack("<Q", str_off)
        out += struct.pack("<Q", str_size)
        out += struct.pack("<Q", tls_off)
        out += struct.pack("<Q", self.tls_size)
        out += struct.pack("<Q", self.init_off)
        out += struct.pack("<Q", self.fini_off)
        assert len(out) == header_size

        for seg in self.segs:
            out += struct.pack(SEG_FMT, seg["vaddr"], seg["filesz"], seg["memsz"],
                                seg["perms"], seg["align"])

        for mo, so, slot, kind, minver in imp_encoded:
            out += struct.pack(IMP_FMT, mo, so, slot, kind, minver, 0)

        for so, kind, value, ver in exp_encoded:
            out += struct.pack(EXP_FMT, so, kind, value, ver, 0)

        out += b"\0" * iat_size
        out += strtab_bytes

        if self.has_tls:
            out += self.tls_data
            pad = (tls_off + len(self.tls_data))
            if data_start - pad > 0:
                out += b"\0" * (data_start - pad)
        else:
            if data_start - len(out) > 0:
                out += b"\0" * (data_start - len(out))

        for seg, data in zip(self.segs, self.seg_data):
            cur_len = len(out)
            out += data
            pad = (8 - (len(out) % 8)) % 8
            out += b"\0" * pad

        with open(path, "wb") as f:
            f.write(out)


def cmd_list(args):
    img = SqrImage(args.file)
    print(f"Imports ({len(img.imports)}):")
    print(f"  {'Slot':>4}  {'Kind':4}  {'Module':<16} {'Symbol':<24} MinVer")
    for i in img.imports:
        print(f"  {i['slot']:4d}  {KIND_NAMES.get(i['kind'], i['kind']):4}  "
              f"{i['module'].decode():<16} {i['symbol'].decode():<24} {ver_str(i['min_version'])}")


def cmd_add_import(args):
    img = SqrImage(args.file)
    module = args.module.encode()
    symbol = args.symbol.encode()
    kind = KIND_VALS[args.kind]
    min_version = parse_version(args.min_version)

    for i in img.imports:
        if i["module"] == module and i["symbol"] == symbol:
            sys.stderr.write(f"sqrlink: {args.file}: import {args.module}:{args.symbol} already present (slot {i['slot']})\n")
            sys.exit(1)

    new_slot = img.iat_count  # always append, never reuse/renumber
    img.imports.append(dict(module=module, symbol=symbol, slot=new_slot,
                             kind=kind, min_version=min_version))
    img.iat_count += 1

    out = args.output or args.file
    img.write(out)
    sys.stderr.write(f"sqrlink: added import {args.module}:{args.symbol} "
                      f"(kind={args.kind}, min_version={ver_str(min_version)}) -> slot {new_slot}, wrote {out}\n")


def cmd_remove_import(args):
    img = SqrImage(args.file)
    symbol = args.symbol.encode()
    module = args.module.encode() if args.module else None

    before = len(img.imports)
    kept = []
    removed = []
    for i in img.imports:
        if i["symbol"] == symbol and (module is None or i["module"] == module):
            removed.append(i)
        else:
            kept.append(i)

    if not removed:
        sys.stderr.write(f"sqrlink: {args.file}: no import matching symbol '{args.symbol}'"
                          f"{' module ' + args.module if module else ''} found\n")
        sys.exit(1)

    img.imports = kept
    # iat_count intentionally left unchanged: removed slot(s) become dead/unused,
    # so any remaining import's slot index (and any code referencing surviving slots)
    # stays valid. Only a re-run of sqrpatcher from source can reclaim the slot.
    out = args.output or args.file
    img.write(out)
    for r in removed:
        sys.stderr.write(f"sqrlink: removed import {r['module'].decode()}:{r['symbol'].decode()} "
                          f"(freed slot {r['slot']} left unused)\n")
    sys.stderr.write(f"sqrlink: wrote {out}\n")


def cmd_validate(args):
    img = SqrImage(args.file)

    # index available modules by module-id-hint-free filename stem, since SQR
    # resolves imports by module *name* via the registry, not by path; here we
    # just match against filenames the user points us at.
    module_files = {}
    for d in args.search_dir:
        if not os.path.isdir(d):
            continue
        for fn in os.listdir(d):
            if fn.endswith(".sqr"):
                module_files[os.path.splitext(fn)[0]] = os.path.join(d, fn)
    for extra in args.module_file:
        stem = os.path.splitext(os.path.basename(extra))[0]
        module_files[stem] = extra

    ok = True
    for imp in img.imports:
        modname = imp["module"].decode()
        path = module_files.get(modname)
        if path is None:
            print(f"UNRESOLVED  {modname}:{imp['symbol'].decode()}  (module '{modname}' not found in search path)")
            ok = False
            continue
        try:
            target = SqrImage(path)
        except Exception as e:
            print(f"ERROR       {modname}:{imp['symbol'].decode()}  ({e})")
            ok = False
            continue
        candidates = [e for e in target.exports if e["symbol"] == imp["symbol"]]
        match = None
        for e in candidates:
            if (e["version"] >> 16) == (imp["min_version"] >> 16) and \
               (e["version"] & 0xFFFF) >= (imp["min_version"] & 0xFFFF):
                if match is None or e["version"] > match["version"]:
                    match = e
        if match is None:
            print(f"UNRESOLVED  {modname}:{imp['symbol'].decode()}  "
                  f"(need >= {ver_str(imp['min_version'])}, "
                  f"module has: {', '.join(ver_str(e['version']) for e in candidates) or 'none'})")
            ok = False
        else:
            print(f"OK          {modname}:{imp['symbol'].decode()}  "
                  f"-> {ver_str(match['version'])} @ {path}")

    sys.exit(0 if ok else 1)


def main():
    ap = argparse.ArgumentParser(prog="sqrlink", description="Patch imports in SQR files")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("list", help="list a file's imports")
    p.add_argument("file")
    p.set_defaults(func=cmd_list)

    p = sub.add_parser("add-import", help="add a new import (appends an IAT slot)")
    p.add_argument("file")
    p.add_argument("--module", required=True)
    p.add_argument("--symbol", required=True)
    p.add_argument("--kind", choices=["func", "data"], default="func")
    p.add_argument("--min-version", default="1.0", help="major.minor, e.g. 1.0")
    p.add_argument("-o", "--output", help="default: overwrite input")
    p.set_defaults(func=cmd_add_import)

    p = sub.add_parser("remove-import", help="remove an import (slot left unused, never renumbered)")
    p.add_argument("file")
    p.add_argument("--symbol", required=True)
    p.add_argument("--module", help="disambiguate if symbol exists from multiple modules")
    p.add_argument("-o", "--output", help="default: overwrite input")
    p.set_defaults(func=cmd_remove_import)

    p = sub.add_parser("validate", help="check every import resolves against other .sqr modules on disk")
    p.add_argument("file")
    p.add_argument("--search-dir", action="append", default=[], help="directory of .sqr modules (repeatable)")
    p.add_argument("--module-file", action="append", default=[], help="explicit path to a module .sqr (repeatable)")
    p.set_defaults(func=cmd_validate)

    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()