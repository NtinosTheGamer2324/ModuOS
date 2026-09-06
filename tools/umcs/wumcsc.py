#!/usr/bin/env python3
"""
wumcsc.py — WUMCS -> UMCS compiler (assembler)

WUMCS is the text form of UMCS (ModuOS's 64-bit shader ISA).
This assembler emits binary that is bit-for-bit identical to what
umcs64.h's C encode_* functions would produce for the same operands
— the field shifts/masks below are a direct transcription of that
header, not a reinterpretation. If umcs64.h's layout ever changes,
this file has to change with it (see FIELD LAYOUT section).

USAGE:
    python3 wumcsc.py input.wumcs -o output.umcs

WUMCS SYNTAX
------------
Comments        : ';' to end of line
Labels          : 'label_name:' on its own line
Capability decl : '.cap CAP_NAME' (repeatable), e.g. '.cap DOUBLE_PRECISION'
Registers       : r0 .. r1023
Predicate       : optional 'pred=N' operand (N = 0..15), default pred=0
                  (pred=0 conventionally means "always execute")

Per-format instruction syntax:

  R-format (4-operand ALU, e.g. FFMA dst = a*b + c):
      OPCODE rd, rs, rt, ra [, mod=N] [, pred=N]
      Unused operand slots for ops needing fewer registers are
      just written as r0 and ignored by the backend for that opcode.

  I-format (register + 32-bit immediate):
      OPCODE rd, IMM [, mod=N] [, pred=N]
      IMM may be a decimal/hex integer (e.g. 42, 0x2A) or a float
      literal (e.g. 3.14159) — floats are packed as IEEE-754 bits.

  B-format (branch):
      OPCODE label [, pred=N]
      Offset is computed automatically (PC-relative, word units).

  M-format (memory/texture):
      OPCODE rd, [rs + OFFSET], tex=N, sz=N [, pred=N]
      OFFSET is a signed byte offset immediate.

EXAMPLE
-------
    .cap DOUBLE_PRECISION

    main:
        MOVI    r1, 3.14159265
        FFMA    r5, r6, r7, r8
        LDG     r9, [r10 + 0], tex=0, sz=2
        ICMP    r0, r9, r1, r0, mod=0
        BRAP    main, pred=1
        EXIT    r0, r0, r0, r0
"""
import sys
import re
import struct
import argparse

# =====================================================================
# FIELD LAYOUT — must match umcs64.h exactly
# =====================================================================
OP_SHIFT, FMT_SHIFT, PRED_SHIFT = 56, 54, 50
FIELD10 = 0x3FF  # 10-bit field mask, used for rd/rs/rt/ra

FMT_R, FMT_I, FMT_B, FMT_M = 0, 1, 2, 3

R_RD_SHIFT, R_RS_SHIFT, R_RT_SHIFT, R_RA_SHIFT, R_MOD_SHIFT = 40, 30, 20, 10, 0

I_RD_SHIFT, I_IMM_SHIFT, I_MOD_SHIFT = 40, 8, 0
I_IMM_MASK, I_MOD_MASK = 0xFFFFFFFF, 0xFF

B_OFFSET_BITS = 50
B_OFFSET_MASK = (1 << B_OFFSET_BITS) - 1

M_RD_SHIFT, M_RS_SHIFT, M_TEX_SHIFT, M_SZ_SHIFT = 40, 30, 22, 19
M_TEX_MASK, M_SZ_MASK = 0xFF, 0x7
M_OFFSET_BITS = 19
M_OFFSET_MASK = (1 << M_OFFSET_BITS) - 1

MASK64 = (1 << 64) - 1

# =====================================================================
# OPCODES + which format each one uses (from umcs64.h categories)
# =====================================================================
OPCODES = {
    # Data movement
    "MOV": (0x01, FMT_R), "MOVI": (0x02, FMT_I), "MOVHI": (0x03, FMT_I),
    "MOVP": (0x04, FMT_R), "SWZL": (0x05, FMT_R), "PACK": (0x06, FMT_R),
    "UNPACK": (0x07, FMT_R),
    # Integer ALU
    "IADD": (0x10, FMT_R), "IADDI": (0x11, FMT_I), "ISUB": (0x12, FMT_R),
    "IMUL": (0x13, FMT_R), "IMULHI": (0x14, FMT_R), "IDIV": (0x15, FMT_R),
    "IMOD": (0x16, FMT_R), "AND": (0x17, FMT_R), "OR": (0x18, FMT_R),
    "XOR": (0x19, FMT_R), "NOT": (0x1A, FMT_R), "SHL": (0x1B, FMT_R),
    "SHR": (0x1C, FMT_R), "SAR": (0x1D, FMT_R), "ICMP": (0x1E, FMT_R),
    "ISEL": (0x1F, FMT_R), "CLZ": (0x20, FMT_R), "POPCNT": (0x21, FMT_R),
    "BREV": (0x22, FMT_R),
    # Float ALU
    "FADD": (0x30, FMT_R), "FSUB": (0x31, FMT_R), "FMUL": (0x32, FMT_R),
    "FFMA": (0x33, FMT_R), "FDIV": (0x34, FMT_R), "FRCP": (0x35, FMT_R),
    "FRSQ": (0x36, FMT_R), "FSQRT": (0x37, FMT_R), "FMIN": (0x38, FMT_R),
    "FMAX": (0x39, FMT_R), "FSAT": (0x3A, FMT_R), "FCMP": (0x3B, FMT_R),
    "FSEL": (0x3C, FMT_R), "F2I": (0x3D, FMT_R), "I2F": (0x3E, FMT_R),
    "F2H": (0x3F, FMT_R), "H2F": (0x40, FMT_R), "FSIN": (0x41, FMT_R),
    "FCOS": (0x42, FMT_R), "FLOG2": (0x43, FMT_R), "FEXP2": (0x44, FMT_R),
    # Flow control
    "BRA": (0x50, FMT_B), "BRAP": (0x51, FMT_B), "BRAPN": (0x52, FMT_B),
    "CALL": (0x53, FMT_B), "RET": (0x54, FMT_B), "SYNC": (0x55, FMT_B),
    "SSY": (0x56, FMT_B), "EMIT": (0x57, FMT_B), "ENDPRIM": (0x58, FMT_B),
    "KILL": (0x59, FMT_B), "EXIT": (0x5F, FMT_R),  # EXIT takes no branch target
    # Memory / Texture
    "LDL": (0x60, FMT_M), "STL": (0x61, FMT_M), "LDS": (0x62, FMT_M),
    "STS": (0x63, FMT_M), "LDG": (0x64, FMT_M), "STG": (0x65, FMT_M),
    "LDU": (0x66, FMT_M), "ATOMIC": (0x67, FMT_M), "TEX": (0x68, FMT_M),
    "TEXB": (0x69, FMT_M), "TEXL": (0x6A, FMT_M), "TEXG": (0x6B, FMT_M),
    "TEXF": (0x6C, FMT_M), "MEMBAR": (0x6D, FMT_M), "PREFETCH": (0x6E, FMT_M),
}

CAPABILITIES = {
    "DOUBLE_PRECISION": 1 << 0, "INT64": 1 << 1, "ATOMICS_FLOAT": 1 << 2,
    "TEXTURE_ARRAY": 1 << 3, "RAY_QUERY": 1 << 4, "MESH_SHADING": 1 << 5,
    "SUBGROUP_OPS": 1 << 6,
}

UMCS_MAGIC = 0x53434D55
VERSION_MAJOR, VERSION_MINOR = 2, 0


class AsmError(Exception):
    def __init__(self, msg, lineno=None):
        super().__init__(f"line {lineno}: {msg}" if lineno else msg)


def parse_reg(tok):
    tok = tok.strip()
    m = re.fullmatch(r"[rR](\d+)", tok)
    if not m:
        raise AsmError(f"expected register, got '{tok}'")
    n = int(m.group(1))
    if not (0 <= n < 1024):
        raise AsmError(f"register out of range (0-1023): r{n}")
    return n


def parse_int_or_float_imm(tok):
    """Returns 32-bit unsigned bit pattern for an int or float literal."""
    tok = tok.strip()
    try:
        if re.fullmatch(r"0[xX][0-9a-fA-F]+", tok):
            v = int(tok, 16) & 0xFFFFFFFF
            return v
        if re.fullmatch(r"-?\d+", tok):
            v = int(tok)
            return v & 0xFFFFFFFF
        # float literal
        v = float(tok)
        return struct.unpack("<I", struct.pack("<f", v))[0]
    except ValueError:
        raise AsmError(f"invalid immediate '{tok}'")


def parse_kv_operands(rest):
    """Parse trailing ', key=val' operands (mod=, pred=, tex=, sz=) into a dict."""
    kv = {}
    parts = [p.strip() for p in rest if p.strip()]
    for p in parts:
        m = re.fullmatch(r"(\w+)\s*=\s*(-?\w+)", p)
        if not m:
            raise AsmError(f"expected key=value operand, got '{p}'")
        kv[m.group(1)] = m.group(2)
    return kv


def split_operands(text):
    """Split on top-level commas (none of our operands nest, so this is simple)."""
    return [t.strip() for t in text.split(",")] if text.strip() else []


class Assembler:
    def __init__(self):
        self.lines = []       # (lineno, mnemonic, operand_text)
        self.labels = {}      # name -> word index
        self.capability_bits = 0

    def pass1(self, src_lines):
        """Strip comments/whitespace, record label addresses, collect real instructions."""
        word_index = 0
        for lineno, raw in enumerate(src_lines, 1):
            line = raw.split(";", 1)[0].strip()
            if not line:
                continue
            if line.startswith(".cap"):
                _, name = line.split(None, 1)
                name = name.strip()
                if name not in CAPABILITIES:
                    raise AsmError(f"unknown capability '{name}'", lineno)
                self.capability_bits |= CAPABILITIES[name]
                continue
            label_match = re.fullmatch(r"(\w+):", line)
            if label_match:
                self.labels[label_match.group(1)] = word_index
                continue
            m = re.match(r"(\w+)\s*(.*)", line)
            if not m:
                raise AsmError(f"could not parse line: '{line}'", lineno)
            mnemonic, operand_text = m.group(1).upper(), m.group(2)
            self.lines.append((lineno, mnemonic, operand_text))
            word_index += 1

    def encode_r(self, op, operand_text, lineno):
        ops = split_operands(operand_text)
        if len(ops) < 4:
            raise AsmError(f"{op} (R-format) needs 4 register operands", lineno)
        rd, rs, rt, ra = (parse_reg(x) for x in ops[:4])
        kv = parse_kv_operands(ops[4:])
        mod = int(kv.get("mod", "0"), 0) & FIELD10
        pred = int(kv.get("pred", "0"), 0) & 0xF
        opcode, _ = OPCODES[op]
        w = (opcode << OP_SHIFT) | (FMT_R << FMT_SHIFT) | (pred << PRED_SHIFT)
        w |= (rd & FIELD10) << R_RD_SHIFT
        w |= (rs & FIELD10) << R_RS_SHIFT
        w |= (rt & FIELD10) << R_RT_SHIFT
        w |= (ra & FIELD10) << R_RA_SHIFT
        w |= mod << R_MOD_SHIFT
        return w & MASK64

    def encode_i(self, op, operand_text, lineno):
        ops = split_operands(operand_text)
        if len(ops) < 2:
            raise AsmError(f"{op} (I-format) needs 'rd, imm'", lineno)
        rd = parse_reg(ops[0])
        imm32 = parse_int_or_float_imm(ops[1])
        kv = parse_kv_operands(ops[2:])
        mod = int(kv.get("mod", "0"), 0) & I_MOD_MASK
        pred = int(kv.get("pred", "0"), 0) & 0xF
        opcode, _ = OPCODES[op]
        w = (opcode << OP_SHIFT) | (FMT_I << FMT_SHIFT) | (pred << PRED_SHIFT)
        w |= (rd & FIELD10) << I_RD_SHIFT
        w |= (imm32 & I_IMM_MASK) << I_IMM_SHIFT
        w |= mod << I_MOD_SHIFT
        return w & MASK64

    def encode_b(self, op, operand_text, lineno, word_index):
        ops = split_operands(operand_text)
        if op == "EXIT":  # EXIT is FMT_R with no real operands, handled by encode_r path
            raise AsmError("internal: EXIT should not reach encode_b", lineno)
        if not ops:
            raise AsmError(f"{op} (B-format) needs a label", lineno)
        label = ops[0]
        kv = parse_kv_operands(ops[1:])
        pred = int(kv.get("pred", "0"), 0) & 0xF
        if label not in self.labels:
            raise AsmError(f"undefined label '{label}'", lineno)
        offset = self.labels[label] - word_index
        opcode, _ = OPCODES[op]
        w = (opcode << OP_SHIFT) | (FMT_B << FMT_SHIFT) | (pred << PRED_SHIFT)
        w |= offset & B_OFFSET_MASK
        return w & MASK64

    def encode_m(self, op, operand_text, lineno):
        # syntax: rd, [rs + OFFSET], tex=N, sz=N [, pred=N]
        m = re.match(
            r"\s*([rR]\d+)\s*,\s*\[\s*([rR]\d+)\s*([+-]\s*\w+)?\s*\]\s*(.*)",
            operand_text,
        )
        if not m:
            raise AsmError(
                f"{op} (M-format) expected 'rd, [rs + OFFSET], tex=N, sz=N'", lineno
            )
        rd = parse_reg(m.group(1))
        rs = parse_reg(m.group(2))
        offset_txt = (m.group(3) or "+0").replace(" ", "")
        offset = int(offset_txt, 0)
        kv = parse_kv_operands(m.group(4).split(","))
        tex = int(kv.get("tex", "0"), 0) & M_TEX_MASK
        sz = int(kv.get("sz", "0"), 0) & M_SZ_MASK
        pred = int(kv.get("pred", "0"), 0) & 0xF
        opcode, _ = OPCODES[op]
        w = (opcode << OP_SHIFT) | (FMT_M << FMT_SHIFT) | (pred << PRED_SHIFT)
        w |= (rd & FIELD10) << M_RD_SHIFT
        w |= (rs & FIELD10) << M_RS_SHIFT
        w |= tex << M_TEX_SHIFT
        w |= sz << M_SZ_SHIFT
        w |= offset & M_OFFSET_MASK
        return w & MASK64

    def assemble(self, src_lines):
        self.pass1(src_lines)
        words = []
        for i, (lineno, mnemonic, operand_text) in enumerate(self.lines):
            if mnemonic not in OPCODES:
                raise AsmError(f"unknown opcode '{mnemonic}'", lineno)
            _, fmt = OPCODES[mnemonic]
            if mnemonic == "EXIT":
                # EXIT rd,rs,rt,ra are conventionally r0 placeholders
                text = operand_text.strip() or "r0, r0, r0, r0"
                words.append(self.encode_r(mnemonic, text, lineno))
            elif fmt == FMT_R:
                words.append(self.encode_r(mnemonic, operand_text, lineno))
            elif fmt == FMT_I:
                words.append(self.encode_i(mnemonic, operand_text, lineno))
            elif fmt == FMT_B:
                words.append(self.encode_b(mnemonic, operand_text, lineno, i))
            elif fmt == FMT_M:
                words.append(self.encode_m(mnemonic, operand_text, lineno))
        return words

    def to_binary(self, words):
        header = struct.pack(
            "<IHHIII",
            UMCS_MAGIC, VERSION_MAJOR, VERSION_MINOR,
            len(words), self.capability_bits, 0,
        )
        body = b"".join(struct.pack("<Q", w) for w in words)
        return header + body


def main():
    ap = argparse.ArgumentParser(description="WUMCS -> UMCS compiler")
    ap.add_argument("input", help="input .wumcs source file")
    ap.add_argument("-o", "--output", required=True, help="output .umcs binary path")
    args = ap.parse_args()

    with open(args.input, "r") as f:
        src_lines = f.readlines()

    asm = Assembler()
    try:
        words = asm.assemble(src_lines)
    except AsmError as e:
        print(f"wumcsc: error: {e}", file=sys.stderr)
        sys.exit(1)

    binary = asm.to_binary(words)
    with open(args.output, "wb") as f:
        f.write(binary)

    print(f"wumcsc: assembled {len(words)} instruction(s), "
          f"{len(binary)} bytes -> {args.output}")
    if asm.capability_bits:
        active = [name for name, bit in CAPABILITIES.items()
                  if asm.capability_bits & bit]
        print(f"wumcsc: capabilities: {', '.join(active)}")


if __name__ == "__main__":
    main()