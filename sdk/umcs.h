#pragma once
#include <stdint.h>
#include <assert.h>

/* ============================================================
 *  UMCS Instruction Word — v1.0
 *  All instruction words are 32-bit (4-byte) aligned.
 *  64-bit memory/texture ops: word0 = header, word1 = payload.
 * ============================================================ */

/* Opcode definitions */
typedef enum UMCS_Opcode {
    /* Data movement */
    UMCS_OP_MOV     = 0x01,  UMCS_OP_MOVI    = 0x02,
    UMCS_OP_MOVHI   = 0x03,  UMCS_OP_MOVP    = 0x04,
    UMCS_OP_SWZL    = 0x05,  UMCS_OP_PACK    = 0x06,
    UMCS_OP_UNPACK  = 0x07,
    /* Integer ALU */
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
    /* Float ALU */
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
    /* Flow control */
    UMCS_OP_BRA     = 0x50,  UMCS_OP_BRAP    = 0x51,
    UMCS_OP_BRAPN   = 0x52,  UMCS_OP_CALL    = 0x53,
    UMCS_OP_RET     = 0x54,  UMCS_OP_SYNC    = 0x55,
    UMCS_OP_SSY     = 0x56,  UMCS_OP_EMIT    = 0x57,
    UMCS_OP_ENDPRIM = 0x58,  UMCS_OP_KILL    = 0x59,
    UMCS_OP_EXIT    = 0x5F,
    /* Memory / Texture */
    UMCS_OP_LDL     = 0x60,  UMCS_OP_STL     = 0x61,
    UMCS_OP_LDS     = 0x62,  UMCS_OP_STS     = 0x63,
    UMCS_OP_LDG     = 0x64,  UMCS_OP_STG     = 0x65,
    UMCS_OP_LDU     = 0x66,  UMCS_OP_ATOMIC  = 0x67,
    UMCS_OP_TEX     = 0x68,  UMCS_OP_TEXB    = 0x69,
    UMCS_OP_TEXL    = 0x6A,  UMCS_OP_TEXG    = 0x6B,
    UMCS_OP_TEXF    = 0x6C,  UMCS_OP_MEMBAR  = 0x6D,
    UMCS_OP_PREFETCH= 0x6E,
} UMCS_Opcode;

/* Instruction format tag (bits [23:22]) */
typedef enum UMCS_Format {
    UMCS_FMT_R = 0,   /* reg-reg ALU          */
    UMCS_FMT_I = 1,   /* reg + imm16          */
    UMCS_FMT_B = 2,   /* branch / flow ctrl   */
    UMCS_FMT_M = 3,   /* memory / texture     */
} UMCS_Format;

/* ── Format R: register-register ─────────────────────────── */
typedef struct __attribute__((packed)) {
    uint32_t mod : 4;   /* [3:0]   modifier flags           */
    uint32_t rt  : 6;   /* [9:4]   source reg 2             */
    uint32_t rs  : 6;   /* [15:10] source reg 1             */
    uint32_t rd  : 6;   /* [21:16] destination reg          */
    uint32_t fmt : 2;   /* [23:22] = UMCS_FMT_R (0b00)     */
    uint32_t op  : 8;   /* [31:24] opcode                   */
} UMCS_InstrR;

/* ── Format I: immediate ──────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint32_t imm16 : 16; /* [15:0]  immediate value          */
    uint32_t rd    : 6;  /* [21:16] dst or base reg          */
    uint32_t fmt   : 2;  /* [23:22] = UMCS_FMT_I (0b01)     */
    uint32_t op    : 8;  /* [31:24] opcode                   */
} UMCS_InstrI;

/* ── Format B: branch ─────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    int32_t  offset : 18; /* [17:0]  PC-relative word offset  */
    uint32_t pn     : 4;  /* [21:18] predicate reg / cond     */
    uint32_t fmt    : 2;  /* [23:22] = UMCS_FMT_B (0b10)     */
    uint32_t op     : 8;  /* [31:24] opcode                   */
} UMCS_InstrB;

/* ── Format M: memory / texture (first 32-bit word) ──────── */
typedef struct __attribute__((packed)) {
    uint32_t sz  : 2;    /* [1:0]   data size 00-8b…11-128b  */
    uint32_t tex : 4;    /* [5:2]   texture / sampler index  */
    uint32_t rs  : 6;    /* [11:6]  base address register    */
    uint32_t rd  : 6;    /* [17:12] destination register     */
    uint32_t mod : 4;    /* [21:18] cache/atomic mode flags  */
    uint32_t fmt : 2;    /* [23:22] = UMCS_FMT_M (0b11)     */
    uint32_t op  : 8;    /* [31:24] opcode                   */
} UMCS_InstrM_W0;

/* Format M second word (offset / sampler / reserved) */
typedef struct __attribute__((packed)) {
    uint32_t reserved : 16; /* [31:16] cache hints / reserved   */
    uint32_t offset   : 16; /* [15:0]  byte offset / samp idx   */
} UMCS_InstrM_W1;

/* ── Master union for all instruction types ───────────────── */
typedef union {
    uint32_t     raw;    /* raw 32-bit word; always valid     */
    UMCS_InstrR  r;
    UMCS_InstrI  i;
    UMCS_InstrB  b;
    UMCS_InstrM_W0 m;
} UMCS_Instr;

/* 64-bit pair for M-format instructions */
typedef struct __attribute__((packed)) {
    UMCS_InstrM_W0 w0;
    UMCS_InstrM_W1 w1;
} UMCS_InstrM64;

/* ── Divergence stack entry ───────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint32_t reconv_pc;    /* reconvergence program counter    */
    uint32_t active_mask;  /* thread active mask at this level */
} UMCS_DivStackEntry;

#define UMCS_DIV_STACK_DEPTH 16

typedef struct {
    UMCS_DivStackEntry entries[UMCS_DIV_STACK_DEPTH];
    uint8_t            sp;  /* stack pointer, 0 = empty         */
} UMCS_DivStack;

/* ── Decoder helper: extract opcode + format in one op ────── */
static inline uint8_t       umcs_op(UMCS_Instr w)  { return w.r.op; }
static inline UMCS_Format   umcs_fmt(UMCS_Instr w) { return (UMCS_Format)w.r.fmt; }

/* ── Compile-time size assertions ────────────────────────── */
static_assert(sizeof(UMCS_Instr)       == 4,  "UMCS_Instr must be 4 bytes");
static_assert(sizeof(UMCS_InstrM64)    == 8,  "UMCS_InstrM64 must be 8 bytes");
static_assert(sizeof(UMCS_DivStackEntry) == 8, "DivStackEntry must be 8 bytes");
static_assert(sizeof(UMCS_DivStack)    == UMCS_DIV_STACK_DEPTH * 8 + 1,
              "DivStack layout mismatch");