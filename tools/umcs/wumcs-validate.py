#!/usr/bin/env python3
"""
wumcs-validate.py — validates a UMCS binary for structural correctness.

This is deliberately separate from the assembler (same relationship
spirv-val has to spirv-as): a binary can arrive here from ANY producer
— the wumcsc.py assembler, a future GLSL-like front end, a hand-patched
file — and this tool has to catch problems regardless of source, the
same way a driver's own loader should before it trusts the binary.

Checks performed:
  - magic / version header sanity
  - every opcode byte is a known UMCS opcode
  - the format bits encoded in the word match the format that opcode
    is actually defined to use (catches corrupted/hand-edited binaries
    where op and fmt bits disagree)
  - M-format 'sz' field is in the currently-defined range (0-3;
    4-7 are reserved and would silently mean something undefined
    on a real backend)
  - B-format branch targets resolve to a valid instruction index
    inside the module (not just a valid bit pattern — an in-range
    offset can still point off the end of the program)
  - capability_bits in the header only sets bits this validator
    recognizes (catches a header built against a newer/unknown spec)

Deliberately NOT checked yet (see notes at bottom of file): register
liveness, whether operands make semantic sense for the opcode,
predicate logic. Structural validity only, same scope spirv-val
starts with.

USAGE:
    python3 wumcs-validate.py file.umcs
Exit code 0 = valid, 1 = errors found (each printed with instr index).
"""
import sys
import struct
from wumcsc import (
    OPCODES, OP_SHIFT, FMT_SHIFT, PRED_SHIFT,
    FMT_R, FMT_I, FMT_B, FMT_M,
    B_OFFSET_BITS, B_OFFSET_MASK,
    M_SZ_SHIFT, M_SZ_MASK,
    UMCS_MAGIC, VERSION_MAJOR, CAPABILITIES,
)

OPCODE_NAMES = {v[0]: k for k, v in OPCODES.items()}
# EXIT is a special case: categorized under Flow control in the opcode
# table but actually encoded as FMT_R (no branch target). Every other
# opcode's format is exactly what OPCODES says.
EXPECTED_FMT = {v[0]: (FMT_R if k == "EXIT" else v[1]) for k, v in OPCODES.items()}

VALID_SZ_RANGE = range(0, 4)  # 0..3 defined (8/16/32/64-bit); 4-7 reserved


def sign_extend(val, bits):
    if val & (1 << (bits - 1)):
        val -= (1 << bits)
    return val


def validate(data):
    errors = []
    if len(data) < 20:
        return ["file too short to contain a UMCS module header"]

    magic, vmaj, vmin, count, caps, reserved = struct.unpack("<IHHIII", data[:20])
    if magic != UMCS_MAGIC:
        errors.append(f"bad magic: {magic:#010x} (expected {UMCS_MAGIC:#010x})")
        return errors  # nothing else is trustworthy if magic is wrong

    if vmaj != VERSION_MAJOR:
        errors.append(f"unsupported major version {vmaj} (this validator knows v{VERSION_MAJOR})")

    known_caps = 0
    for bit in CAPABILITIES.values():
        known_caps |= bit
    unknown_caps = caps & ~known_caps
    if unknown_caps:
        errors.append(f"header declares unknown capability bits: {unknown_caps:#x}")

    body = data[20:]
    expected_bytes = count * 8
    if len(body) < expected_bytes:
        errors.append(
            f"header claims {count} instructions ({expected_bytes} bytes) "
            f"but only {len(body)} bytes follow"
        )
        count = len(body) // 8  # validate what we actually have

    for i in range(count):
        (w,) = struct.unpack_from("<Q", body, i * 8)
        op = (w >> OP_SHIFT) & 0xFF
        fmt = (w >> FMT_SHIFT) & 0x3
        pred = (w >> PRED_SHIFT) & 0xF

        if op not in OPCODE_NAMES:
            errors.append(f"instr {i}: unknown opcode byte {op:#04x}")
            continue

        name = OPCODE_NAMES[op]
        expected = EXPECTED_FMT[op]
        if fmt != expected:
            errors.append(
                f"instr {i}: {name} encoded with format {fmt}, "
                f"expected format {expected} — op/format bits disagree"
            )
            continue  # further field checks assume correct format; skip

        if fmt == FMT_B:
            raw = w & B_OFFSET_MASK
            offset = sign_extend(raw, B_OFFSET_BITS)
            target = i + offset
            if not (0 <= target < count):
                errors.append(
                    f"instr {i}: {name} branch target {target} is outside "
                    f"the module (0..{count-1})"
                )

        if fmt == FMT_M:
            sz = (w >> M_SZ_SHIFT) & M_SZ_MASK
            if sz not in VALID_SZ_RANGE:
                errors.append(
                    f"instr {i}: {name} uses reserved sz={sz} (valid: 0-3)"
                )

        if pred > 15:  # unreachable given the 4-bit mask, kept for clarity/future-proofing
            errors.append(f"instr {i}: predicate register {pred} out of range")

    return errors


def main():
    if len(sys.argv) != 2:
        print("usage: wumcs-validate.py file.umcs", file=sys.stderr)
        sys.exit(2)
    with open(sys.argv[1], "rb") as f:
        data = f.read()
    errors = validate(data)
    if errors:
        for e in errors:
            print(f"error: {e}", file=sys.stderr)
        print(f"wumcs-validate: {len(errors)} error(s)", file=sys.stderr)
        sys.exit(1)
    print("wumcs-validate: OK")
    sys.exit(0)


if __name__ == "__main__":
    main()