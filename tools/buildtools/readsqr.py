#!/usr/bin/env python3
"""readsqr - dump SQR file headers/tables, like readelf -S / -d / -s."""
import sys
import struct
import argparse

MAGIC = b"SQR\0"
ARCH_NAMES = {1: "X86_64", 2: "AARCH64", 3: "RISCV64"}
FLAG_BITS = [
    (0x1, "EXEC"), (0x2, "LIB"), (0x4, "HAS_TLS"), (0x8, "SORTED_EXPORTS"),
]
KIND_NAMES = {0: "FUNC", 1: "DATA"}


def cstr(buf, off):
    end = buf.find(b"\0", off)
    return buf[off:end].decode("utf-8", "replace")


def flags_str(flags):
    names = [n for bit, n in FLAG_BITS if flags & bit]
    return f"0x{flags:x} [{', '.join(names) if names else '-'}]"


def ver_str(v):
    return f"{v >> 16}.{v & 0xffff}"


def parse(path):
    with open(path, "rb") as f:
        buf = f.read()

    if buf[:4] != MAGIC:
        sys.stderr.write(f"readsqr: {path}: not an SQR file (bad magic)\n")
        sys.exit(1)

    (magic, version, arch, flags, _res0, entry_off, mod_hint, _res1,
     seg_count, _res2, seg_off, imp_count, _res3, imp_off,
     exp_count, _res4, exp_off, iat_count, _res5, iat_off,
     str_off, str_size, tls_off, tls_size, init_off, fini_off) = struct.unpack_from(
        "<4sHHIIQIIIIQIIQIIQIIQQQQQQQ", buf, 0)

    strtab = buf[str_off:str_off + str_size]

    hdr = dict(version=version, arch=arch, flags=flags, entry_off=entry_off,
               mod_hint=mod_hint, seg_count=seg_count, seg_off=seg_off,
               imp_count=imp_count, imp_off=imp_off, exp_count=exp_count,
               exp_off=exp_off, iat_count=iat_count, iat_off=iat_off,
               str_off=str_off, str_size=str_size, tls_off=tls_off,
               tls_size=tls_size, init_off=init_off, fini_off=fini_off)

    segs = []
    for i in range(seg_count):
        o = seg_off + i * 0x20
        vaddr, filesz, memsz, perms, align = struct.unpack_from("<QQQII", buf, o)
        segs.append(dict(vaddr=vaddr, filesz=filesz, memsz=memsz, perms=perms, align=align))

    imports = []
    for i in range(imp_count):
        o = imp_off + i * 0x18
        mod_o, sym_o, slot, kind, minver, _r = struct.unpack_from("<IIIIII", buf, o)
        imports.append(dict(module=cstr(strtab, mod_o), symbol=cstr(strtab, sym_o),
                             slot=slot, kind=kind, min_version=minver))

    exports = []
    for i in range(exp_count):
        o = exp_off + i * 0x18
        sym_o, kind, value, ver, _r = struct.unpack_from("<IIQII", buf, o)
        exports.append(dict(symbol=cstr(strtab, sym_o), kind=kind, value=value, version=ver))

    return buf, hdr, segs, imports, exports


def perms_str(p):
    return ("R" if p & 1 else "-") + ("W" if p & 2 else "-") + ("X" if p & 4 else "-")


def print_header(path, hdr, buf):
    print(f"SQR Header: {path}")
    print(f"  Magic:              SQR\\0")
    print(f"  Version:            {hdr['version']}")
    print(f"  Architecture:       {ARCH_NAMES.get(hdr['arch'], hdr['arch'])} ({hdr['arch']})")
    print(f"  Flags:              {flags_str(hdr['flags'])}")
    print(f"  Entry offset:       0x{hdr['entry_off']:x}")
    print(f"  Module ID hint:     0x{hdr['mod_hint']:x}")
    print(f"  Segments:           {hdr['seg_count']} @ 0x{hdr['seg_off']:x}")
    print(f"  Imports:            {hdr['imp_count']} @ 0x{hdr['imp_off']:x}")
    print(f"  Exports:            {hdr['exp_count']} @ 0x{hdr['exp_off']:x}")
    print(f"  IAT slots:          {hdr['iat_count']} @ 0x{hdr['iat_off']:x}")
    print(f"  String table:       0x{hdr['str_off']:x}, {hdr['str_size']} bytes")
    print(f"  TLS template:       off=0x{hdr['tls_off']:x} size={hdr['tls_size']}")
    print(f"  Init/Fini offset:   0x{hdr['init_off']:x} / 0x{hdr['fini_off']:x}")
    print(f"  File size:          {len(buf)} bytes")


def print_segments(segs):
    print(f"\nSegments ({len(segs)}):")
    print(f"  {'VAddr':>18} {'FileSz':>10} {'MemSz':>10} Perms Align")
    for s in segs:
        print(f"  0x{s['vaddr']:016x} {s['filesz']:10d} {s['memsz']:10d}   "
              f"{perms_str(s['perms'])}  {s['align']}")


def print_imports(imports):
    print(f"\nImports ({len(imports)}):")
    if imports:
        print(f"  {'Slot':>4}  {'Kind':4}  {'Module':<16} {'Symbol':<24} MinVer")
    for i in imports:
        print(f"  {i['slot']:4d}  {KIND_NAMES.get(i['kind'], i['kind']):4}  "
              f"{i['module']:<16} {i['symbol']:<24} {ver_str(i['min_version'])}")


def print_exports(exports):
    print(f"\nExports ({len(exports)}):")
    if exports:
        print(f"  {'Kind':4}  {'Offset':>10}  {'Version':8}  Symbol")
    for e in exports:
        print(f"  {KIND_NAMES.get(e['kind'], e['kind']):4}  0x{e['value']:08x}  "
              f"{ver_str(e['version']):8}  {e['symbol']}")


def main():
    ap = argparse.ArgumentParser(prog="readsqr", description="Display information about SQR files")
    ap.add_argument("file")
    ap.add_argument("-H", "--header", action="store_true", help="show file header")
    ap.add_argument("-l", "--segments", action="store_true", help="show segments")
    ap.add_argument("-i", "--imports", action="store_true", help="show import table")
    ap.add_argument("-e", "--exports", action="store_true", help="show export table")
    ap.add_argument("-a", "--all", action="store_true", help="show everything (default)")
    args = ap.parse_args()

    buf, hdr, segs, imports, exports = parse(args.file)

    show_any = args.header or args.segments or args.imports or args.exports
    if args.all or not show_any:
        args.header = args.segments = args.imports = args.exports = True

    if args.header:
        print_header(args.file, hdr, buf)
    if args.segments:
        print_segments(segs)
    if args.imports:
        print_imports(imports)
    if args.exports:
        print_exports(exports)


if __name__ == "__main__":
    main()