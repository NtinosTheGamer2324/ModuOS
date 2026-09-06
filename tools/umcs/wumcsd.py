#!/usr/bin/env python3
"""wumcsd.py — UMCS binary -> readable text (verification tool, not full WUMCS)."""
import sys
import struct
from wumcsc import (
    OPCODES, OP_SHIFT, FMT_SHIFT, PRED_SHIFT, FIELD10,
    FMT_R, FMT_I, FMT_B, FMT_M,
    R_RD_SHIFT, R_RS_SHIFT, R_RT_SHIFT, R_RA_SHIFT, R_MOD_SHIFT,
    I_RD_SHIFT, I_IMM_SHIFT, I_MOD_SHIFT, I_IMM_MASK, I_MOD_MASK,
    B_OFFSET_BITS, B_OFFSET_MASK,
    M_RD_SHIFT, M_RS_SHIFT, M_TEX_SHIFT, M_SZ_SHIFT, M_TEX_MASK, M_SZ_MASK,
    M_OFFSET_BITS, M_OFFSET_MASK, UMCS_MAGIC,
)

OPCODE_NAMES = {v[0]: k for k, v in OPCODES.items()}


def sign_extend(val, bits):
    if val & (1 << (bits - 1)):
        val -= (1 << bits)
    return val


def decode_word(w, idx):
    op = (w >> OP_SHIFT) & 0xFF
    fmt = (w >> FMT_SHIFT) & 0x3
    pred = (w >> PRED_SHIFT) & 0xF
    name = OPCODE_NAMES.get(op, f"UNK({op:#x})")

    if name == "EXIT" or fmt == FMT_R:
        rd = (w >> R_RD_SHIFT) & FIELD10
        rs = (w >> R_RS_SHIFT) & FIELD10
        rt = (w >> R_RT_SHIFT) & FIELD10
        ra = (w >> R_RA_SHIFT) & FIELD10
        mod = (w >> R_MOD_SHIFT) & FIELD10
        return f"{idx:4d}: {name:8s} r{rd}, r{rs}, r{rt}, r{ra}, mod={mod}, pred={pred}"
    elif fmt == FMT_I:
        rd = (w >> I_RD_SHIFT) & FIELD10
        imm = (w >> I_IMM_SHIFT) & I_IMM_MASK
        mod = (w >> I_MOD_SHIFT) & I_MOD_MASK
        return f"{idx:4d}: {name:8s} r{rd}, imm32={imm:#010x}, mod={mod}, pred={pred}"
    elif fmt == FMT_B:
        raw = w & B_OFFSET_MASK
        offset = sign_extend(raw, B_OFFSET_BITS)
        return f"{idx:4d}: {name:8s} target={idx+offset} (offset={offset}), pred={pred}"
    elif fmt == FMT_M:
        rd = (w >> M_RD_SHIFT) & FIELD10
        rs = (w >> M_RS_SHIFT) & FIELD10
        tex = (w >> M_TEX_SHIFT) & M_TEX_MASK
        sz = (w >> M_SZ_SHIFT) & M_SZ_MASK
        raw = w & M_OFFSET_MASK
        offset = sign_extend(raw, M_OFFSET_BITS)
        return f"{idx:4d}: {name:8s} r{rd}, [r{rs} + {offset}], tex={tex}, sz={sz}, pred={pred}"
    return f"{idx:4d}: <unknown format>"


def main():
    path = sys.argv[1]
    with open(path, "rb") as f:
        data = f.read()
    magic, vmaj, vmin, count, caps, _ = struct.unpack("<IHHIII", data[:20])
    if magic != UMCS_MAGIC:
        print("not a UMCS file (bad magic)", file=sys.stderr)
        sys.exit(1)
    print(f"; UMCS v{vmaj}.{vmin}, {count} instructions, capability_bits={caps:#x}")
    body = data[20:]
    for i in range(count):
        (w,) = struct.unpack_from("<Q", body, i * 8)
        print(decode_word(w, i))


if __name__ == "__main__":
    main()