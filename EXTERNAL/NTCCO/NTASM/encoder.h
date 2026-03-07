#ifndef ENCODER_H
#define ENCODER_H

#include <stdint.h>

/* Assembly mode */
typedef enum {
    MODE_16BIT = 16,
    MODE_32BIT = 32,
    MODE_64BIT = 64,
} AssemblyMode;

/* Register encoding - low 4 bits = hardware number, bits 4-5 = size class */
typedef enum {
    /* 64-bit */
    REG_RAX = 0,  REG_RCX = 1,  REG_RDX = 2,  REG_RBX = 3,
    REG_RSP = 4,  REG_RBP = 5,  REG_RSI = 6,  REG_RDI = 7,
    REG_R8  = 8,  REG_R9  = 9,  REG_R10 = 10, REG_R11 = 11,
    REG_R12 = 12, REG_R13 = 13, REG_R14 = 14, REG_R15 = 15,
    /* 32-bit (offset 16) */
    REG_EAX = 16, REG_ECX = 17, REG_EDX = 18, REG_EBX = 19,
    REG_ESP = 20, REG_EBP = 21, REG_ESI = 22, REG_EDI = 23,
    REG_R8D = 24, REG_R9D = 25, REG_R10D= 26, REG_R11D= 27,
    REG_R12D= 28, REG_R13D= 29, REG_R14D= 30, REG_R15D= 31,
    /* 16-bit (offset 32) */
    REG_AX  = 32, REG_CX  = 33, REG_DX  = 34, REG_BX  = 35,
    REG_SP  = 36, REG_BP  = 37, REG_SI  = 38, REG_DI  = 39,
    REG_R8W = 40, REG_R9W = 41, REG_R10W= 42, REG_R11W= 43,
    REG_R12W= 44, REG_R13W= 45, REG_R14W= 46, REG_R15W= 47,
    /* 8-bit low (offset 48) */
    REG_AL  = 48, REG_CL  = 49, REG_DL  = 50, REG_BL  = 51,
    REG_SPL = 52, REG_BPL = 53, REG_SIL = 54, REG_DIL = 55,
    REG_R8B = 56, REG_R9B = 57, REG_R10B= 58, REG_R11B= 59,
    REG_R12B= 60, REG_R13B= 61, REG_R14B= 62, REG_R15B= 63,
    /* 8-bit high (offset 64) */
    REG_AH  = 64, REG_CH  = 65, REG_DH  = 66, REG_BH  = 67,
    REG_INVALID = -1,
} RegisterID;

/* Operand types */
typedef enum {
    OP_NONE  = 0,
    OP_REG,         /* register */
    OP_MEM,         /* memory: [base + index*scale + disp] */
    OP_IMM,         /* immediate */
    OP_LABEL,       /* label reference */
} OperandType;

/* Size hint attached to memory operands (byte ptr, word ptr, etc.) */
typedef enum {
    SZ_NONE = 0,
    SZ_BYTE = 1,
    SZ_WORD = 2,
    SZ_DWORD = 4,
    SZ_QWORD = 8,
} SizeHint;

typedef struct {
    OperandType type;
    SizeHint    size_hint; /* for memory operands */
    int         is_rel;    /* RIP-relative label reference */
    char        rel_label[256];
    union {
        RegisterID reg;
        int64_t    imm;
        char       label[256];
        struct {
            RegisterID base;    /* REG_INVALID if none */
            RegisterID index;   /* REG_INVALID if none */
            int        scale;   /* 1, 2, 4, or 8 */
            int64_t    disp;
        } mem;
    } value;
} Operand;

/* Code buffer with relocation support */
typedef struct Reloc {
    int      offset;      /* byte offset in code buffer */
    int      size;        /* 1, 2, or 4 bytes */
    char     label[256];  /* target label */
    int      addend;      /* addend (instruction size to subtract) */
    int      section;     /* 0=text, 1=data */
    struct Reloc *next;
} Reloc;

typedef struct {
    uint8_t *code;
    int      capacity;
    int      size;
    Reloc   *relocs;      /* linked list of pending relocations */
} CodeBuffer;

CodeBuffer *codebuf_create(int capacity);
void        codebuf_free(CodeBuffer *buf);
void        codebuf_emit_byte(CodeBuffer *buf, uint8_t byte);
void        codebuf_emit_bytes(CodeBuffer *buf, const uint8_t *bytes, int n);
void        codebuf_emit_word(CodeBuffer *buf, uint16_t w);
void        codebuf_emit_dword(CodeBuffer *buf, uint32_t d);
void        codebuf_emit_qword(CodeBuffer *buf, uint64_t q);
/* Emit a 4-byte placeholder and record a relocation for label fixup */
void        codebuf_emit_label_ref(CodeBuffer *buf, const char *label,
                                   int addend, int section);

/* Register info */
RegisterID  register_id(const char *name);
int         register_hw(RegisterID id);   /* hardware register number 0-15 */
int         register_size(RegisterID id); /* in bytes: 1, 2, 4, 8 */
int         register_needs_rex(RegisterID id); /* 1 if REX required */

/* Instruction encoders */
int encode_mov(CodeBuffer *buf, Operand dst, Operand src, AssemblyMode mode);
int encode_add(CodeBuffer *buf, Operand dst, Operand src, AssemblyMode mode);
int encode_sub(CodeBuffer *buf, Operand dst, Operand src, AssemblyMode mode);
int encode_and(CodeBuffer *buf, Operand dst, Operand src, AssemblyMode mode);
int encode_or (CodeBuffer *buf, Operand dst, Operand src, AssemblyMode mode);
int encode_xor(CodeBuffer *buf, Operand dst, Operand src, AssemblyMode mode);
int encode_cmp(CodeBuffer *buf, Operand dst, Operand src, AssemblyMode mode);
int encode_test(CodeBuffer *buf, Operand dst, Operand src, AssemblyMode mode);
int encode_imul(CodeBuffer *buf, Operand dst, Operand src, AssemblyMode mode);
int encode_push(CodeBuffer *buf, Operand op, AssemblyMode mode);
int encode_pop (CodeBuffer *buf, Operand op, AssemblyMode mode);
int encode_inc (CodeBuffer *buf, Operand op, AssemblyMode mode);
int encode_dec (CodeBuffer *buf, Operand op, AssemblyMode mode);
int encode_neg (CodeBuffer *buf, Operand op, AssemblyMode mode);
int encode_not (CodeBuffer *buf, Operand op, AssemblyMode mode);
int encode_shl (CodeBuffer *buf, Operand dst, Operand cnt, AssemblyMode mode);
int encode_shr (CodeBuffer *buf, Operand dst, Operand cnt, AssemblyMode mode);
int encode_sar (CodeBuffer *buf, Operand dst, Operand cnt, AssemblyMode mode);
int encode_rol (CodeBuffer *buf, Operand dst, Operand cnt, AssemblyMode mode);
int encode_ror (CodeBuffer *buf, Operand dst, Operand cnt, AssemblyMode mode);
int encode_ret (CodeBuffer *buf, AssemblyMode mode);
int encode_nop (CodeBuffer *buf, AssemblyMode mode);
int encode_syscall(CodeBuffer *buf, AssemblyMode mode);
int encode_int (CodeBuffer *buf, uint8_t vec, AssemblyMode mode);
int encode_hlt (CodeBuffer *buf, AssemblyMode mode);
int encode_cli (CodeBuffer *buf, AssemblyMode mode);
int encode_sti (CodeBuffer *buf, AssemblyMode mode);
int encode_call(CodeBuffer *buf, int offset, AssemblyMode mode);
int encode_jmp (CodeBuffer *buf, int offset, AssemblyMode mode);
int encode_je  (CodeBuffer *buf, int offset, AssemblyMode mode);
int encode_jne (CodeBuffer *buf, int offset, AssemblyMode mode);
int encode_jl  (CodeBuffer *buf, int offset, AssemblyMode mode);
int encode_jle (CodeBuffer *buf, int offset, AssemblyMode mode);
int encode_jg  (CodeBuffer *buf, int offset, AssemblyMode mode);
int encode_jge (CodeBuffer *buf, int offset, AssemblyMode mode);
int encode_ja  (CodeBuffer *buf, int offset, AssemblyMode mode);
int encode_jae (CodeBuffer *buf, int offset, AssemblyMode mode);
int encode_jb  (CodeBuffer *buf, int offset, AssemblyMode mode);
int encode_jbe (CodeBuffer *buf, int offset, AssemblyMode mode);
int encode_js  (CodeBuffer *buf, int offset, AssemblyMode mode);
int encode_jns (CodeBuffer *buf, int offset, AssemblyMode mode);
int encode_jo  (CodeBuffer *buf, int offset, AssemblyMode mode);
int encode_jno (CodeBuffer *buf, int offset, AssemblyMode mode);
int encode_jz  (CodeBuffer *buf, int offset, AssemblyMode mode);
int encode_jnz (CodeBuffer *buf, int offset, AssemblyMode mode);
int encode_ljmp (CodeBuffer *buf, uint16_t seg, uint32_t off, AssemblyMode mode);
int encode_lcall(CodeBuffer *buf, uint16_t seg, uint32_t off, AssemblyMode mode);
int encode_movsx(CodeBuffer *buf, Operand dst, Operand src, AssemblyMode mode);
int encode_movzx(CodeBuffer *buf, Operand dst, Operand src, AssemblyMode mode);
int encode_lea  (CodeBuffer *buf, Operand dst, Operand src, AssemblyMode mode);
int encode_xchg (CodeBuffer *buf, Operand dst, Operand src, AssemblyMode mode);

#endif /* ENCODER_H */
