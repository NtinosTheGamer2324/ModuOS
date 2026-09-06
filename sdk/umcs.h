#pragma once
#include <stdint.h>
#include <assert.h>

/* ============================================================
 *  UMCS — Universal MicroCode Shader  (ModuOS)
 *  Instruction Set Header — v2.0 (64-bit fixed-width words)
 *
 *  Change from v1.0: every instruction is a single 64-bit word,
 *  fixed width, no dual-word formats. Rationale (see chat):
 *    - v1.0's 32-bit Format R had no room for a 4-operand FMA
 *      (dst = a*b+c is unencodable in 3 register slots)
 *    - v1.0's 6-bit register fields (64 regs) force early spills
 *    - v1.0's Format M already needed 2 words -> fixed-64 removes
 *      the "is this a 1 or 2 word instruction" branch entirely
 *
 *  Field layout is defined ONLY via shift/mask constants.
 *  Do not reintroduce compiler bitfield structs for the wire
 *  format — see v1.0 notes on implementation-defined layout.
 * ============================================================ */

/* ---------------------------------------------------------
 * Module header
 * --------------------------------------------------------- */
#define UMCS_MAGIC         0x53434D55u   /* "UMCS" little-endian */
#define UMCS_VERSION_MAJOR 2
#define UMCS_VERSION_MINOR 0

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version_major;
    uint16_t version_minor;
    uint32_t instr_count;     /* number of 64-bit words following */
    uint32_t capability_bits; /* UMCS_CAP_* flags                 */
    uint32_t reserved;
} UMCS_ModuleHeader;
static_assert(sizeof(UMCS_ModuleHeader) == 20, "ModuleHeader layout changed");

typedef enum UMCS_Capability {
    UMCS_CAP_DOUBLE_PRECISION = 1u << 0,
    UMCS_CAP_INT64            = 1u << 1,
    UMCS_CAP_ATOMICS_FLOAT    = 1u << 2,
    UMCS_CAP_TEXTURE_ARRAY    = 1u << 3,
    UMCS_CAP_RAY_QUERY        = 1u << 4,
    UMCS_CAP_MESH_SHADING     = 1u << 5,
    UMCS_CAP_SUBGROUP_OPS     = 1u << 6,
} UMCS_Capability;

/* ---------------------------------------------------------
 * Opcodes (unchanged from v1.0)
 * --------------------------------------------------------- */
typedef enum UMCS_Opcode {
    UMCS_OP_MOV     = 0x01,  UMCS_OP_MOVI    = 0x02,
    UMCS_OP_MOVHI   = 0x03,  UMCS_OP_MOVP    = 0x04,
    UMCS_OP_SWZL    = 0x05,  UMCS_OP_PACK    = 0x06,
    UMCS_OP_UNPACK  = 0x07,

    UMCS_OP_IADD    = 0x10,  UMCS_OP_IADDI   = 0x11,
    UMCS_OP_ISUB    = 0x12,  UMCS_OP_IMUL    = 0x13,
    UMCS_OP_IMULHI  = 0x14,  UMCS_OP_IDIV    = 0x15,
    UMCS_OP_IMOD    = 0x16,  UMCS_OP_AND     = 0x17,
    UMCS_OP_OR      = 0x18,  UMCS_OP_XOR     = 0x19,
    UMCS_OP_NOT     = 0x1A,  UMCS_OP_SHL     = 0x1B,
    UMCS_OP_SHR     = 0x1C,  UMCS_OP_SAR     = 0x1D,
    UMCS_OP_ICMP    = 0x1E,  UMCS_OP_ISEL    = 0x1F,
    UMCS_OP_CLZ     = 0x20,  UMCS_OP_POPCNT  = 0x21,
    UMCS_OP_BREV    = 0x22,

    UMCS_OP_FADD    = 0x30,  UMCS_OP_FSUB    = 0x31,
    UMCS_OP_FMUL    = 0x32,  UMCS_OP_FFMA    = 0x33,
    UMCS_OP_FDIV    = 0x34,  UMCS_OP_FRCP    = 0x35,
    UMCS_OP_FRSQ    = 0x36,  UMCS_OP_FSQRT   = 0x37,
    UMCS_OP_FMIN    = 0x38,  UMCS_OP_FMAX    = 0x39,
    UMCS_OP_FSAT    = 0x3A,  UMCS_OP_FCMP    = 0x3B,
    UMCS_OP_FSEL    = 0x3C,  UMCS_OP_F2I     = 0x3D,
    UMCS_OP_I2F     = 0x3E,  UMCS_OP_F2H     = 0x3F,
    UMCS_OP_H2F     = 0x40,  UMCS_OP_FSIN    = 0x41,
    UMCS_OP_FCOS    = 0x42,  UMCS_OP_FLOG2   = 0x43,
    UMCS_OP_FEXP2   = 0x44,

    UMCS_OP_BRA     = 0x50,  UMCS_OP_BRAP    = 0x51,
    UMCS_OP_BRAPN   = 0x52,  UMCS_OP_CALL    = 0x53,
    UMCS_OP_RET     = 0x54,  UMCS_OP_SYNC    = 0x55,
    UMCS_OP_SSY     = 0x56,  UMCS_OP_EMIT    = 0x57,
    UMCS_OP_ENDPRIM = 0x58,  UMCS_OP_KILL    = 0x59,
    UMCS_OP_EXIT    = 0x5F,

    UMCS_OP_LDL     = 0x60,  UMCS_OP_STL     = 0x61,
    UMCS_OP_LDS     = 0x62,  UMCS_OP_STS     = 0x63,
    UMCS_OP_LDG     = 0x64,  UMCS_OP_STG     = 0x65,
    UMCS_OP_LDU     = 0x66,  UMCS_OP_ATOMIC  = 0x67,
    UMCS_OP_TEX     = 0x68,  UMCS_OP_TEXB    = 0x69,
    UMCS_OP_TEXL    = 0x6A,  UMCS_OP_TEXG    = 0x6B,
    UMCS_OP_TEXF    = 0x6C,  UMCS_OP_MEMBAR  = 0x6D,
    UMCS_OP_PREFETCH= 0x6E,
} UMCS_Opcode;

/* ---------------------------------------------------------
 * Format tag, bits [55:54] of every 64-bit word.
 * Universal fields shared by all formats:
 *   [63:56] op    8 bits  -> 256 opcodes
 *   [55:54] fmt   2 bits  -> 4 formats
 *   [53:50] pred  4 bits  -> predicate register (SIMT divergence,
 *                            see note below — every op can be
 *                            predicated, not just branches)
 * --------------------------------------------------------- */
typedef enum UMCS_Format {
    UMCS_FMT_R = 0,   /* reg-reg-reg-reg ALU (supports true FMA) */
    UMCS_FMT_I = 1,   /* reg + 32-bit immediate                  */
    UMCS_FMT_B = 2,   /* branch / flow control                   */
    UMCS_FMT_M = 3,   /* memory / texture                        */
} UMCS_Format;

#define UMCS_OP_SHIFT   56
#define UMCS_OP_MASK    0xFFull
#define UMCS_FMT_SHIFT  54
#define UMCS_FMT_MASK   0x3ull
#define UMCS_PRED_SHIFT 50
#define UMCS_PRED_MASK  0xFull

static inline uint8_t umcs_op(uint64_t w) {
    return (uint8_t)((w >> UMCS_OP_SHIFT) & UMCS_OP_MASK);
}
static inline UMCS_Format umcs_fmt(uint64_t w) {
    return (UMCS_Format)((w >> UMCS_FMT_SHIFT) & UMCS_FMT_MASK);
}
static inline uint8_t umcs_pred(uint64_t w) {
    return (uint8_t)((w >> UMCS_PRED_SHIFT) & UMCS_PRED_MASK);
}
static inline uint64_t umcs_pack_head(uint8_t op, UMCS_Format fmt, uint8_t pred) {
    return ((uint64_t)op   << UMCS_OP_SHIFT)
         | ((uint64_t)fmt  << UMCS_FMT_SHIFT)
         | ((uint64_t)(pred & UMCS_PRED_MASK) << UMCS_PRED_SHIFT);
}

/* ── Format R: 4-operand register ALU (dst, a, b, c + mod) ──
 * [49:40] rd   10 bits (1024 regs)
 * [39:30] rs   10 bits
 * [29:20] rt   10 bits
 * [19:10] ra   10 bits  (4th operand — e.g. FFMA's addend)
 * [9:0]   mod  10 bits  (modifier/negate/abs flags per operand)
 */
#define UMCS_R_RD_SHIFT  40
#define UMCS_R_RS_SHIFT  30
#define UMCS_R_RT_SHIFT  20
#define UMCS_R_RA_SHIFT  10
#define UMCS_R_MOD_SHIFT 0
#define UMCS_R_FIELD_MASK 0x3FFull /* 10 bits */

typedef struct { uint8_t op, pred; uint16_t rd, rs, rt, ra, mod; } UMCS_DecodedR;

static inline uint64_t umcs_encode_r(uint8_t op, uint8_t pred, uint16_t rd,
                                      uint16_t rs, uint16_t rt, uint16_t ra,
                                      uint16_t mod) {
    return umcs_pack_head(op, UMCS_FMT_R, pred)
         | ((uint64_t)(rd  & UMCS_R_FIELD_MASK) << UMCS_R_RD_SHIFT)
         | ((uint64_t)(rs  & UMCS_R_FIELD_MASK) << UMCS_R_RS_SHIFT)
         | ((uint64_t)(rt  & UMCS_R_FIELD_MASK) << UMCS_R_RT_SHIFT)
         | ((uint64_t)(ra  & UMCS_R_FIELD_MASK) << UMCS_R_RA_SHIFT)
         | ((uint64_t)(mod & UMCS_R_FIELD_MASK) << UMCS_R_MOD_SHIFT);
}
static inline UMCS_DecodedR umcs_decode_r(uint64_t w) {
    UMCS_DecodedR d;
    d.op   = umcs_op(w);
    d.pred = umcs_pred(w);
    d.rd   = (uint16_t)((w >> UMCS_R_RD_SHIFT)  & UMCS_R_FIELD_MASK);
    d.rs   = (uint16_t)((w >> UMCS_R_RS_SHIFT)  & UMCS_R_FIELD_MASK);
    d.rt   = (uint16_t)((w >> UMCS_R_RT_SHIFT)  & UMCS_R_FIELD_MASK);
    d.ra   = (uint16_t)((w >> UMCS_R_RA_SHIFT)  & UMCS_R_FIELD_MASK);
    d.mod  = (uint16_t)((w >> UMCS_R_MOD_SHIFT) & UMCS_R_FIELD_MASK);
    return d;
}

/* ── Format I: reg + full 32-bit immediate ──────────────────
 * [49:40] rd    10 bits
 * [39:8]  imm32 32 bits  (inline float/int constants, no table)
 * [7:0]   mod    8 bits
 */
#define UMCS_I_RD_SHIFT   40
#define UMCS_I_IMM_SHIFT  8
#define UMCS_I_MOD_SHIFT  0
#define UMCS_I_IMM_MASK   0xFFFFFFFFull
#define UMCS_I_MOD_MASK   0xFFull

typedef struct { uint8_t op, pred, mod; uint16_t rd; uint32_t imm32; } UMCS_DecodedI;

static inline uint64_t umcs_encode_i(uint8_t op, uint8_t pred, uint16_t rd,
                                      uint32_t imm32, uint8_t mod) {
    return umcs_pack_head(op, UMCS_FMT_I, pred)
         | ((uint64_t)(rd & UMCS_R_FIELD_MASK) << UMCS_I_RD_SHIFT)
         | ((uint64_t)imm32 << UMCS_I_IMM_SHIFT)
         | ((uint64_t)mod << UMCS_I_MOD_SHIFT);
}
static inline UMCS_DecodedI umcs_decode_i(uint64_t w) {
    UMCS_DecodedI d;
    d.op    = umcs_op(w);
    d.pred  = umcs_pred(w);
    d.rd    = (uint16_t)((w >> UMCS_I_RD_SHIFT) & UMCS_R_FIELD_MASK);
    d.imm32 = (uint32_t)((w >> UMCS_I_IMM_SHIFT) & UMCS_I_IMM_MASK);
    d.mod   = (uint8_t)((w >> UMCS_I_MOD_SHIFT) & UMCS_I_MOD_MASK);
    return d;
}

/* ── Format B: branch ─────────────────────────────────────
 * [49:0] offset  50 bits, PC-relative, word granularity.
 * (50 bits is absurd headroom on purpose — never worth
 * revisiting; a real shader will never approach it)
 */
#define UMCS_B_OFFSET_SHIFT 0
#define UMCS_B_OFFSET_BITS  50
#define UMCS_B_OFFSET_MASK  0x3FFFFFFFFFFFFull

typedef struct { uint8_t op, pred; int64_t offset; } UMCS_DecodedB;

static inline uint64_t umcs_encode_b(uint8_t op, uint8_t pred, int64_t offset) {
    return umcs_pack_head(op, UMCS_FMT_B, pred)
         | ((uint64_t)offset & UMCS_B_OFFSET_MASK);
}
static inline UMCS_DecodedB umcs_decode_b(uint64_t w) {
    UMCS_DecodedB d;
    d.op   = umcs_op(w);
    d.pred = umcs_pred(w);
    uint64_t raw = w & UMCS_B_OFFSET_MASK;
    if (raw & (1ull << (UMCS_B_OFFSET_BITS - 1)))
        raw |= ~UMCS_B_OFFSET_MASK;
    d.offset = (int64_t)raw;
    return d;
}

/* ── Format M: memory / texture — now fits in ONE 64-bit word ──
 * [49:40] rd      10 bits
 * [39:30] rs      10 bits (base address register)
 * [29:22] tex      8 bits (texture/sampler index)
 * [21:19] sz       3 bits (data size class)
 * [18:0]  offset  19 bits (byte offset, signed)
 */
#define UMCS_M_RD_SHIFT     40
#define UMCS_M_RS_SHIFT     30
#define UMCS_M_TEX_SHIFT    22
#define UMCS_M_SZ_SHIFT     19
#define UMCS_M_OFFSET_SHIFT 0
#define UMCS_M_TEX_MASK     0xFFull
#define UMCS_M_SZ_MASK      0x7ull
#define UMCS_M_OFFSET_BITS  19
#define UMCS_M_OFFSET_MASK  0x7FFFFull

typedef struct { uint8_t op, pred, tex, sz; uint16_t rd, rs; int32_t offset; } UMCS_DecodedM;

static inline uint64_t umcs_encode_m(uint8_t op, uint8_t pred, uint16_t rd,
                                      uint16_t rs, uint8_t tex, uint8_t sz,
                                      int32_t offset) {
    return umcs_pack_head(op, UMCS_FMT_M, pred)
         | ((uint64_t)(rd  & UMCS_R_FIELD_MASK) << UMCS_M_RD_SHIFT)
         | ((uint64_t)(rs  & UMCS_R_FIELD_MASK) << UMCS_M_RS_SHIFT)
         | ((uint64_t)(tex & UMCS_M_TEX_MASK)   << UMCS_M_TEX_SHIFT)
         | ((uint64_t)(sz  & UMCS_M_SZ_MASK)    << UMCS_M_SZ_SHIFT)
         | ((uint64_t)offset & UMCS_M_OFFSET_MASK);
}
static inline UMCS_DecodedM umcs_decode_m(uint64_t w) {
    UMCS_DecodedM d;
    d.op  = umcs_op(w);
    d.pred= umcs_pred(w);
    d.rd  = (uint16_t)((w >> UMCS_M_RD_SHIFT)  & UMCS_R_FIELD_MASK);
    d.rs  = (uint16_t)((w >> UMCS_M_RS_SHIFT)  & UMCS_R_FIELD_MASK);
    d.tex = (uint8_t)((w >> UMCS_M_TEX_SHIFT) & UMCS_M_TEX_MASK);
    d.sz  = (uint8_t)((w >> UMCS_M_SZ_SHIFT)  & UMCS_M_SZ_MASK);
    uint64_t raw = w & UMCS_M_OFFSET_MASK;
    if (raw & (1ull << (UMCS_M_OFFSET_BITS - 1)))
        raw |= ~UMCS_M_OFFSET_MASK;
    d.offset = (int32_t)raw;
    return d;
}

/* ---------------------------------------------------------
 * Divergence / reconvergence state — kept ABSTRACT.
 *
 * No fixed SIMD/wave width is assumed anywhere in this header.
 * ModuOS has no target hardware yet, so hardcoding e.g. 32
 * (Nvidia-style) or 64 (AMD-style) here would bake in a wrong
 * assumption before it's needed. active_mask is sized generously
 * (64 bits = up to 64 lanes/threads per divergence group) and
 * a backend targeting wider hardware groups multiple UMCS
 * "waves" together; a backend targeting narrower hardware just
 * uses fewer bits of the mask. Nothing here dictates lane count.
 * --------------------------------------------------------- */
typedef struct __attribute__((packed)) {
    uint64_t reconv_pc;
    uint64_t active_mask;   /* up to 64 lanes; narrower HW uses a subset */
} UMCS_DivStackEntry;
static_assert(sizeof(UMCS_DivStackEntry) == 16, "DivStackEntry must be 16 bytes");

#define UMCS_DIV_STACK_DEPTH 16

typedef struct __attribute__((packed)) {
    UMCS_DivStackEntry entries[UMCS_DIV_STACK_DEPTH];
    uint8_t            sp;
} UMCS_DivStack;
static_assert(sizeof(UMCS_DivStack) == UMCS_DIV_STACK_DEPTH * 16 + 1,
              "DivStack layout mismatch");