#!/usr/bin/env python3
"""
pak_pnv.py  -  ModuOS .pak packer, unpacker, and viewer  (MPK format)

Usage:
    pak_pnv.py <folder>                    Pack <folder> into <folder>.pak
    pak_pnv.py <folder> -o out.pak         Pack into a specific output path
    pak_pnv.py <file.pak> --view           Print the archive tree
    pak_pnv.py <file.pak> --unpack         Unpack into ./<pak_name>/ folder
    pak_pnv.py <file.pak> --unpack -o dir  Unpack into a specific folder
    pak_pnv.py --help

MPK format (raw, no compression):
    Global header (11 bytes):
        [4]  magic       0x214B504D  ("MPK!" LE)
        [1]  version     0x01
        [4]  entry_count uint32 LE
        [2]  reserved    0x0000
    Per entry:
        [1]  type        0x00 = file, 0x01 = directory
        [2]  path_len    uint16 LE, byte count, no NUL
        [N]  path        UTF-8, forward slashes, no leading slash, no NUL
        [4]  data_len    uint32 LE (0 for directories)
        [M]  data        file bytes (absent for directories)
"""

import sys
import os
import struct
import argparse

# ---------------------------------------------------------------------------
# Format constants
# ---------------------------------------------------------------------------

MPK_MAGIC   = 0x214B504D        # "MPK!" little-endian
MPK_VERSION = 0x01
MPK_HDR     = struct.Struct("<IBIH")   # magic(4) version(1) count(4) reserved(2)
                                        # = 11 bytes
MPK_ENTRY_HDR = struct.Struct("<BHI")  # type(1) path_len(2) data_len(4)
                                        # = 7 bytes

TYPE_FILE = 0x00
TYPE_DIR  = 0x01


# ---------------------------------------------------------------------------
# Low-level encode / decode
# ---------------------------------------------------------------------------

def _encode_entry(entry_type: int, rel_path: str, data: bytes) -> bytes:
    path_bytes = rel_path.replace("\\", "/").encode("utf-8")
    if len(path_bytes) > 4096:
        raise ValueError(f"path too long ({len(path_bytes)} bytes): {rel_path}")
    hdr = MPK_ENTRY_HDR.pack(entry_type, len(path_bytes), len(data))
    return hdr + path_bytes + data


def _decode_entries(raw: bytes) -> list:
    """
    Parse the full archive.  Returns a list of dicts:
        { "type": TYPE_FILE|TYPE_DIR, "path": str, "data": bytes }
    Raises ValueError on malformed input.
    """
    if len(raw) < MPK_HDR.size:
        raise ValueError("file is too small to contain an MPK header")

    magic, version, count, _reserved = MPK_HDR.unpack_from(raw, 0)
    if magic != MPK_MAGIC:
        raise ValueError(f"bad magic 0x{magic:08X} — not an MPK archive")
    if version != MPK_VERSION:
        raise ValueError(f"unsupported MPK version {version}")

    pos     = MPK_HDR.size
    entries = []

    for i in range(count):
        if pos + MPK_ENTRY_HDR.size > len(raw):
            raise ValueError(f"truncated archive at entry {i}")

        etype, path_len, data_len = MPK_ENTRY_HDR.unpack_from(raw, pos)
        pos += MPK_ENTRY_HDR.size

        if path_len == 0 or pos + path_len > len(raw):
            raise ValueError(f"bad path_len {path_len} at entry {i}")

        path = raw[pos:pos + path_len].decode("utf-8", errors="replace")
        pos += path_len

        if pos + data_len > len(raw):
            raise ValueError(
                f"data out of bounds for entry {i} ('{path}')"
            )

        data = raw[pos:pos + data_len]
        pos += data_len

        entries.append({"type": etype, "path": path, "data": data})

    return entries


# ---------------------------------------------------------------------------
# Packer
# ---------------------------------------------------------------------------

def pack(src_folder: str, out_path: str) -> None:
    src_folder = os.path.abspath(src_folder)
    if not os.path.isdir(src_folder):
        print(f"error: '{src_folder}' is not a directory")
        sys.exit(1)

    if not os.path.isfile(os.path.join(src_folder, "pakdata.ini")):
        print(f"error: '{src_folder}' has no pakdata.ini")
        sys.exit(1)

    if not os.path.isdir(os.path.join(src_folder, "sysroot")):
        print(f"error: '{src_folder}' has no sysroot/ directory")
        sys.exit(1)

    parts        = []
    total_files  = 0
    total_dirs   = 0
    entry_count  = 0

    for dirpath, dirnames, filenames in os.walk(src_folder):
        dirnames.sort()
        filenames.sort()

        rel_dir = os.path.relpath(dirpath, src_folder).replace("\\", "/")
        if rel_dir == ".":
            rel_dir = ""

        # Emit directory entry (skip root itself)
        if rel_dir:
            parts.append(_encode_entry(TYPE_DIR, rel_dir, b""))
            entry_count += 1
            total_dirs  += 1

        for fname in filenames:
            fpath    = os.path.join(dirpath, fname)
            rel_file = (rel_dir + "/" + fname).lstrip("/")
            with open(fpath, "rb") as f:
                data = f.read()
            parts.append(_encode_entry(TYPE_FILE, rel_file, data))
            entry_count += 1
            total_files += 1
            print(f"  packing: {rel_file}  ({len(data):,} bytes)")

    if entry_count > 0xFFFF:
        print(f"error: too many entries ({entry_count}); MPK maximum is 65535")
        sys.exit(1)

    header   = MPK_HDR.pack(MPK_MAGIC, MPK_VERSION, entry_count, 0)
    archive  = header + b"".join(parts)

    with open(out_path, "wb") as f:
        f.write(archive)

    print()
    print(f"packed {total_files} file(s) in {total_dirs} director(ies)")
    print(f"size:   {len(archive):>12,} bytes")
    print(f"output: {out_path}")


# ---------------------------------------------------------------------------
# Viewer
# ---------------------------------------------------------------------------

def _build_tree(entries: list) -> dict:
    root = {}
    for e in entries:
        parts = e["path"].replace("\\", "/").split("/")
        node  = root
        for part in parts[:-1]:
            node = node.setdefault(part, {})
        leaf = parts[-1]
        if e["type"] == TYPE_DIR:
            node.setdefault(leaf, {})
        else:
            node[leaf] = None
    return root


def _print_tree(node: dict, name: str,
                prefix: str = "", is_last: bool = True) -> None:
    connector    = "└───" if is_last else "├───"
    child_prefix = prefix + ("    " if is_last else "│   ")
    if name:
        print(prefix + connector + name)

    dirs     = sorted(k for k, v in node.items() if isinstance(v, dict))
    files    = sorted(k for k, v in node.items() if v is None)
    children = dirs + files

    for i, child in enumerate(children):
        last = (i == len(children) - 1)
        val  = node[child]
        if isinstance(val, dict):
            _print_tree(val, child, child_prefix, last)
        else:
            c = "└───" if last else "├───"
            print(child_prefix + c + child)


def view(pak_path: str) -> None:
    if not os.path.isfile(pak_path):
        print(f"error: '{pak_path}' not found")
        sys.exit(1)

    with open(pak_path, "rb") as f:
        raw = f.read()

    try:
        entries = _decode_entries(raw)
    except ValueError as e:
        print(f"error: {e}")
        sys.exit(1)

    tree     = _build_tree(entries)
    pak_name = os.path.basename(pak_path)
    print(pak_name)

    top_files = sorted(k for k, v in tree.items() if v is None)
    top_dirs  = sorted(k for k, v in tree.items() if isinstance(v, dict))
    all_top   = top_files + top_dirs

    for i, name in enumerate(all_top):
        last = (i == len(all_top) - 1)
        val  = tree[name]
        if val is None:
            print(("    " if last else "│   ") + name)
        else:
            _print_tree(val, name, "", last)


# ---------------------------------------------------------------------------
# Unpacker
# ---------------------------------------------------------------------------

def _safe_join(base: str, rel: str) -> str:
    rel      = rel.replace("\\", "/").lstrip("/")
    out      = os.path.normpath(os.path.join(base, rel))
    base_abs = os.path.realpath(base)
    out_abs  = os.path.realpath(out)
    if not out_abs.startswith(base_abs + os.sep) and out_abs != base_abs:
        raise ValueError(f"path traversal blocked: '{rel}'")
    return out


def unpack(pak_path: str, out_dir: str) -> None:
    if not os.path.isfile(pak_path):
        print(f"error: '{pak_path}' not found")
        sys.exit(1)

    with open(pak_path, "rb") as f:
        raw = f.read()

    try:
        entries = _decode_entries(raw)
    except ValueError as e:
        print(f"error: {e}")
        sys.exit(1)

    out_dir = os.path.abspath(out_dir)
    if os.path.exists(out_dir):
        print(f"error: output directory already exists: '{out_dir}'")
        print(f"       remove it first or choose a different -o path")
        sys.exit(1)

    os.makedirs(out_dir)
    total_files = 0
    total_dirs  = 0

    for e in entries:
        try:
            dest = _safe_join(out_dir, e["path"])
        except ValueError as ex:
            print(f"warning: skipping — {ex}")
            continue

        if e["type"] == TYPE_DIR:
            os.makedirs(dest, exist_ok=True)
            total_dirs += 1
        else:
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            with open(dest, "wb") as fw:
                fw.write(e["data"])
            print(f"  extract: {e['path']}  ({len(e['data']):,} bytes)")
            total_files += 1

    print()
    print(f"extracted {total_files} file(s) in {total_dirs} director(ies)")
    print(f"output:   {out_dir}")


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="ModuOS .pak packer, unpacker, and viewer (MPK format)",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument("path",
        help="Source folder to pack, or .pak file to view/unpack")
    parser.add_argument("-o", "--output", metavar="OUT",
        help="Output path: .pak file (pack) or folder (unpack)")
    parser.add_argument("--view",   action="store_true",
        help="View the contents of a .pak file")
    parser.add_argument("--unpack", action="store_true",
        help="Unpack a .pak file to a folder")

    args = parser.parse_args()

    if args.unpack:
        default_out = os.path.splitext(os.path.basename(args.path))[0]
        unpack(args.path, args.output or default_out)
    elif args.view:
        view(args.path)
    elif os.path.isfile(args.path) and args.path.endswith(".pak") and not args.output:
        view(args.path)
    else:
        src = args.path.rstrip("/\\")
        pack(src, args.output or (src + ".pak"))


if __name__ == "__main__":
    main()