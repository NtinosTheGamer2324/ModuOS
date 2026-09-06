#!/usr/bin/env python3
"""
umcs_viewer.py — terminal viewer for compiled UMCS binaries.

Same decode logic as wumcsd.py, but formatted as a readable,
color-coded report instead of a flat disassembly line — includes
the module header, capability flags, and a bit-field breakdown
per instruction, plus branch target / mismatch warnings (same
checks wumcs-validate.py performs, shown inline here).

USAGE:
    python3 umcs_viewer.py file.umcs
    python3 umcs_viewer.py file.umcs --no-color
"""
import sys
import struct
import argparse
from wumcsc import (
    OPCODES, OP_SHIFT, FMT_SHIFT, PRED_SHIFT, FIELD10,
    FMT_R, FMT_I, FMT_B, FMT_M,
    R_RD_SHIFT, R_RS_SHIFT, R_RT_SHIFT, R_RA_SHIFT, R_MOD_SHIFT,
    I_RD_SHIFT, I_IMM_SHIFT, I_MOD_SHIFT, I_IMM_MASK, I_MOD_MASK,
    B_OFFSET_BITS, B_OFFSET_MASK,
    M_RD_SHIFT, M_RS_SHIFT, M_TEX_SHIFT, M_SZ_SHIFT, M_TEX_MASK, M_SZ_MASK,
    M_OFFSET_BITS, M_OFFSET_MASK, UMCS_MAGIC, CAPABILITIES,
)

OPCODE_NAMES = {v[0]: k for k, v in OPCODES.items()}
EXPECTED_FMT = {v[0]: v[1] for v in OPCODES.values()}
FMT_LABEL = {FMT_R: "R", FMT_I: "I", FMT_B: "B", FMT_M: "M"}

# ANSI color codes per format, matching the HTML viewer's palette
COLORS = {
    FMT_R: "\033[38;5;179m",  # amber
    FMT_I: "\033[38;5;79m",   # teal
    FMT_B: "\033[38;5;140m",  # violet
    FMT_M: "\033[38;5;167m",  # coral
}
DIM = "\033[2m"
BOLD = "\033[1m"
RED = "\033[91m"
RESET = "\033[0m"


def sign_extend(val, bits):
    if val & (1 << (bits - 1)):
        val -= (1 << bits)
    return val


def c(code, text, use_color):
    return f"{code}{text}{RESET}" if use_color else text


def decode_and_format(w, idx, count, use_color):
    op = (w >> OP_SHIFT) & 0xFF
    fmt = (w >> FMT_SHIFT) & 0x3
    pred = (w >> PRED_SHIFT) & 0xF
    name = OPCODE_NAMES.get(op, f"UNK({op:#04x})")
    expected = EXPECTED_FMT.get(op)
    mismatch = expected is not None and fmt != expected
    color = COLORS.get(fmt, "")

    if op not in OPCODE_NAMES or mismatch:
        label = c(RED, f"{name:8s}", use_color)
        detail = (f"format {fmt} disagrees with expected {expected}"
                   if mismatch else f"unknown opcode {op:#04x}")
        return f"{idx:4d}  [{FMT_LABEL.get(fmt,'?')}]  {label}  {c(RED, '⚠ ' + detail, use_color)}", None

    mnem = c(color, f"{name:8s}", use_color)
    badge = c(color, f"[{FMT_LABEL[fmt]}]", use_color)
    jump = None

    if fmt == FMT_R:
        rd = (w >> R_RD_SHIFT) & FIELD10
        rs = (w >> R_RS_SHIFT) & FIELD10
        rt = (w >> R_RT_SHIFT) & FIELD10
        ra = (w >> R_RA_SHIFT) & FIELD10
        mod = (w >> R_MOD_SHIFT) & FIELD10
        operands = f"r{rd}, r{rs}, r{rt}, r{ra}, mod={mod}"
    elif fmt == FMT_I:
        rd = (w >> I_RD_SHIFT) & FIELD10
        imm = (w >> I_IMM_SHIFT) & I_IMM_MASK
        mod = (w >> I_MOD_SHIFT) & I_MOD_MASK
        fbits = struct.pack("<I", imm)
        fval = struct.unpack("<f", fbits)[0]
        operands = f"r{rd}, imm32={imm:#010x} (f32≈{fval:.6g}), mod={mod}"
    elif fmt == FMT_B:
        raw = w & B_OFFSET_MASK
        offset = sign_extend(raw, B_OFFSET_BITS)
        target = idx + offset
        in_range = 0 <= target < count
        jump = target if in_range else None
        tgt_str = f"→ instr {target}" if in_range else c(RED, f"→ instr {target} (OUT OF RANGE)", use_color)
        operands = f"offset={offset}  {tgt_str}"
    elif fmt == FMT_M:
        rd = (w >> M_RD_SHIFT) & FIELD10
        rs = (w >> M_RS_SHIFT) & FIELD10
        tex = (w >> M_TEX_SHIFT) & M_TEX_MASK
        sz = (w >> M_SZ_SHIFT) & M_SZ_MASK
        raw = w & M_OFFSET_MASK
        offset = sign_extend(raw, M_OFFSET_BITS)
        sz_flag = c(RED, " ⚠ reserved sz", use_color) if sz > 3 else ""
        operands = f"r{rd}, [r{rs} + {offset}], tex={tex}, sz={sz}{sz_flag}"
    else:
        operands = f"raw={w:#018x}"

    pred_str = f"  {DIM if use_color else ''}pred={pred}{RESET if use_color else ''}" if pred else ""
    line = f"{idx:4d}  {badge}  {mnem}  {operands}{pred_str}"
    return line, jump


def print_bitlane(w, fmt, use_color, width=64):
    """ASCII bit-lane: one block character per field, sized proportionally."""
    if fmt == FMT_R:
        segs = [("op", 8), ("fmt", 2), ("pred", 4), ("rd", 10), ("rs", 10), ("rt", 10), ("ra", 10), ("mod", 10)]
    elif fmt == FMT_I:
        segs = [("op", 8), ("fmt", 2), ("pred", 4), ("rd", 10), ("imm32", 32), ("mod", 8)]
    elif fmt == FMT_B:
        segs = [("op", 8), ("fmt", 2), ("pred", 4), ("offset", 50)]
    elif fmt == FMT_M:
        segs = [("op", 8), ("fmt", 2), ("pred", 4), ("rd", 10), ("rs", 10), ("tex", 8), ("sz", 3), ("offset", 19)]
    else:
        segs = [("raw", 64)]

    color = COLORS.get(fmt, "")
    bar = ""
    for _, bits in segs:
        n = max(1, round(bits / 64 * 48))
        bar += "█" * n
    print(f"      {c(color, bar, use_color)}")


def main():
    ap = argparse.ArgumentParser(description="Terminal viewer for UMCS binaries")
    ap.add_argument("file", help="path to .umcs binary")
    ap.add_argument("--no-color", action="store_true", help="disable ANSI color output")
    ap.add_argument("--bitlane", action="store_true", help="show a bit-lane bar under each instruction")
    args = ap.parse_args()

    use_color = not args.no_color and sys.stdout.isatty()

    with open(args.file, "rb") as f:
        data = f.read()

    if len(data) < 20:
        print(f"error: file too short to be a UMCS module ({len(data)} bytes)", file=sys.stderr)
        sys.exit(1)

    magic, vmaj, vmin, count, caps, _ = struct.unpack("<IHHIII", data[:20])
    if magic != UMCS_MAGIC:
        print(f"error: bad magic {magic:#010x} — not a UMCS file", file=sys.stderr)
        sys.exit(1)

    body = data[20:]
    available = len(body) // 8
    n = min(count, available)

    active_caps = [name for name, bit in CAPABILITIES.items() if caps & bit]

    title = c(BOLD, f"UMCS v{vmaj}.{vmin}", use_color)
    print(f"{title}  —  {args.file}")
    print(f"  instructions : {n}{'  (declared ' + str(count) + ', truncated!)' if count != available else ''}")
    print(f"  capabilities : {', '.join(active_caps) if active_caps else 'none'}")
    print()

    for i in range(n):
        (w,) = struct.unpack_from("<Q", body, i * 8)
        fmt = (w >> FMT_SHIFT) & 0x3
        line, _jump = decode_and_format(w, i, n, use_color)
        print(line)
        if args.bitlane:
            print_bitlane(w, fmt, use_color)

    print()
    legend = "  ".join(
        c(COLORS[f], f"[{FMT_LABEL[f]}]", use_color) + f" {name}"
        for f, name in [(FMT_R, "alu"), (FMT_I, "immediate"), (FMT_B, "branch"), (FMT_M, "memory/tex")]
    )
    print(legend)


if __name__ == "__main__":
    main()