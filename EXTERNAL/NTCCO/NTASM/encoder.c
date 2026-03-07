#include "encoder.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* =========================================================
 * Code buffer
 * ========================================================= */

CodeBuffer *codebuf_create(int capacity) {
    CodeBuffer *b = malloc(sizeof(CodeBuffer));
    b->code     = malloc(capacity);
    b->capacity = capacity;
    b->size     = 0;
    b->relocs   = NULL;
    return b;
}

void codebuf_free(CodeBuffer *b) {
    Reloc *r = b->relocs;
    while (r) { Reloc *n = r->next; free(r); r = n; }
    free(b->code);
    free(b);
}

void codebuf_emit_byte(CodeBuffer *b, uint8_t v) {
    if (b->size >= b->capacity) {
        b->capacity *= 2;
        b->code = realloc(b->code, b->capacity);
    }
    b->code[b->size++] = v;
}

void codebuf_emit_bytes(CodeBuffer *b, const uint8_t *p, int n) {
    for (int i = 0; i < n; i++) codebuf_emit_byte(b, p[i]);
}

void codebuf_emit_word(CodeBuffer *b, uint16_t v) {
    codebuf_emit_byte(b, v & 0xff);
    codebuf_emit_byte(b, (v >> 8) & 0xff);
}

void codebuf_emit_dword(CodeBuffer *b, uint32_t v) {
    codebuf_emit_byte(b, v & 0xff);
    codebuf_emit_byte(b, (v >> 8) & 0xff);
    codebuf_emit_byte(b, (v >> 16) & 0xff);
    codebuf_emit_byte(b, (v >> 24) & 0xff);
}

void codebuf_emit_qword(CodeBuffer *b, uint64_t v) {
    codebuf_emit_dword(b, (uint32_t)(v & 0xffffffff));
    codebuf_emit_dword(b, (uint32_t)(v >> 32));
}

void codebuf_emit_label_ref(CodeBuffer *b, const char *label,
                             int addend, int section) {
    Reloc *r = malloc(sizeof(Reloc));
    r->offset  = b->size;
    r->size    = 4;
    r->addend  = addend;
    r->section = section;
    strncpy(r->label, label, 255);
    r->label[255] = '\0';
    r->next    = b->relocs;
    b->relocs  = r;
    codebuf_emit_dword(b, 0); /* placeholder */
}

/* =========================================================
 * Register table
 * =========================================================
 * Each entry: { name, RegisterID, hw_num, size_bytes }
 */
typedef struct { const char *name; RegisterID id; int hw; int sz; } RegEntry;
static const RegEntry reg_table[] = {
    /* 64-bit */
    {"rax",  REG_RAX,  0,  8}, {"rcx",  REG_RCX,  1,  8},
    {"rdx",  REG_RDX,  2,  8}, {"rbx",  REG_RBX,  3,  8},
    {"rsp",  REG_RSP,  4,  8}, {"rbp",  REG_RBP,  5,  8},
    {"rsi",  REG_RSI,  6,  8}, {"rdi",  REG_RDI,  7,  8},
    {"r8",   REG_R8,   8,  8}, {"r9",   REG_R9,   9,  8},
    {"r10",  REG_R10, 10,  8}, {"r11",  REG_R11, 11,  8},
    {"r12",  REG_R12, 12,  8}, {"r13",  REG_R13, 13,  8},
    {"r14",  REG_R14, 14,  8}, {"r15",  REG_R15, 15,  8},
    /* 32-bit */
    {"eax",  REG_EAX,  0,  4}, {"ecx",  REG_ECX,  1,  4},
    {"edx",  REG_EDX,  2,  4}, {"ebx",  REG_EBX,  3,  4},
    {"esp",  REG_ESP,  4,  4}, {"ebp",  REG_EBP,  5,  4},
    {"esi",  REG_ESI,  6,  4}, {"edi",  REG_EDI,  7,  4},
    {"r8d",  REG_R8D,  8,  4}, {"r9d",  REG_R9D,  9,  4},
    {"r10d", REG_R10D,10,  4}, {"r11d", REG_R11D,11,  4},
    {"r12d", REG_R12D,12,  4}, {"r13d", REG_R13D,13,  4},
    {"r14d", REG_R14D,14,  4}, {"r15d", REG_R15D,15,  4},
    /* 16-bit */
    {"ax",   REG_AX,   0,  2}, {"cx",   REG_CX,   1,  2},
    {"dx",   REG_DX,   2,  2}, {"bx",   REG_BX,   3,  2},
    {"sp",   REG_SP,   4,  2}, {"bp",   REG_BP,   5,  2},
    {"si",   REG_SI,   6,  2}, {"di",   REG_DI,   7,  2},
    {"r8w",  REG_R8W,  8,  2}, {"r9w",  REG_R9W,  9,  2},
    {"r10w", REG_R10W,10,  2}, {"r11w", REG_R11W,11,  2},
    {"r12w", REG_R12W,12,  2}, {"r13w", REG_R13W,13,  2},
    {"r14w", REG_R14W,14,  2}, {"r15w", REG_R15W,15,  2},
    /* 8-bit low */
    {"al",   REG_AL,   0,  1}, {"cl",   REG_CL,   1,  1},
    {"dl",   REG_DL,   2,  1}, {"bl",   REG_BL,   3,  1},
    {"spl",  REG_SPL,  4,  1}, {"bpl",  REG_BPL,  5,  1},
    {"sil",  REG_SIL,  6,  1}, {"dil",  REG_DIL,  7,  1},
    {"r8b",  REG_R8B,  8,  1}, {"r9b",  REG_R9B,  9,  1},
    {"r10b", REG_R10B,10,  1}, {"r11b", REG_R11B,11,  1},
    {"r12b", REG_R12B,12,  1}, {"r13b", REG_R13B,13,  1},
    {"r14b", REG_R14B,14,  1}, {"r15b", REG_R15B,15,  1},
    /* 8-bit high */
    {"ah",   REG_AH,   4,  1}, {"ch",   REG_CH,   5,  1},
    {"dh",   REG_DH,   6,  1}, {"bh",   REG_BH,   7,  1},
    {NULL, REG_INVALID, 0, 0}
};

RegisterID register_id(const char *name) {
    for (int i = 0; reg_table[i].name; i++)
        if (strcasecmp(reg_table[i].name, name) == 0)
            return reg_table[i].id;
    return REG_INVALID;
}

int register_hw(RegisterID id) {
    for (int i = 0; reg_table[i].name; i++)
        if (reg_table[i].id == id)
            return reg_table[i].hw;
    return 0;
}

int register_size(RegisterID id) {
    for (int i = 0; reg_table[i].name; i++)
        if (reg_table[i].id == id)
            return reg_table[i].sz;
    return 8;
}

/* Returns 1 if the register is r8-r15 (needs REX.B or REX.R) */
int register_needs_rex(RegisterID id) {
    return register_hw(id) >= 8;
}

/* =========================================================
 * Internal encoding helpers
 * ========================================================= */

/* REX byte: W=64-bit, R=reg ext, X=sib idx ext, B=rm/base ext */
static void emit_rex(CodeBuffer *b, int W, int R, int X, int B) {
    uint8_t rex = 0x40 | (W<<3) | (R<<2) | (X<<1) | B;
    codebuf_emit_byte(b, rex);
}

/* Emit REX only if any field is set (avoid unnecessary REX) */
static void emit_rex_if(CodeBuffer *b, int W, int R, int X, int B) {
    if (W || R || X || B) emit_rex(b, W, R, X, B);
}

static void emit_modrm(CodeBuffer *b, int mod, int reg, int rm) {
    codebuf_emit_byte(b, ((mod&3)<<6)|((reg&7)<<3)|(rm&7));
}

static void emit_sib(CodeBuffer *b, int scale, int index, int base) {
    codebuf_emit_byte(b, ((scale&3)<<6)|((index&7)<<3)|(base&7));
}

/* Encode a displacement: 0, 1, or 4 bytes */
static int disp_size(int64_t disp, int base_hw) {
    /* RBP/R13 base with no displacement still needs disp8=0 */
    if (disp == 0 && (base_hw & 7) != 5) return 0;
    if (disp >= -128 && disp <= 127)      return 1;
    return 4;
}

static void emit_disp(CodeBuffer *b, int64_t disp, int sz) {
    if (sz == 1) codebuf_emit_byte(b, (uint8_t)(int8_t)disp);
    else if (sz == 4) codebuf_emit_dword(b, (uint32_t)(int32_t)disp);
}

/*
 * Emit ModRM + optional SIB + displacement for a memory operand.
 * reg_field = the /r or /digit field.
 */
static void emit_mem_modrm(CodeBuffer *b, int reg_field, const Operand *mem) {
    int base_hw  = (mem->value.mem.base  != REG_INVALID)
                 ? register_hw(mem->value.mem.base) : -1;
    int idx_hw   = (mem->value.mem.index != REG_INVALID)
                 ? register_hw(mem->value.mem.index) : -1;
    int64_t disp = mem->value.mem.disp;
    int scale    = mem->value.mem.scale;

    /* Scale factor -> SIB.ss encoding */
    int ss = 0;
    if      (scale == 2) ss = 1;
    else if (scale == 4) ss = 2;
    else if (scale == 8) ss = 3;

    if (base_hw < 0 && idx_hw < 0) {
        /* disp32 only: mod=00, rm=101, SIB not needed in 64-bit
         * Actually in 64-bit RIP-relative requires SIB trick; use
         * mod=00 rm=100 SIB(ss=0,idx=100,base=101) for plain disp32 */
        emit_modrm(b, 0, reg_field & 7, 4);
        emit_sib(b, 0, 4, 5);
        codebuf_emit_dword(b, (uint32_t)(int32_t)disp);
        return;
    }

    if (idx_hw >= 0) {
        /* Need SIB */
        int ds = disp_size(disp, base_hw < 0 ? 0 : base_hw);
        int mod = (ds == 0) ? 0 : (ds == 1) ? 1 : 2;
        int rm_base = (base_hw < 0) ? 5 : base_hw;
        if (base_hw < 0) mod = 0; /* no base: disp32 in SIB */
        emit_modrm(b, mod, reg_field & 7, 4);
        emit_sib(b, ss, idx_hw & 7, rm_base & 7);
        if (base_hw < 0)
            codebuf_emit_dword(b, (uint32_t)(int32_t)disp);
        else
            emit_disp(b, disp, ds);
    } else {
        /* No index */
        if ((base_hw & 7) == 4) {
            /* RSP/R12 base: requires SIB with index=none(4) */
            int ds = disp_size(disp, base_hw);
            int mod = (ds == 0) ? 0 : (ds == 1) ? 1 : 2;
            emit_modrm(b, mod, reg_field & 7, 4);
            emit_sib(b, 0, 4, base_hw & 7);
            emit_disp(b, disp, ds);
        } else {
            int ds = disp_size(disp, base_hw);
            int mod = (ds == 0) ? 0 : (ds == 1) ? 1 : 2;
            emit_modrm(b, mod, reg_field & 7, base_hw & 7);
            emit_disp(b, disp, ds);
        }
    }
}

/* Determine effective operand size from registers and mode */
static int effective_size(Operand dst, Operand src, AssemblyMode mode) {
    if (dst.type == OP_REG) return register_size(dst.value.reg);
    if (src.type == OP_REG) return register_size(src.value.reg);
    if (dst.type == OP_MEM && dst.size_hint) return (int)dst.size_hint;
    if (src.type == OP_MEM && src.size_hint) return (int)src.size_hint;
    return (mode == MODE_64BIT) ? 8 : (mode == MODE_32BIT) ? 4 : 2;
}

/* Emit operand-size prefix for 16-bit operands when not in 16-bit mode */
static void emit_opsz_prefix(CodeBuffer *b, int sz, AssemblyMode mode) {
    if (sz == 2 && mode != MODE_16BIT) codebuf_emit_byte(b, 0x66);
    if (sz != 2 && mode == MODE_16BIT) codebuf_emit_byte(b, 0x66);
}

/*
 * Generic alu/mov helper: opcode, /digit, REX setup, then ModRM.
 * Handles reg-reg, reg-mem, mem-reg, reg-imm, mem-imm.
 *
 *   op_rr  = opcode for r/m, r  (e.g. 0x01 for ADD r/m64,r64)
 *   op_ri  = opcode for r/m, imm with sign-extension (0x81/0x83)
 *   digit  = ModRM /digit for imm form
 *   allow_imm8 = use 0x83 form for sign-extended imm8
 */
static int encode_alu(CodeBuffer *b, Operand dst, Operand src,
                      AssemblyMode mode,
                      uint8_t op_rr_store,  /* opcode: rm = r */
                      uint8_t op_rr_load,   /* opcode: r  = rm (for mov) */
                      uint8_t op_imm,       /* 0x81 */
                      int     digit,        /* /digit for imm */
                      int     is_mov)       /* 1 = mov (no 0x83 form) */
{
    int sz = effective_size(dst, src, mode);
    emit_opsz_prefix(b, sz, mode);

    int W = (sz == 8) ? 1 : 0;

    if (dst.type == OP_REG && src.type == OP_REG) {
        int dreg = register_hw(dst.value.reg);
        int sreg = register_hw(src.value.reg);
        emit_rex_if(b, W, (sreg>>3)&1, 0, (dreg>>3)&1);
        codebuf_emit_byte(b, op_rr_store);
        emit_modrm(b, 3, sreg & 7, dreg & 7);
        return 0;
    }

    if (dst.type == OP_REG && src.type == OP_MEM) {
        int dreg = register_hw(dst.value.reg);
        int brex  = (src.value.mem.base  != REG_INVALID)
                  ? (register_hw(src.value.mem.base) >> 3) & 1 : 0;
        int xrex  = (src.value.mem.index != REG_INVALID)
                  ? (register_hw(src.value.mem.index) >> 3) & 1 : 0;
        emit_rex_if(b, W, (dreg>>3)&1, xrex, brex);
        codebuf_emit_byte(b, op_rr_load);
        emit_mem_modrm(b, dreg, &src);
        return 0;
    }

    if (dst.type == OP_MEM && src.type == OP_REG) {
        int sreg = register_hw(src.value.reg);
        int brex  = (dst.value.mem.base  != REG_INVALID)
                  ? (register_hw(dst.value.mem.base) >> 3) & 1 : 0;
        int xrex  = (dst.value.mem.index != REG_INVALID)
                  ? (register_hw(dst.value.mem.index) >> 3) & 1 : 0;
        emit_rex_if(b, W, (sreg>>3)&1, xrex, brex);
        codebuf_emit_byte(b, op_rr_store);
        emit_mem_modrm(b, sreg, &dst);
        return 0;
    }

    if (dst.type == OP_REG && src.type == OP_IMM) {
        int dreg = register_hw(dst.value.reg);
        int64_t imm = src.value.imm;
        /* For mov in 64-bit with large imm: use REX.W + B8+rd + imm64 */
        if (is_mov && sz == 8 && (imm > 0x7fffffff || imm < -0x80000000LL)) {
            emit_rex(b, 1, 0, 0, (dreg>>3)&1);
            codebuf_emit_byte(b, 0xB8 + (dreg & 7));
            codebuf_emit_qword(b, (uint64_t)imm);
            return 0;
        }
        if (is_mov) {
            /* mov r/m, imm32 (sign-extended to 64) via C7 /0 */
            emit_rex_if(b, W, 0, 0, (dreg>>3)&1);
            if (sz == 1) {
                codebuf_emit_byte(b, 0xC6);
                emit_modrm(b, 3, 0, dreg & 7);
                codebuf_emit_byte(b, (uint8_t)imm);
            } else {
                codebuf_emit_byte(b, 0xC7);
                emit_modrm(b, 3, 0, dreg & 7);
                if (sz == 2) codebuf_emit_word(b, (uint16_t)imm);
                else         codebuf_emit_dword(b, (uint32_t)(int32_t)imm);
            }
            return 0;
        }
        /* For 8-bit ALU, always use 0x80 form */
        if (sz == 1) {
            emit_rex_if(b, W, 0, 0, (dreg>>3)&1);
            codebuf_emit_byte(b, 0x80);
            emit_modrm(b, 3, digit & 7, dreg & 7);
            codebuf_emit_byte(b, (uint8_t)imm);
            return 0;
        }
        /* 16/32/64-bit: use 0x83 if imm fits in sign-extended byte */
        if (imm >= -128 && imm <= 127) {
            emit_rex_if(b, W, 0, 0, (dreg>>3)&1);
            codebuf_emit_byte(b, 0x83);
            emit_modrm(b, 3, digit & 7, dreg & 7);
            codebuf_emit_byte(b, (uint8_t)(int8_t)imm);
        } else {
            emit_rex_if(b, W, 0, 0, (dreg>>3)&1);
            codebuf_emit_byte(b, op_imm);
            emit_modrm(b, 3, digit & 7, dreg & 7);
            if (sz == 2) codebuf_emit_word(b, (uint16_t)imm);
            else         codebuf_emit_dword(b, (uint32_t)(int32_t)imm);
        }
        return 0;
    }

    if (dst.type == OP_MEM && src.type == OP_IMM) {
        int64_t imm = src.value.imm;
        int brex = (dst.value.mem.base  != REG_INVALID)
                 ? (register_hw(dst.value.mem.base)  >> 3) & 1 : 0;
        int xrex = (dst.value.mem.index != REG_INVALID)
                 ? (register_hw(dst.value.mem.index) >> 3) & 1 : 0;
        if (is_mov) {
            emit_rex_if(b, W, 0, xrex, brex);
            codebuf_emit_byte(b, sz == 1 ? 0xC6 : 0xC7);
            emit_mem_modrm(b, 0, &dst);
            if (sz == 1)       codebuf_emit_byte(b,  (uint8_t)imm);
            else if (sz == 2)  codebuf_emit_word(b,  (uint16_t)imm);
            else               codebuf_emit_dword(b, (uint32_t)(int32_t)imm);
            return 0;
        }
        /* For 8-bit ALU, always use 0x80 form */
        if (sz == 1) {
            emit_rex_if(b, W, 0, xrex, brex);
            codebuf_emit_byte(b, 0x80);
            emit_mem_modrm(b, digit & 7, &dst);
            codebuf_emit_byte(b, (uint8_t)imm);
            return 0;
        }
        /* 16/32/64-bit: use 0x83 if imm fits in sign-extended byte */
        if (imm >= -128 && imm <= 127) {
            emit_rex_if(b, W, 0, xrex, brex);
            codebuf_emit_byte(b, 0x83);
            emit_mem_modrm(b, digit & 7, &dst);
            codebuf_emit_byte(b, (uint8_t)(int8_t)imm);
        } else {
            emit_rex_if(b, W, 0, xrex, brex);
            codebuf_emit_byte(b, op_imm);
            emit_mem_modrm(b, digit & 7, &dst);
            if (sz == 2) codebuf_emit_word(b,  (uint16_t)imm);
            else         codebuf_emit_dword(b, (uint32_t)(int32_t)imm);
        }
        return 0;
    }

    fprintf(stderr, "encode_alu: unsupported operand combination\n");
    return -1;
}

/* =========================================================
 * Public instruction encoders
 * ========================================================= */

int encode_mov(CodeBuffer *b, Operand dst, Operand src, AssemblyMode mode) {
    return encode_alu(b, dst, src, mode, 0x89, 0x8B, 0xC7, 0, 1);
}
int encode_add(CodeBuffer *b, Operand dst, Operand src, AssemblyMode mode) {
    return encode_alu(b, dst, src, mode, 0x01, 0x03, 0x81, 0, 0);
}
int encode_sub(CodeBuffer *b, Operand dst, Operand src, AssemblyMode mode) {
    return encode_alu(b, dst, src, mode, 0x29, 0x2B, 0x81, 5, 0);
}
int encode_and(CodeBuffer *b, Operand dst, Operand src, AssemblyMode mode) {
    return encode_alu(b, dst, src, mode, 0x21, 0x23, 0x81, 4, 0);
}
int encode_or(CodeBuffer *b, Operand dst, Operand src, AssemblyMode mode) {
    return encode_alu(b, dst, src, mode, 0x09, 0x0B, 0x81, 1, 0);
}
int encode_xor(CodeBuffer *b, Operand dst, Operand src, AssemblyMode mode) {
    return encode_alu(b, dst, src, mode, 0x31, 0x33, 0x81, 6, 0);
}
int encode_cmp(CodeBuffer *b, Operand dst, Operand src, AssemblyMode mode) {
    return encode_alu(b, dst, src, mode, 0x39, 0x3B, 0x81, 7, 0);
}
int encode_test(CodeBuffer *b, Operand dst, Operand src, AssemblyMode mode) {
    /* TEST has no imm8 short form — use 0xF7 /0 for imm */
    if (dst.type == OP_REG && src.type == OP_IMM) {
        int sz  = register_size(dst.value.reg);
        int reg = register_hw(dst.value.reg);
        int W   = (sz == 8) ? 1 : 0;
        emit_opsz_prefix(b, sz, mode);
        emit_rex_if(b, W, 0, 0, (reg>>3)&1);
        codebuf_emit_byte(b, sz == 1 ? 0xF6 : 0xF7);
        emit_modrm(b, 3, 0, reg & 7);
        if (sz == 1)      codebuf_emit_byte(b,  (uint8_t)src.value.imm);
        else if (sz == 2) codebuf_emit_word(b,  (uint16_t)src.value.imm);
        else              codebuf_emit_dword(b, (uint32_t)src.value.imm);
        return 0;
    }
    return encode_alu(b, dst, src, mode, 0x85, 0x85, 0xF7, 0, 0);
}

/* IMUL: two-operand form (dst *= src) using 0F AF, or imm form */
int encode_imul(CodeBuffer *b, Operand dst, Operand src, AssemblyMode mode) {
    if (dst.type != OP_REG) return -1;
    int sz   = register_size(dst.value.reg);
    int dreg = register_hw(dst.value.reg);
    int W    = (sz == 8) ? 1 : 0;
    emit_opsz_prefix(b, sz, mode);

    if (src.type == OP_REG) {
        int sreg = register_hw(src.value.reg);
        emit_rex_if(b, W, (dreg>>3)&1, 0, (sreg>>3)&1);
        codebuf_emit_byte(b, 0x0F);
        codebuf_emit_byte(b, 0xAF);
        emit_modrm(b, 3, dreg & 7, sreg & 7);
        return 0;
    }
    if (src.type == OP_MEM) {
        int brex = (src.value.mem.base  != REG_INVALID)
                 ? (register_hw(src.value.mem.base)  >> 3) & 1 : 0;
        int xrex = (src.value.mem.index != REG_INVALID)
                 ? (register_hw(src.value.mem.index) >> 3) & 1 : 0;
        emit_rex_if(b, W, (dreg>>3)&1, xrex, brex);
        codebuf_emit_byte(b, 0x0F);
        codebuf_emit_byte(b, 0xAF);
        emit_mem_modrm(b, dreg, &src);
        return 0;
    }
    if (src.type == OP_IMM) {
        int64_t imm = src.value.imm;
        emit_rex_if(b, W, (dreg>>3)&1, 0, (dreg>>3)&1);
        if (imm >= -128 && imm <= 127) {
            codebuf_emit_byte(b, 0x6B);
            emit_modrm(b, 3, dreg & 7, dreg & 7);
            codebuf_emit_byte(b, (uint8_t)(int8_t)imm);
        } else {
            codebuf_emit_byte(b, 0x69);
            emit_modrm(b, 3, dreg & 7, dreg & 7);
            codebuf_emit_dword(b, (uint32_t)(int32_t)imm);
        }
        return 0;
    }
    return -1;
}

/* Unary: INC=0, DEC=1 use FE/FF; NOT=2, NEG=3 use F6/F7 */
static int encode_unary(CodeBuffer *b, Operand op, AssemblyMode mode,
                        int digit) {
    int sz  = (op.type == OP_REG) ? register_size(op.value.reg)
                                  : (op.size_hint ? (int)op.size_hint : 8);
    int W   = (sz == 8) ? 1 : 0;
    emit_opsz_prefix(b, sz, mode);
    /* INC=0,DEC=1 use FE/FF; NOT=2,NEG=3 use F6/F7 */
    uint8_t opc = (digit <= 1) ? (sz == 1 ? 0xFE : 0xFF)
                               : (sz == 1 ? 0xF6 : 0xF7);

    if (op.type == OP_REG) {
        int reg = register_hw(op.value.reg);
        emit_rex_if(b, W, 0, 0, (reg>>3)&1);
        codebuf_emit_byte(b, opc);
        emit_modrm(b, 3, digit & 7, reg & 7);
        return 0;
    }
    if (op.type == OP_MEM) {
        int brex = (op.value.mem.base  != REG_INVALID)
                 ? (register_hw(op.value.mem.base)  >> 3) & 1 : 0;
        int xrex = (op.value.mem.index != REG_INVALID)
                 ? (register_hw(op.value.mem.index) >> 3) & 1 : 0;
        emit_rex_if(b, W, 0, xrex, brex);
        codebuf_emit_byte(b, opc);
        emit_mem_modrm(b, digit & 7, &op);
        return 0;
    }
    return -1;
}

int encode_inc(CodeBuffer *b, Operand op, AssemblyMode mode) { return encode_unary(b, op, mode, 0); }
int encode_dec(CodeBuffer *b, Operand op, AssemblyMode mode) { return encode_unary(b, op, mode, 1); }
int encode_neg(CodeBuffer *b, Operand op, AssemblyMode mode) { return encode_unary(b, op, mode, 3); }
int encode_not(CodeBuffer *b, Operand op, AssemblyMode mode) { return encode_unary(b, op, mode, 2); }

/* Shifts: SHL=4, SHR=5, SAR=7, ROL=0, ROR=1 */
static int encode_shift(CodeBuffer *b, Operand dst, Operand cnt,
                        AssemblyMode mode, int digit) {
    int sz  = (dst.type == OP_REG) ? register_size(dst.value.reg) : 8;
    int W   = (sz == 8) ? 1 : 0;
    emit_opsz_prefix(b, sz, mode);

    if (dst.type == OP_REG) {
        int reg = register_hw(dst.value.reg);
        if (cnt.type == OP_IMM) {
            int64_t n = cnt.value.imm;
            emit_rex_if(b, W, 0, 0, (reg>>3)&1);
            if (n == 1) {
                codebuf_emit_byte(b, sz == 1 ? 0xD0 : 0xD1);
                emit_modrm(b, 3, digit & 7, reg & 7);
            } else {
                codebuf_emit_byte(b, sz == 1 ? 0xC0 : 0xC1);
                emit_modrm(b, 3, digit & 7, reg & 7);
                codebuf_emit_byte(b, (uint8_t)n & 0x3F);
            }
            return 0;
        }
        if (cnt.type == OP_REG && cnt.value.reg == REG_CL) {
            emit_rex_if(b, W, 0, 0, (reg>>3)&1);
            codebuf_emit_byte(b, sz == 1 ? 0xD2 : 0xD3);
            emit_modrm(b, 3, digit & 7, reg & 7);
            return 0;
        }
    }
    return -1;
}

int encode_shl(CodeBuffer *b, Operand d, Operand c, AssemblyMode m) { return encode_shift(b,d,c,m,4); }
int encode_shr(CodeBuffer *b, Operand d, Operand c, AssemblyMode m) { return encode_shift(b,d,c,m,5); }
int encode_sar(CodeBuffer *b, Operand d, Operand c, AssemblyMode m) { return encode_shift(b,d,c,m,7); }
int encode_rol(CodeBuffer *b, Operand d, Operand c, AssemblyMode m) { return encode_shift(b,d,c,m,0); }
int encode_ror(CodeBuffer *b, Operand d, Operand c, AssemblyMode m) { return encode_shift(b,d,c,m,1); }

int encode_push(CodeBuffer *b, Operand op, AssemblyMode mode) {
    (void)mode;
    if (op.type == OP_REG) {
        int reg = register_hw(op.value.reg);
        if (reg >= 8) emit_rex(b, 0, 0, 0, 1);
        codebuf_emit_byte(b, 0x50 + (reg & 7));
        return 0;
    }
    if (op.type == OP_IMM) {
        int64_t imm = op.value.imm;
        if (imm >= -128 && imm <= 127) {
            codebuf_emit_byte(b, 0x6A);
            codebuf_emit_byte(b, (uint8_t)(int8_t)imm);
        } else {
            codebuf_emit_byte(b, 0x68);
            codebuf_emit_dword(b, (uint32_t)(int32_t)imm);
        }
        return 0;
    }
    if (op.type == OP_MEM) {
        int brex = (op.value.mem.base  != REG_INVALID)
                 ? (register_hw(op.value.mem.base)  >> 3) & 1 : 0;
        int xrex = (op.value.mem.index != REG_INVALID)
                 ? (register_hw(op.value.mem.index) >> 3) & 1 : 0;
        emit_rex_if(b, 0, 0, xrex, brex);
        codebuf_emit_byte(b, 0xFF);
        emit_mem_modrm(b, 6, &op);
        return 0;
    }
    return -1;
}

int encode_pop(CodeBuffer *b, Operand op, AssemblyMode mode) {
    (void)mode;
    if (op.type == OP_REG) {
        int reg = register_hw(op.value.reg);
        if (reg >= 8) emit_rex(b, 0, 0, 0, 1);
        codebuf_emit_byte(b, 0x58 + (reg & 7));
        return 0;
    }
    if (op.type == OP_MEM) {
        int brex = (op.value.mem.base  != REG_INVALID)
                 ? (register_hw(op.value.mem.base)  >> 3) & 1 : 0;
        int xrex = (op.value.mem.index != REG_INVALID)
                 ? (register_hw(op.value.mem.index) >> 3) & 1 : 0;
        emit_rex_if(b, 0, 0, xrex, brex);
        codebuf_emit_byte(b, 0x8F);
        emit_mem_modrm(b, 0, &op);
        return 0;
    }
    return -1;
}

int encode_ret(CodeBuffer *b, AssemblyMode mode)  { (void)mode; codebuf_emit_byte(b, 0xC3); return 0; }
int encode_nop(CodeBuffer *b, AssemblyMode mode)  { (void)mode; codebuf_emit_byte(b, 0x90); return 0; }
int encode_hlt(CodeBuffer *b, AssemblyMode mode)  { (void)mode; codebuf_emit_byte(b, 0xF4); return 0; }
int encode_cli(CodeBuffer *b, AssemblyMode mode)  { (void)mode; codebuf_emit_byte(b, 0xFA); return 0; }
int encode_sti(CodeBuffer *b, AssemblyMode mode)  { (void)mode; codebuf_emit_byte(b, 0xFB); return 0; }
int encode_syscall(CodeBuffer *b, AssemblyMode mode) {
    (void)mode;
    codebuf_emit_byte(b, 0x0F);
    codebuf_emit_byte(b, 0x05);
    return 0;
}
int encode_int(CodeBuffer *b, uint8_t vec, AssemblyMode mode) {
    (void)mode;
    if (vec == 3) { codebuf_emit_byte(b, 0xCC); return 0; }
    codebuf_emit_byte(b, 0xCD);
    codebuf_emit_byte(b, vec);
    return 0;
}

/* Jumps and calls.
 * 'offset' is the raw rel32 value = target_addr - end_of_instruction.
 * Callers must pre-compute this correctly. */
static int encode_jcc(CodeBuffer *b, uint8_t op2, int offset, AssemblyMode mode) {
    if (mode == MODE_16BIT) {
        codebuf_emit_byte(b, 0x0F);
        codebuf_emit_byte(b, op2);
        codebuf_emit_word(b, (uint16_t)offset);
    } else {
        codebuf_emit_byte(b, 0x0F);
        codebuf_emit_byte(b, op2);
        codebuf_emit_dword(b, (uint32_t)offset);
    }
    return 0;
}

int encode_jmp(CodeBuffer *b, int offset, AssemblyMode mode) {
    if (mode == MODE_16BIT) {
        codebuf_emit_byte(b, 0xE9);
        codebuf_emit_word(b, (uint16_t)offset);
    } else {
        codebuf_emit_byte(b, 0xE9);
        codebuf_emit_dword(b, (uint32_t)offset);
    }
    return 0;
}
int encode_call(CodeBuffer *b, int offset, AssemblyMode mode) {
    (void)mode;
    codebuf_emit_byte(b, 0xE8);
    codebuf_emit_dword(b, (uint32_t)offset);
    return 0;
}
int encode_je (CodeBuffer *b, int o, AssemblyMode m) { return encode_jcc(b,0x84,o,m); }
int encode_jne(CodeBuffer *b, int o, AssemblyMode m) { return encode_jcc(b,0x85,o,m); }
int encode_jz (CodeBuffer *b, int o, AssemblyMode m) { return encode_jcc(b,0x84,o,m); }
int encode_jnz(CodeBuffer *b, int o, AssemblyMode m) { return encode_jcc(b,0x85,o,m); }
int encode_jl (CodeBuffer *b, int o, AssemblyMode m) { return encode_jcc(b,0x8C,o,m); }
int encode_jle(CodeBuffer *b, int o, AssemblyMode m) { return encode_jcc(b,0x8E,o,m); }
int encode_jg (CodeBuffer *b, int o, AssemblyMode m) { return encode_jcc(b,0x8F,o,m); }
int encode_jge(CodeBuffer *b, int o, AssemblyMode m) { return encode_jcc(b,0x8D,o,m); }
int encode_ja (CodeBuffer *b, int o, AssemblyMode m) { return encode_jcc(b,0x87,o,m); }
int encode_jae(CodeBuffer *b, int o, AssemblyMode m) { return encode_jcc(b,0x83,o,m); }
int encode_jb (CodeBuffer *b, int o, AssemblyMode m) { return encode_jcc(b,0x82,o,m); }
int encode_jbe(CodeBuffer *b, int o, AssemblyMode m) { return encode_jcc(b,0x86,o,m); }
int encode_js (CodeBuffer *b, int o, AssemblyMode m) { return encode_jcc(b,0x88,o,m); }
int encode_jns(CodeBuffer *b, int o, AssemblyMode m) { return encode_jcc(b,0x89,o,m); }
int encode_jo (CodeBuffer *b, int o, AssemblyMode m) { return encode_jcc(b,0x80,o,m); }
int encode_jno(CodeBuffer *b, int o, AssemblyMode m) { return encode_jcc(b,0x81,o,m); }

int encode_ljmp(CodeBuffer *b, uint16_t seg, uint32_t off, AssemblyMode mode) {
    codebuf_emit_byte(b, 0xEA);
    if (mode == MODE_16BIT) codebuf_emit_word(b, (uint16_t)off);
    else                    codebuf_emit_dword(b, off);
    codebuf_emit_word(b, seg);
    return 0;
}
int encode_lcall(CodeBuffer *b, uint16_t seg, uint32_t off, AssemblyMode mode) {
    codebuf_emit_byte(b, 0x9A);
    if (mode == MODE_16BIT) codebuf_emit_word(b, (uint16_t)off);
    else                    codebuf_emit_dword(b, off);
    codebuf_emit_word(b, seg);
    return 0;
}

/* LEA: load effective address */
int encode_lea(CodeBuffer *b, Operand dst, Operand src, AssemblyMode mode) {
    if (dst.type != OP_REG || src.type != OP_MEM) return -1;
    int sz   = register_size(dst.value.reg);
    int dreg = register_hw(dst.value.reg);
    int W    = (sz == 8) ? 1 : 0;
    emit_opsz_prefix(b, sz, mode);

    if (src.is_rel) {
        /* RIP-relative LEA: mod=00, rm=101, disp32 = label - next */
        emit_rex_if(b, W, (dreg>>3)&1, 0, 0);
        codebuf_emit_byte(b, 0x8D);
        emit_modrm(b, 0, dreg & 7, 5);
        codebuf_emit_label_ref(b, src.rel_label, 0, 0);
        return 0;
    }

    int brex = (src.value.mem.base  != REG_INVALID)
             ? (register_hw(src.value.mem.base)  >> 3) & 1 : 0;
    int xrex = (src.value.mem.index != REG_INVALID)
             ? (register_hw(src.value.mem.index) >> 3) & 1 : 0;
    emit_rex_if(b, W, (dreg>>3)&1, xrex, brex);
    codebuf_emit_byte(b, 0x8D);
    emit_mem_modrm(b, dreg, &src);
    return 0;
}

/* XCHG */
int encode_xchg(CodeBuffer *b, Operand dst, Operand src, AssemblyMode mode) {
    if (dst.type != OP_REG || src.type != OP_REG) return -1;
    int sz   = register_size(dst.value.reg);
    int dreg = register_hw(dst.value.reg);
    int sreg = register_hw(src.value.reg);
    int W    = (sz == 8) ? 1 : 0;
    emit_opsz_prefix(b, sz, mode);
    emit_rex_if(b, W, (sreg>>3)&1, 0, (dreg>>3)&1);
    codebuf_emit_byte(b, 0x87);
    emit_modrm(b, 3, sreg & 7, dreg & 7);
    return 0;
}

/* MOVSX / MOVZX: sign/zero extend smaller source to larger dest */
int encode_movsx(CodeBuffer *b, Operand dst, Operand src, AssemblyMode mode) {
    if (dst.type != OP_REG) return -1;
    int dsz  = register_size(dst.value.reg);
    int ssz  = (src.type == OP_REG) ? register_size(src.value.reg) : (src.size_hint ? (int)src.size_hint : 1);
    int dreg = register_hw(dst.value.reg);
    int W    = (dsz == 8) ? 1 : 0;
    emit_opsz_prefix(b, dsz, mode);

    if (src.type == OP_REG) {
        int sreg = register_hw(src.value.reg);
        emit_rex_if(b, W, (dreg>>3)&1, 0, (sreg>>3)&1);
        if (ssz == 1) { codebuf_emit_byte(b, 0x0F); codebuf_emit_byte(b, 0xBE); }
        else          { codebuf_emit_byte(b, 0x0F); codebuf_emit_byte(b, 0xBF); }
        emit_modrm(b, 3, dreg & 7, sreg & 7);
        return 0;
    }
    if (src.type == OP_MEM) {
        int brex = (src.value.mem.base  != REG_INVALID)
                 ? (register_hw(src.value.mem.base)  >> 3) & 1 : 0;
        int xrex = (src.value.mem.index != REG_INVALID)
                 ? (register_hw(src.value.mem.index) >> 3) & 1 : 0;
        emit_rex_if(b, W, (dreg>>3)&1, xrex, brex);
        if (ssz == 1) { codebuf_emit_byte(b, 0x0F); codebuf_emit_byte(b, 0xBE); }
        else          { codebuf_emit_byte(b, 0x0F); codebuf_emit_byte(b, 0xBF); }
        emit_mem_modrm(b, dreg, &src);
        return 0;
    }
    return -1;
}

int encode_movzx(CodeBuffer *b, Operand dst, Operand src, AssemblyMode mode) {
    if (dst.type != OP_REG) return -1;
    int dsz  = register_size(dst.value.reg);
    int ssz  = (src.type == OP_REG) ? register_size(src.value.reg) : (src.size_hint ? (int)src.size_hint : 1);
    int dreg = register_hw(dst.value.reg);
    int W    = (dsz == 8) ? 1 : 0;
    emit_opsz_prefix(b, dsz, mode);

    if (src.type == OP_REG) {
        int sreg = register_hw(src.value.reg);
        emit_rex_if(b, W, (dreg>>3)&1, 0, (sreg>>3)&1);
        if (ssz == 1) { codebuf_emit_byte(b, 0x0F); codebuf_emit_byte(b, 0xB6); }
        else          { codebuf_emit_byte(b, 0x0F); codebuf_emit_byte(b, 0xB7); }
        emit_modrm(b, 3, dreg & 7, sreg & 7);
        return 0;
    }
    if (src.type == OP_MEM) {
        int brex = (src.value.mem.base  != REG_INVALID)
                 ? (register_hw(src.value.mem.base)  >> 3) & 1 : 0;
        int xrex = (src.value.mem.index != REG_INVALID)
                 ? (register_hw(src.value.mem.index) >> 3) & 1 : 0;
        emit_rex_if(b, W, (dreg>>3)&1, xrex, brex);
        if (ssz == 1) { codebuf_emit_byte(b, 0x0F); codebuf_emit_byte(b, 0xB6); }
        else          { codebuf_emit_byte(b, 0x0F); codebuf_emit_byte(b, 0xB7); }
        emit_mem_modrm(b, dreg, &src);
        return 0;
    }
    (void)ssz; return -1;
}

