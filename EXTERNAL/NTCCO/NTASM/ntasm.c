/* ntasm.c - NTASM x86/x86-64 Assembler
 * Two-pass assembler with ELF64 output, sections, and full label resolution.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "lexer.h"
#include "encoder.h"
#include "symbols.h"

/* =========================================================
 * Section buffers
 * ========================================================= */
typedef struct {
    uint8_t *data;
    int      size;
    int      capacity;
} SecBuf;

static void secbuf_init(SecBuf *s, int cap) {
    s->data     = malloc(cap);
    s->size     = 0;
    s->capacity = cap;
}

static void secbuf_emit(SecBuf *s, uint8_t b) {
    if (s->size >= s->capacity) {
        s->capacity *= 2;
        s->data = realloc(s->data, s->capacity);
    }
    s->data[s->size++] = b;
}

static void secbuf_emit_bytes(SecBuf *s, const uint8_t *p, int n) {
    for (int i = 0; i < n; i++) secbuf_emit(s, p[i]);
}




/* =========================================================
 * Pending jump/call relocations (for second-pass backpatch)
 * ========================================================= */
typedef struct JmpReloc {
    int    patch_offset;  /* offset in text section of the 4-byte placeholder */
    int    instr_end;     /* offset of byte after the full instruction */
    char   label[256];
    struct JmpReloc *next;
} JmpReloc;

/* =========================================================
 * Assembler state
 * ========================================================= */
typedef struct {
    Lexer        *lexer;
    Token         tok;
    AssemblyMode  mode;
    int           cur_section; /* 0=text, 1=data, 2=bss */

    SecBuf        text;
    SecBuf        data;
    int           bss_size;

    SymbolTable  *syms;
    JmpReloc     *jmp_relocs;

    int           output_elf;  /* 1 = ELF64, 0 = flat binary */
    char          out_path[256];

    int           pass;        /* 1 or 2 */
    int           errors;
} Asm;

/* =========================================================
 * Token helpers
 * ========================================================= */
static void next(Asm *a) {
    a->tok = lexer_next_token(a->lexer);
    /* skip blank newlines between statements */
}

static void skip_line(Asm *a) {
    while (a->tok.type != TOK_NEWLINE && a->tok.type != TOK_EOF)
        next(a);
}

static int cur_offset(Asm *a) {
    return (a->cur_section == 0) ? a->text.size : a->data.size;
}

static void emit_byte(Asm *a, uint8_t b) {
    if (a->cur_section == 0) secbuf_emit(&a->text, b);
    else                     secbuf_emit(&a->data, b);
}

static void emit_bytes(Asm *a, const uint8_t *p, int n) {
    for (int i = 0; i < n; i++) emit_byte(a, p[i]);
}

/* Relay CodeBuffer emissions into the current section */
static void flush_codebuf(Asm *a, CodeBuffer *cb) {
    emit_bytes(a, cb->code, cb->size);
    /* Transfer any label relocs recorded by the encoder (pass 2 only) */
    if (a->pass == 2) {
        Reloc *r = cb->relocs;
        while (r) {
            /* Convert encoder-relative offset to section-absolute */
            JmpReloc *jr = malloc(sizeof(JmpReloc));
            jr->patch_offset = a->text.size - cb->size + r->offset;
            jr->instr_end    = jr->patch_offset + r->size;
            strncpy(jr->label, r->label, 255);
            jr->label[255] = '\0';
            jr->next = a->jmp_relocs;
            a->jmp_relocs = jr;
            r = r->next;
        }
    }
    cb->relocs = NULL;
    cb->size   = 0;
}

static void err(Asm *a, const char *msg) {
    fprintf(stderr, "Error (line %d): %s\n", a->tok.line, msg);
    a->errors++;
}

/* =========================================================
 * Number parsing helper
 * ========================================================= */
static int64_t parse_number(const char *s) {
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        return (int64_t)strtoull(s, NULL, 16);
    if (s[0] == '0' && s[1] != '\0')
        return (int64_t)strtoull(s, NULL, 8);
    return strtoll(s, NULL, 10);
}

/* =========================================================
 * Operand parser
 *   Handles: reg, imm, $imm, %reg, [base+index*scale+disp]
 *   Size hints: BYTE PTR, WORD PTR, DWORD PTR, QWORD PTR
 * ========================================================= */
static Operand parse_operand(Asm *a) {
    Operand op;
    memset(&op, 0, sizeof(op));
    op.is_rel = 0;
    op.rel_label[0] = '\0';
    op.value.mem.base  = REG_INVALID;
    op.value.mem.index = REG_INVALID;
    op.value.mem.scale = 1;

    /* Size hint: byte/word/dword/qword */
    if (a->tok.type == TOK_IDENTIFIER) {
        SizeHint sh = SZ_NONE;
        if      (strcasecmp(a->tok.value, "byte")  == 0) sh = SZ_BYTE;
        else if (strcasecmp(a->tok.value, "word")  == 0) sh = SZ_WORD;
        else if (strcasecmp(a->tok.value, "dword") == 0) sh = SZ_DWORD;
        else if (strcasecmp(a->tok.value, "qword") == 0) sh = SZ_QWORD;
        if (sh != SZ_NONE) {
            op.size_hint = sh;
            next(a); /* consume byte/word/etc */
            /* optional "ptr" keyword */
            if (a->tok.type == TOK_IDENTIFIER &&
                strcasecmp(a->tok.value, "ptr") == 0)
                next(a);
        }
    }

    /* AT&T %register */
    if (a->tok.type == TOK_PERCENT) {
        next(a);
        if (a->tok.type != TOK_REGISTER) { err(a, "expected register after %"); return op; }
        op.type       = OP_REG;
        op.value.reg  = register_id(a->tok.value);
        next(a);
        return op;
    }

    /* AT&T $immediate */
    if (a->tok.type == TOK_DOLLAR) {
        next(a);
        int neg = 0;
        if (a->tok.type == TOK_MINUS) { neg = 1; next(a); }
        if (a->tok.type == TOK_NUMBER) {
            op.type      = OP_IMM;
            op.value.imm = parse_number(a->tok.value) * (neg ? -1 : 1);
            next(a);
            return op;
        }
        if (a->tok.type == TOK_IDENTIFIER) {
            /* $label — address-of */
            op.type = OP_LABEL;
            strncpy(op.value.label, a->tok.value, 255);
            next(a);
            return op;
        }
        err(a, "expected number or label after $"); return op;
    }

    /* Intel register (no prefix) */
    if (a->tok.type == TOK_REGISTER) {
        op.type      = OP_REG;
        op.value.reg = register_id(a->tok.value);
        next(a);
        return op;
    }

    /* Intel immediate (plain number) */
    if (a->tok.type == TOK_NUMBER) {
        op.type      = OP_IMM;
        op.value.imm = parse_number(a->tok.value);
        next(a);
        return op;
    }

    /* Negative immediate */
    if (a->tok.type == TOK_MINUS) {
        next(a);
        if (a->tok.type == TOK_NUMBER) {
            op.type      = OP_IMM;
            op.value.imm = -parse_number(a->tok.value);
            next(a);
            return op;
        }
        err(a, "expected number after '-'"); return op;
    }

    /* Memory: [base + index*scale + disp] */
    if (a->tok.type == TOK_LBRACKET) {
        next(a);
        op.type = OP_MEM;

        while (a->tok.type != TOK_RBRACKET && a->tok.type != TOK_EOF) {
            if (a->tok.type == TOK_IDENTIFIER && strcasecmp(a->tok.value, "rel") == 0) {
                next(a); /* consume 'rel' */
                if (a->tok.type == TOK_IDENTIFIER || a->tok.type == TOK_DIRECTIVE) {
                    op.is_rel = 1;
                    strncpy(op.rel_label, a->tok.value, 255);
                    op.rel_label[255] = '\0';
                    next(a);
                }
                continue;
            }
            if (a->tok.type == TOK_PLUS || a->tok.type == TOK_MINUS) {
                int neg2 = (a->tok.type == TOK_MINUS);
                next(a);
                if (a->tok.type == TOK_NUMBER) {
                    op.value.mem.disp += (neg2 ? -1 : 1) * parse_number(a->tok.value);
                    next(a);
                } else if (a->tok.type == TOK_REGISTER) {
                    /* +reg after base — treat as index*1 */
                    RegisterID r = register_id(a->tok.value);
                    next(a);
                    if (a->tok.type == TOK_STAR) {
                        next(a);
                        int sc = (int)parse_number(a->tok.value);
                        next(a);
                        op.value.mem.index = r;
                        op.value.mem.scale = sc;
                    } else {
                        op.value.mem.index = r;
                        op.value.mem.scale = 1;
                    }
                }
                continue;
            }
            if (a->tok.type == TOK_REGISTER) {
                RegisterID r = register_id(a->tok.value);
                next(a);
                if (a->tok.type == TOK_STAR) {
                    /* index*scale with no base yet */
                    next(a);
                    int sc = (int)parse_number(a->tok.value);
                    next(a);
                    op.value.mem.index = r;
                    op.value.mem.scale = sc;
                } else {
                    if (op.value.mem.base == REG_INVALID)
                        op.value.mem.base = r;
                    else {
                        op.value.mem.index = r;
                        op.value.mem.scale = 1;
                    }
                }
                continue;
            }
            if (a->tok.type == TOK_NUMBER) {
                op.value.mem.disp += parse_number(a->tok.value);
                next(a);
                continue;
            }
            if (a->tok.type == TOK_IDENTIFIER) {
                /* Symbol as displacement or const */
                Symbol *sym = symtab_find(a->syms, a->tok.value);
                if (sym && sym->defined) {
                    if (sym->type == SYM_CONST)
                        op.value.mem.disp += (int64_t)sym->value;
                    else
                        op.value.mem.disp += (int64_t)sym->value;
                }
                next(a);
                continue;
            }
            next(a); /* skip unknown tokens inside [] */
        }
        if (a->tok.type == TOK_RBRACKET) next(a);
        return op;
    }

    /* Label reference that starts with '.' (lexed as TOK_DIRECTIVE) */
    if (a->tok.type == TOK_DIRECTIVE) {
        op.type = OP_LABEL;
        strncpy(op.value.label, a->tok.value, 255);
        next(a);
        return op;
    }

    /* Bare identifier — label reference */
    if (a->tok.type == TOK_IDENTIFIER) {
        Symbol *s = symtab_find(a->syms, a->tok.value);
        if (s && s->defined && s->type == SYM_CONST) {
            op.type = OP_IMM;
            op.value.imm = (int64_t)s->value;
            next(a);
            return op;
        }
        op.type = OP_LABEL;
        strncpy(op.value.label, a->tok.value, 255);
        next(a);
        return op;
    }

    return op; /* OP_NONE */
}

/* =========================================================
 * Jump/call with label: emit placeholder and record reloc
 * ========================================================= */
static void emit_jmp_label(Asm *a, const char *label,
                           int (*enc)(CodeBuffer*, int, AssemblyMode),
                           int instr_size) {
    Symbol *sym = symtab_find(a->syms, label);
    if (a->pass == 2 && sym && sym->defined && sym->section == 0) {
        /* Pass 2: known label in text section — compute relative offset directly */
        int target   = (int)sym->value;
        int here     = a->text.size;
        int rel      = target - (here + instr_size);
        CodeBuffer *cb = codebuf_create(16);
        enc(cb, rel, a->mode);
        flush_codebuf(a, cb);
        codebuf_free(cb);
    } else if (a->pass == 2) {
        /* Pass 2: Forward/external reference — emit placeholder and record */
        JmpReloc *jr = malloc(sizeof(JmpReloc));
        strncpy(jr->label, label, 255);
        jr->label[255] = '\0';

        /* Emit the instruction with offset=0 to get correct byte size */
        CodeBuffer *cb = codebuf_create(16);
        enc(cb, 0, a->mode);
        /* The reloc patch point is the last 4 bytes of the instruction */
        jr->patch_offset = a->text.size + cb->size - 4;
        jr->instr_end    = a->text.size + cb->size;
        jr->next         = a->jmp_relocs;
        a->jmp_relocs    = jr;
        flush_codebuf(a, cb);
        codebuf_free(cb);
    } else {
        /* Pass 1: just emit to track size, no reloc recording */
        CodeBuffer *cb = codebuf_create(16);
        enc(cb, 0, a->mode);
        flush_codebuf(a, cb);
        codebuf_free(cb);
    }
}

/* =========================================================
 * Directive handler
 * ========================================================= */
static void handle_directive_by_name(Asm *a, const char *dir) {
    /* dir has already been consumed from the token stream */
    if (strcasecmp(dir, ".text") == 0 || strcasecmp(dir, ".code") == 0) {
        a->cur_section = 0;
    } else if (strcasecmp(dir, ".equ") == 0 || strcasecmp(dir, "equ") == 0) {
        if (a->tok.type == TOK_IDENTIFIER) {
            char name[256];
            strncpy(name, a->tok.value, 255); name[255] = '\0';
            next(a);
            if (a->tok.type == TOK_COMMA) next(a);
            int64_t val = 0;
            if (a->tok.type == TOK_NUMBER) { val = parse_number(a->tok.value); next(a); }
            else if (a->tok.type == TOK_IDENTIFIER) {
                Symbol *s = symtab_find(a->syms, a->tok.value);
                if (s && s->defined) val = s->value;
                next(a);
            }
            Symbol *s = symtab_add(a->syms, name, SYM_CONST, (uint64_t)val, -1);
            s->defined = 1;
        }
    } else if (strcasecmp(dir, ".data") == 0) {
        a->cur_section = 1;
    } else if (strcasecmp(dir, ".bss") == 0) {
        a->cur_section = 2;
    } else if (strcasecmp(dir, ".global") == 0 ||
               strcasecmp(dir, ".globl")  == 0) {
        if (a->tok.type == TOK_IDENTIFIER) {
            symtab_mark_global(a->syms, a->tok.value);
            next(a);
        }
    } else if (strcasecmp(dir, ".extern") == 0 ||
               strcasecmp(dir, ".extrn")  == 0) {
        if (a->tok.type == TOK_IDENTIFIER) {
            symtab_mark_extern(a->syms, a->tok.value);
            next(a);
        }
    } else if (strcasecmp(dir, ".db") == 0 ||
               strcasecmp(dir, "db")  == 0) {
        /* Byte data: db 0x90, 0x91, "string", ... */
        while (1) {
            if (a->tok.type == TOK_NUMBER) {
                if (a->pass == 2) emit_byte(a, (uint8_t)parse_number(a->tok.value));
                next(a);
            } else if (a->tok.type == TOK_STRING) {
                if (a->pass == 2)
                    for (int i = 0; a->tok.value[i]; i++)
                        emit_byte(a, (uint8_t)a->tok.value[i]);
                next(a);
            } else break;
            if (a->tok.type == TOK_COMMA) next(a); else break;
        }
    } else if (strcasecmp(dir, ".dw") == 0 || strcasecmp(dir, "dw") == 0) {
        while (1) {
            if (a->tok.type == TOK_NUMBER) {
                uint16_t v = (uint16_t)parse_number(a->tok.value);
                if (a->pass == 2) { emit_byte(a, v & 0xff); emit_byte(a, v >> 8); }
                next(a);
            } else break;
            if (a->tok.type == TOK_COMMA) next(a); else break;
        }
    } else if (strcasecmp(dir, ".dd") == 0 || strcasecmp(dir, "dd") == 0) {
        while (1) {
            if (a->tok.type == TOK_NUMBER) {
                uint32_t v = (uint32_t)parse_number(a->tok.value);
                if (a->pass == 2) {
                    emit_byte(a, v & 0xff); emit_byte(a, (v>>8) & 0xff);
                    emit_byte(a, (v>>16) & 0xff); emit_byte(a, (v>>24) & 0xff);
                }
                next(a);
            } else break;
            if (a->tok.type == TOK_COMMA) next(a); else break;
        }
    } else if (strcasecmp(dir, ".dq") == 0 || strcasecmp(dir, "dq") == 0) {
        while (1) {
            if (a->tok.type == TOK_NUMBER) {
                uint64_t v = (uint64_t)parse_number(a->tok.value);
                if (a->pass == 2) {
                    for (int i = 0; i < 8; i++)
                        emit_byte(a, (v >> (i*8)) & 0xff);
                }
                next(a);
            } else break;
            if (a->tok.type == TOK_COMMA) next(a); else break;
        }
    } else if (strcasecmp(dir, ".resb") == 0 || strcasecmp(dir, "resb") == 0) {
        if (a->tok.type == TOK_NUMBER) {
            int n = (int)parse_number(a->tok.value);
            if (a->cur_section == 2) a->bss_size += n;
            else if (a->pass == 2)
                for (int i = 0; i < n; i++) emit_byte(a, 0);
            next(a);
        }
    } else if (strcasecmp(dir, ".align") == 0) {
        if (a->tok.type == TOK_NUMBER) {
            int align = (int)parse_number(a->tok.value);
            int off   = cur_offset(a);
            int pad   = (align - (off % align)) % align;
            if (a->pass == 2)
                for (int i = 0; i < pad; i++) emit_byte(a, 0x90);
            next(a);
        }
    } else {
        /* Unknown directive: skip rest of line */
        skip_line(a);
    }
}

static void handle_directive(Asm *a) {
    char dir[64];
    strncpy(dir, a->tok.value, 63); dir[63] = '\0';
    next(a); /* consume directive token */
    handle_directive_by_name(a, dir);
}

/* =========================================================
 * Instruction dispatch
 * ========================================================= */
static void handle_instruction(Asm *a) {
    char mnem[64];
    strncpy(mnem, a->tok.value, 63); mnem[63] = '\0';
    next(a); /* consume mnemonic */

    /* Parse up to 3 operands */
    Operand ops[3];
    int n = 0;
    memset(ops, 0, sizeof(ops));
    for (int i = 0; i < 3; i++) {
        ops[i].is_rel = 0;
        ops[i].rel_label[0] = '\0';
        ops[i].value.mem.base  = REG_INVALID;
        ops[i].value.mem.index = REG_INVALID;
        ops[i].value.mem.scale = 1;
    }

    if (a->tok.type != TOK_NEWLINE && a->tok.type != TOK_EOF) {
        ops[n++] = parse_operand(a);
        while (a->tok.type == TOK_COMMA && n < 3) {
            next(a);
            ops[n++] = parse_operand(a);
        }
    }

    /* AT&T syntax operand swap: two-operand instructions have src,dst order
     * (opposite of Intel).  Detect by checking if first operand was a $imm
     * or %reg and the instruction is not a jump/call.  We track this via the
     * first token being TOK_DOLLAR or TOK_PERCENT during parse_operand.
     * Simpler heuristic: if ops[0] is IMM/LABEL and ops[1] is REG/MEM,
     * the user likely wrote AT&T "op $src, %dst" — swap them so the encoder
     * always sees (dst, src) Intel order.                                    */
    if (n == 2) {
        int is_jmp_like =
            strcasecmp(mnem,"jmp")==0 || strcasecmp(mnem,"call")==0 ||
            strncasecmp(mnem,"j",1)==0;
        int at_and_t_order =
            !is_jmp_like &&
            (ops[0].type == OP_IMM || ops[0].type == OP_LABEL) &&
            (ops[1].type == OP_REG || ops[1].type == OP_MEM);
        if (!at_and_t_order && !is_jmp_like) {
            /* Also swap %reg, %reg when both have registers but src appears
             * first (AT&T: add %rcx, %rax means rax += rcx).
             * We can't distinguish AT&T from Intel for reg,reg reliably here,
             * so we rely on the $ prefix heuristic above only.             */
        }
        if (at_and_t_order) {
            Operand tmp = ops[0]; ops[0] = ops[1]; ops[1] = tmp;
        }
    }

    CodeBuffer *cb = codebuf_create(32);
    int rc = 0;

    /* Jump/call instructions with label operand handled specially.
     * Must check BEFORE label->imm resolution so OP_LABEL is still set. */
#define JLABEL(fn, sz) \
    if (n == 1 && ops[0].type == OP_LABEL) { \
        emit_jmp_label(a, ops[0].value.label, fn, sz); \
        codebuf_free(cb); return; \
    }

    if      (strcasecmp(mnem, "jmp") == 0) { JLABEL(encode_jmp,  5); rc = encode_jmp(cb,  ops[0].value.imm, a->mode); }
    else if (strcasecmp(mnem, "call") == 0){ JLABEL(encode_call, 5); rc = encode_call(cb, ops[0].value.imm, a->mode); }
    else if (strcasecmp(mnem, "je")   == 0) { JLABEL(encode_je,   6); rc = encode_je(cb,   0, a->mode); }
    else if (strcasecmp(mnem, "jne")  == 0) { JLABEL(encode_jne,  6); rc = encode_jne(cb,  0, a->mode); }
    else if (strcasecmp(mnem, "jz")   == 0) { JLABEL(encode_jz,   6); rc = encode_jz(cb,   0, a->mode); }
    else if (strcasecmp(mnem, "jnz")  == 0) { JLABEL(encode_jnz,  6); rc = encode_jnz(cb,  0, a->mode); }
    else if (strcasecmp(mnem, "jl")   == 0) { JLABEL(encode_jl,   6); rc = encode_jl(cb,   0, a->mode); }
    else if (strcasecmp(mnem, "jle")  == 0) { JLABEL(encode_jle,  6); rc = encode_jle(cb,  0, a->mode); }
    else if (strcasecmp(mnem, "jg")   == 0) { JLABEL(encode_jg,   6); rc = encode_jg(cb,   0, a->mode); }
    else if (strcasecmp(mnem, "jge")  == 0) { JLABEL(encode_jge,  6); rc = encode_jge(cb,  0, a->mode); }
    else if (strcasecmp(mnem, "ja")   == 0) { JLABEL(encode_ja,   6); rc = encode_ja(cb,   0, a->mode); }
    else if (strcasecmp(mnem, "jae")  == 0) { JLABEL(encode_jae,  6); rc = encode_jae(cb,  0, a->mode); }
    else if (strcasecmp(mnem, "jb")   == 0) { JLABEL(encode_jb,   6); rc = encode_jb(cb,   0, a->mode); }
    else if (strcasecmp(mnem, "jbe")  == 0) { JLABEL(encode_jbe,  6); rc = encode_jbe(cb,  0, a->mode); }
    else if (strcasecmp(mnem, "js")   == 0) { JLABEL(encode_js,   6); rc = encode_js(cb,   0, a->mode); }
    else if (strcasecmp(mnem, "jns")  == 0) { JLABEL(encode_jns,  6); rc = encode_jns(cb,  0, a->mode); }
    else if (strcasecmp(mnem, "jo")   == 0) { JLABEL(encode_jo,   6); rc = encode_jo(cb,   0, a->mode); }
    else if (strcasecmp(mnem, "jno")  == 0) { JLABEL(encode_jno,  6); rc = encode_jno(cb,  0, a->mode); }
    else {
        /* Non-jump instruction: resolve any OP_LABEL operands to OP_IMM.
         * In pass 1, unresolved labels resolve to 0 so the encoder always
         * produces the correct byte count for offset tracking.             */
        for (int i = 0; i < n; i++) {
            if (ops[i].type == OP_LABEL) {
                Symbol *sym2 = symtab_find(a->syms, ops[i].value.label);
                ops[i].type      = OP_IMM;
                ops[i].value.imm = (sym2 && sym2->defined)
                                   ? (int64_t)sym2->value : 0;
            }
        }
        if      (strcasecmp(mnem, "mov")   == 0) rc = encode_mov(cb,   ops[0], ops[1], a->mode);
        else if (strcasecmp(mnem, "movsx") == 0) rc = encode_movsx(cb, ops[0], ops[1], a->mode);
        else if (strcasecmp(mnem, "movzx") == 0) rc = encode_movzx(cb, ops[0], ops[1], a->mode);
        else if (strcasecmp(mnem, "lea")   == 0) rc = encode_lea(cb,   ops[0], ops[1], a->mode);
        else if (strcasecmp(mnem, "xchg")  == 0) rc = encode_xchg(cb,  ops[0], ops[1], a->mode);
        else if (strcasecmp(mnem, "add")   == 0) rc = encode_add(cb,   ops[0], ops[1], a->mode);
        else if (strcasecmp(mnem, "sub")   == 0) rc = encode_sub(cb,   ops[0], ops[1], a->mode);
        else if (strcasecmp(mnem, "and")   == 0) rc = encode_and(cb,   ops[0], ops[1], a->mode);
        else if (strcasecmp(mnem, "or")    == 0) rc = encode_or(cb,    ops[0], ops[1], a->mode);
        else if (strcasecmp(mnem, "xor")   == 0) rc = encode_xor(cb,   ops[0], ops[1], a->mode);
        else if (strcasecmp(mnem, "cmp")   == 0) rc = encode_cmp(cb,   ops[0], ops[1], a->mode);
        else if (strcasecmp(mnem, "test")  == 0) rc = encode_test(cb,  ops[0], ops[1], a->mode);
        else if (strcasecmp(mnem, "imul")  == 0) rc = encode_imul(cb,  ops[0], ops[1], a->mode);
        else if (strcasecmp(mnem, "inc")   == 0) rc = encode_inc(cb,   ops[0], a->mode);
        else if (strcasecmp(mnem, "dec")   == 0) rc = encode_dec(cb,   ops[0], a->mode);
        else if (strcasecmp(mnem, "neg")   == 0) rc = encode_neg(cb,   ops[0], a->mode);
        else if (strcasecmp(mnem, "not")   == 0) rc = encode_not(cb,   ops[0], a->mode);
        else if (strcasecmp(mnem, "shl")   == 0 ||
                 strcasecmp(mnem, "sal")   == 0) rc = encode_shl(cb,   ops[0], ops[1], a->mode);
        else if (strcasecmp(mnem, "shr")   == 0) rc = encode_shr(cb,   ops[0], ops[1], a->mode);
        else if (strcasecmp(mnem, "sar")   == 0) rc = encode_sar(cb,   ops[0], ops[1], a->mode);
        else if (strcasecmp(mnem, "rol")   == 0) rc = encode_rol(cb,   ops[0], ops[1], a->mode);
        else if (strcasecmp(mnem, "ror")   == 0) rc = encode_ror(cb,   ops[0], ops[1], a->mode);
        else if (strcasecmp(mnem, "push")  == 0) rc = encode_push(cb,  ops[0], a->mode);
        else if (strcasecmp(mnem, "pop")   == 0) rc = encode_pop(cb,   ops[0], a->mode);
        else if (strcasecmp(mnem, "ret")   == 0) rc = encode_ret(cb,   a->mode);
        else if (strcasecmp(mnem, "nop")   == 0) rc = encode_nop(cb,   a->mode);
        else if (strcasecmp(mnem, "hlt")   == 0) rc = encode_hlt(cb,   a->mode);
        else if (strcasecmp(mnem, "cli")   == 0) rc = encode_cli(cb,   a->mode);
        else if (strcasecmp(mnem, "sti")   == 0) rc = encode_sti(cb,   a->mode);
        else if (strcasecmp(mnem, "syscall")== 0) rc = encode_syscall(cb, a->mode);
        else if (strcasecmp(mnem, "int")   == 0 && n >= 1 && ops[0].type == OP_IMM)
                                                 rc = encode_int(cb, (uint8_t)ops[0].value.imm, a->mode);
        else if (strcasecmp(mnem, "ljmp")  == 0 && n == 2)
                                                 rc = encode_ljmp(cb,  (uint16_t)ops[0].value.imm,
                                                                  (uint32_t)ops[1].value.imm, a->mode);
        else if (strcasecmp(mnem, "lcall") == 0 && n == 2)
                                                 rc = encode_lcall(cb, (uint16_t)ops[0].value.imm,
                                                                  (uint32_t)ops[1].value.imm, a->mode);
        else {
            fprintf(stderr, "Line %d: unknown instruction '%s'\n", a->tok.line, mnem);
            a->errors++;
            codebuf_free(cb);
            return;
        }
    }

    if (rc != 0 && a->pass == 2) {
        fprintf(stderr, "Line %d: cannot encode '%s' with given operands\n",
                a->tok.line, mnem);
        a->errors++;
    }
    flush_codebuf(a, cb);
    codebuf_free(cb);
#undef JLABEL
}

/* =========================================================
 * Single-pass scan: processes one statement at current token
 * ========================================================= */
static void scan_statement(Asm *a) {
    /* Skip blank lines */
    while (a->tok.type == TOK_NEWLINE) { next(a); }

    if (a->tok.type == TOK_EOF) return;

    /* bits N — mode switch */
    if (a->tok.type == TOK_BITS) {
        next(a);
        if (a->tok.type == TOK_NUMBER) {
            int b = (int)parse_number(a->tok.value);
            if      (b == 16) a->mode = MODE_16BIT;
            else if (b == 32) a->mode = MODE_32BIT;
            else              a->mode = MODE_64BIT;
            next(a);
        }
        return;
    }

    /* Directive or local label starting with '.' */
    if (a->tok.type == TOK_PERCENT) {
        next(a);
        if (a->tok.type == TOK_IDENTIFIER) {
            if (strcasecmp(a->tok.value, "define") == 0) {
                next(a);
                if (a->tok.type == TOK_IDENTIFIER) {
                    char name[256];
                    strncpy(name, a->tok.value, 255); name[255] = '\0';
                    next(a);
                    int64_t val = 0;
                    if (a->tok.type == TOK_NUMBER)
                        val = parse_number(a->tok.value), next(a);
                    else if (a->tok.type == TOK_IDENTIFIER) {
                        Symbol *s = symtab_find(a->syms, a->tok.value);
                        if (s && s->defined) val = s->value;
                        next(a);
                    }
                    Symbol *s = symtab_add(a->syms, name, SYM_CONST, (uint64_t)val, -1);
                    s->defined = 1;
                }
                return;
            }
            if (strcasecmp(a->tok.value, "undef") == 0) {
                next(a);
                if (a->tok.type == TOK_IDENTIFIER) {
                    symtab_remove(a->syms, a->tok.value);
                    next(a);
                }
                return;
            }
        }
    }

    if (a->tok.type == TOK_DIRECTIVE) {
        char dname[256];
        strncpy(dname, a->tok.value, 255); dname[255] = '\0';
        next(a); /* consume directive/potential-label token */
        if (a->tok.type == TOK_COLON) {
            /* It's a local label like .foo: */
            next(a);
            if (a->pass == 1) {
                symtab_add(a->syms, dname, SYM_LABEL,
                           (uint64_t)cur_offset(a), a->cur_section);
            }
            if (a->tok.type == TOK_INSTRUCTION)
                handle_instruction(a);
            return;
        }
        handle_directive_by_name(a, dname);
        return;
    }

    /* db/dw/dd/dq/resb as bare keywords (NASM style without dot) */
    if (a->tok.type == TOK_IDENTIFIER) {
        const char *v = a->tok.value;
        if (strcasecmp(v,"db")==0 || strcasecmp(v,"dw")==0 ||
            strcasecmp(v,"dd")==0 || strcasecmp(v,"dq")==0 ||
            strcasecmp(v,"resb")==0) {
            handle_directive(a);
            return;
        }
    }

    /* Label definition: identifier followed by colon */
    if (a->tok.type == TOK_IDENTIFIER) {
        char name[256];
        strncpy(name, a->tok.value, 255); name[255] = '\0';
        Token saved = a->tok;
        next(a);
        if (a->tok.type == TOK_COLON) {
            next(a); /* consume colon */
            /* Only define labels in pass 1 */
            if (a->pass == 1) {
                symtab_add(a->syms, name, SYM_LABEL,
                           (uint64_t)cur_offset(a), a->cur_section);
            }
            /* Continue to parse rest of line (instruction on same line) */
            if (a->tok.type == TOK_INSTRUCTION)
                handle_instruction(a);
            return;
        }
        /* Not a label — was it an instruction? (identifier that the lexer
         * classified wrong; shouldn't happen but handle gracefully) */
        /* Restore: we can't un-advance the lexer, so just skip */
        (void)saved;
        return;
    }

    /* Instruction */
    if (a->tok.type == TOK_INSTRUCTION) {
        handle_instruction(a);
        return;
    }

    /* Anything else on a line: skip */
    next(a);
}

/* =========================================================
 * Backpatch all recorded jump relocations
 * ========================================================= */
static void backpatch(Asm *a) {
    JmpReloc *jr = a->jmp_relocs;
    while (jr) {
        Symbol *sym = symtab_find(a->syms, jr->label);
        if (!sym || !sym->defined) {
            fprintf(stderr, "Error: undefined label '%s'\n", jr->label);
            a->errors++;
        } else {
            int32_t rel = (int32_t)((int)sym->value - jr->instr_end);
            uint8_t *p  = a->text.data + jr->patch_offset;
            p[0] = (uint8_t)(rel & 0xff);
            p[1] = (uint8_t)((rel >> 8) & 0xff);
            p[2] = (uint8_t)((rel >> 16) & 0xff);
            p[3] = (uint8_t)((rel >> 24) & 0xff);
        }
        jr = jr->next;
    }
}

/* =========================================================
 * ELF64 output
 * ========================================================= */
#define ET_REL  1
#define EM_X86_64 62
#define SHT_NULL     0
#define SHT_PROGBITS 1
#define SHT_SYMTAB   2
#define SHT_STRTAB   3
#define SHT_NOBITS   8
#define SHF_ALLOC    0x2
#define SHF_EXECINSTR 0x4
#define SHF_WRITE    0x1
#define STB_LOCAL  0
#define STB_GLOBAL 1
#define STT_NOTYPE 0
#define STT_FUNC   2
#define STT_SECTION 3
#define STV_DEFAULT 0

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff, e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum;
    uint16_t e_shentsize, e_shnum, e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    uint32_t sh_name, sh_type;
    uint64_t sh_flags, sh_addr, sh_offset, sh_size;
    uint32_t sh_link, sh_info;
    uint64_t sh_addralign, sh_entsize;
} Elf64_Shdr;

typedef struct {
    uint32_t st_name;
    uint8_t  st_info, st_other;
    uint16_t st_shndx;
    uint64_t st_value, st_size;
} Elf64_Sym;


static void write_elf64(Asm *a, FILE *f) {
    /* Build string tables */
    SecBuf shstrtab, strtab;
    secbuf_init(&shstrtab, 64);
    secbuf_init(&strtab, 64);

    /* shstrtab: section name strings */
    secbuf_emit(&shstrtab, 0); /* index 0 = empty string */
    int sh_text_off  = shstrtab.size;
    secbuf_emit_bytes(&shstrtab, (uint8_t*)".text",  6);
    int sh_data_off  = shstrtab.size;
    secbuf_emit_bytes(&shstrtab, (uint8_t*)".data",  6);
    int sh_bss_off   = shstrtab.size;
    secbuf_emit_bytes(&shstrtab, (uint8_t*)".bss",   5);
    int sh_sym_off   = shstrtab.size;
    secbuf_emit_bytes(&shstrtab, (uint8_t*)".symtab",8);
    int sh_str_off   = shstrtab.size;
    secbuf_emit_bytes(&shstrtab, (uint8_t*)".strtab",8);
    int sh_shs_off   = shstrtab.size;
    secbuf_emit_bytes(&shstrtab, (uint8_t*)".shstrtab",10);

    /* strtab: symbol name strings — index 0 = empty */
    secbuf_emit(&strtab, 0);

    /* Count symbols: 1 (STN_UNDEF) + section syms + user syms */
    int nsyms = 1 + 3; /* undef + .text + .data + .bss */
    /* Count globals */
    for (Symbol *s = a->syms->head; s; s = s->next) {
        nsyms++;
    }

    /* Build symtab */
    /* Assign strtab indices */
    /* We'll build them in order: locals first, then globals */
    /* Collect symbols into arrays */
    int total_user = a->syms->count;
    Elf64_Sym *symtab_buf = calloc(1 + 3 + total_user, sizeof(Elf64_Sym));
    int si = 0;

    /* STN_UNDEF */
    memset(&symtab_buf[si++], 0, sizeof(Elf64_Sym));

    /* Section symbols */
    uint16_t text_shndx = 1, data_shndx = 2, bss_shndx = 3;
    for (int sec = 0; sec < 3; sec++) {
        Elf64_Sym *sym = &symtab_buf[si++];
        sym->st_name  = 0;
        sym->st_info  = (STB_LOCAL << 4) | STT_SECTION;
        sym->st_other = STV_DEFAULT;
        sym->st_shndx = (uint16_t)(sec + 1);
        sym->st_value = 0;
        sym->st_size  = 0;
    }
    int n_local_syms = si; /* locals before globals */

    /* User symbols — locals first */
    for (Symbol *s = a->syms->head; s; s = s->next) {
        if (s->is_global) continue;
        Elf64_Sym *sym = &symtab_buf[si++];
        sym->st_name  = strtab.size;
        secbuf_emit_bytes(&strtab, (uint8_t*)s->name, strlen(s->name)+1);
        sym->st_info  = (STB_LOCAL << 4) | STT_NOTYPE;
        sym->st_other = STV_DEFAULT;
        sym->st_shndx = (s->section == 0) ? text_shndx :
                        (s->section == 1) ? data_shndx : bss_shndx;
        sym->st_value = s->value;
        sym->st_size  = 0;
        n_local_syms = si;
    }

    /* Globals */
    for (Symbol *s = a->syms->head; s; s = s->next) {
        if (!s->is_global) continue;
        Elf64_Sym *sym = &symtab_buf[si++];
        sym->st_name  = strtab.size;
        secbuf_emit_bytes(&strtab, (uint8_t*)s->name, strlen(s->name)+1);
        sym->st_info  = (STB_GLOBAL << 4) |
                        (s->type == SYM_EXTERN ? STT_NOTYPE : STT_FUNC);
        sym->st_other = STV_DEFAULT;
        sym->st_shndx = (s->type == SYM_EXTERN) ? 0 :
                        (s->section == 0) ? text_shndx :
                        (s->section == 1) ? data_shndx : bss_shndx;
        sym->st_value = (s->type == SYM_EXTERN) ? 0 : s->value;
        sym->st_size  = 0;
    }
    int total_syms = si;

    /* Layout:
     * ELF header (64)
     * .text
     * .data
     * .symtab
     * .strtab
     * .shstrtab
     * Section headers (9 * 64)
     */
    uint64_t off = sizeof(Elf64_Ehdr);
    uint64_t text_off  = off; off += a->text.size;
    uint64_t data_off  = off; off += a->data.size;
    /* .bss has no file content */
    uint64_t sym_off   = off; off += (uint64_t)total_syms * sizeof(Elf64_Sym);
    uint64_t str_off   = off; off += strtab.size;
    uint64_t shs_off   = off; off += shstrtab.size;
    /* Align shoff to 8 */
    off = (off + 7) & ~7ULL;
    uint64_t shoff = off;

    /* 7 section headers: null, text, data, bss, symtab, strtab, shstrtab */
    int shnum = 7;
    int shstrndx = 6;

    /* Write ELF header */
    Elf64_Ehdr ehdr;
    memset(&ehdr, 0, sizeof(ehdr));
    ehdr.e_ident[0] = 0x7f; ehdr.e_ident[1] = 'E';
    ehdr.e_ident[2] = 'L';  ehdr.e_ident[3] = 'F';
    ehdr.e_ident[4] = 2;    /* 64-bit */
    ehdr.e_ident[5] = 1;    /* little-endian */
    ehdr.e_ident[6] = 1;    /* ELF version */
    ehdr.e_type      = ET_REL;
    ehdr.e_machine   = EM_X86_64;
    ehdr.e_version   = 1;
    ehdr.e_shoff     = shoff;
    ehdr.e_ehsize    = sizeof(Elf64_Ehdr);
    ehdr.e_shentsize = sizeof(Elf64_Shdr);
    ehdr.e_shnum     = (uint16_t)shnum;
    ehdr.e_shstrndx  = (uint16_t)shstrndx;
    fwrite(&ehdr, sizeof(ehdr), 1, f);

    /* Write section data */
    fwrite(a->text.data, 1, a->text.size, f);
    fwrite(a->data.data, 1, a->data.size, f);
    fwrite(symtab_buf, sizeof(Elf64_Sym), total_syms, f);
    fwrite(strtab.data, 1, strtab.size, f);
    fwrite(shstrtab.data, 1, shstrtab.size, f);

    /* Pad to shoff */
    uint64_t cur = sizeof(Elf64_Ehdr) + a->text.size + a->data.size
                 + (uint64_t)total_syms * sizeof(Elf64_Sym)
                 + strtab.size + shstrtab.size;
    while (cur < shoff) { fputc(0, f); cur++; }

    /* Section headers */
    Elf64_Shdr sh;
    /* 0: null */
    memset(&sh, 0, sizeof(sh)); fwrite(&sh, sizeof(sh), 1, f);

    /* 1: .text */
    memset(&sh, 0, sizeof(sh));
    sh.sh_name = (uint32_t)sh_text_off; sh.sh_type = SHT_PROGBITS;
    sh.sh_flags = SHF_ALLOC | SHF_EXECINSTR;
    sh.sh_offset = text_off; sh.sh_size = a->text.size;
    sh.sh_addralign = 16;
    fwrite(&sh, sizeof(sh), 1, f);

    /* 2: .data */
    memset(&sh, 0, sizeof(sh));
    sh.sh_name = (uint32_t)sh_data_off; sh.sh_type = SHT_PROGBITS;
    sh.sh_flags = SHF_ALLOC | SHF_WRITE;
    sh.sh_offset = data_off; sh.sh_size = a->data.size;
    sh.sh_addralign = 8;
    fwrite(&sh, sizeof(sh), 1, f);

    /* 3: .bss */
    memset(&sh, 0, sizeof(sh));
    sh.sh_name = (uint32_t)sh_bss_off; sh.sh_type = SHT_NOBITS;
    sh.sh_flags = SHF_ALLOC | SHF_WRITE;
    sh.sh_offset = data_off + a->data.size; sh.sh_size = a->bss_size;
    sh.sh_addralign = 8;
    fwrite(&sh, sizeof(sh), 1, f);

    /* 4: .symtab */
    memset(&sh, 0, sizeof(sh));
    sh.sh_name = (uint32_t)sh_sym_off; sh.sh_type = SHT_SYMTAB;
    sh.sh_offset = sym_off;
    sh.sh_size   = (uint64_t)total_syms * sizeof(Elf64_Sym);
    sh.sh_link   = 5; /* .strtab index */
    sh.sh_info   = (uint32_t)n_local_syms;
    sh.sh_addralign = 8; sh.sh_entsize = sizeof(Elf64_Sym);
    fwrite(&sh, sizeof(sh), 1, f);

    /* 5: .strtab */
    memset(&sh, 0, sizeof(sh));
    sh.sh_name = (uint32_t)sh_str_off; sh.sh_type = SHT_STRTAB;
    sh.sh_offset = str_off; sh.sh_size = strtab.size;
    sh.sh_addralign = 1;
    fwrite(&sh, sizeof(sh), 1, f);

    /* 6: .shstrtab */
    memset(&sh, 0, sizeof(sh));
    sh.sh_name = (uint32_t)sh_shs_off; sh.sh_type = SHT_STRTAB;
    sh.sh_offset = shs_off; sh.sh_size = shstrtab.size;
    sh.sh_addralign = 1;
    fwrite(&sh, sizeof(sh), 1, f);

    free(symtab_buf);
    free(shstrtab.data);
    free(strtab.data);
}

/* =========================================================
 * Main assembler driver
 * ========================================================= */
static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Cannot open '%s'\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f); rewind(f);
    char *buf = malloc(sz + 1);
    size_t rd = fread(buf, 1, sz, f);
    buf[rd] = '\0';
    fclose(f);
    return buf;
}

static char *path_dirname(const char *path) {
    const char *slash = strrchr(path, '/');
    const char *bslash = strrchr(path, '\\');
    const char *sep = slash > bslash ? slash : bslash;
    if (!sep) return strdup(".");
    size_t len = (size_t)(sep - path);
    char *dir = malloc(len + 1);
    memcpy(dir, path, len);
    dir[len] = '\0';
    return dir;
}

static char *expand_includes(const char *path, int depth);

static char *concat3(const char *a, const char *b, const char *c) {
    size_t la = strlen(a), lb = strlen(b), lc = strlen(c);
    char *r = malloc(la + lb + lc + 1);
    memcpy(r, a, la);
    memcpy(r + la, b, lb);
    memcpy(r + la + lb, c, lc);
    r[la + lb + lc] = '\0';
    return r;
}

static char *expand_includes(const char *path, int depth) {
    if (depth > 16) {
        fprintf(stderr, "%include depth too deep\n");
        return NULL;
    }
    char *src = read_file(path);
    if (!src) return NULL;
    char *dir = path_dirname(path);

    size_t out_cap = strlen(src) + 1;
    char *out = malloc(out_cap);
    out[0] = '\0';

    char *line = src;
    while (*line) {
        char *next = strchr(line, '\n');
        size_t len = next ? (size_t)(next - line) : strlen(line);
        char tmp[1024];
        size_t copy_len = len < sizeof(tmp)-1 ? len : sizeof(tmp)-1;
        memcpy(tmp, line, copy_len);
        tmp[copy_len] = '\0';

        char *p = tmp;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "%include", 8) == 0) {
            p += 8;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '"') {
                p++;
                char *q = strchr(p, '"');
                if (q) *q = '\0';
            }
            if (*p) {
                char *inc_path = NULL;
                if (p[0] == '/' || (strlen(p) > 1 && p[1] == ':'))
                    inc_path = strdup(p);
                else
                    inc_path = concat3(dir, "/", p);
                char *inc_src = expand_includes(inc_path, depth + 1);
                if (inc_src) {
                    size_t need = strlen(out) + strlen(inc_src) + 2;
                    if (need > out_cap) { out_cap = need * 2; out = realloc(out, out_cap); }
                    strcat(out, inc_src);
                    strcat(out, "\n");
                    free(inc_src);
                }
                free(inc_path);
            }
        } else {
            size_t need = strlen(out) + len + 2;
            if (need > out_cap) { out_cap = need * 2; out = realloc(out, out_cap); }
            strncat(out, line, len);
            strcat(out, "\n");
        }

        if (!next) break;
        line = next + 1;
    }

    free(src);
    free(dir);
    return out;
}

static void run_pass(Asm *a, const char *source) {
    if (a->lexer) lexer_free(a->lexer);
    a->lexer      = lexer_create(source);
    a->tok        = lexer_next_token(a->lexer);
    a->mode       = MODE_64BIT;
    a->cur_section = 0;
    a->text.size  = 0;
    a->data.size  = 0;
    a->bss_size   = 0;
    /* Reset jmp relocs at start of pass 2 */
    if (a->pass == 2) a->jmp_relocs = NULL;
    while (a->tok.type != TOK_EOF)
        scan_statement(a);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "NTASM x86/x86-64 Assembler\n");
        fprintf(stderr, "Usage: %s <input.asm> [-o output] [-f elf64|bin]\n", argv[0]);
        return 1;
    }

    const char *in_path  = argv[1];
    const char *out_path = "output.bin";
    int output_elf = 0;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i+1 < argc)
            out_path = argv[++i];
        else if (strcmp(argv[i], "-f") == 0 && i+1 < argc) {
            i++;
            if (strcmp(argv[i], "elf64") == 0 || strcmp(argv[i], "elf") == 0)
                output_elf = 1;
        } else if (strcmp(argv[i], "--elf64") == 0 ||
                   strcmp(argv[i], "--elf")   == 0) {
            output_elf = 1;
        } else {
            /* positional: treat as output path */
            out_path = argv[i];
        }
    }

    char *source = expand_includes(in_path, 0);
    if (!source) return 1;

    Asm a;
    memset(&a, 0, sizeof(a));
    a.syms = symtab_create();
    secbuf_init(&a.text, 4096);
    secbuf_init(&a.data, 1024);
    a.output_elf = output_elf;
    strncpy(a.out_path, out_path, 255);

    /* Pass 1: collect labels and track instruction sizes */
    a.pass = 1;
    run_pass(&a, source);

    /* Pass 2: generate code with relocations */
    a.pass = 2;
    run_pass(&a, source);

    if (a.errors) {
        fprintf(stderr, "%d error(s). Assembly failed.\n", a.errors);
        free(source);
        return 1;
    }

    /* Backpatch forward jump references */
    backpatch(&a);

    if (a.errors) {
        fprintf(stderr, "%d error(s) during backpatch.\n", a.errors);
        free(source);
        return 1;
    }

    /* Write output */
    FILE *out = fopen(out_path, "wb");
    if (!out) {
        fprintf(stderr, "Cannot create '%s'\n", out_path);
        free(source);
        return 1;
    }

    if (output_elf) {
        write_elf64(&a, out);
    } else {
        fwrite(a.text.data, 1, a.text.size, out);
        if (a.data.size > 0)
            fwrite(a.data.data, 1, a.data.size, out);
    }
    fclose(out);

    printf("Assembled: %s -> %s (%d bytes text",
           in_path, out_path, a.text.size);
    if (a.data.size) printf(", %d bytes data", a.data.size);
    if (a.bss_size)  printf(", %d bytes bss",  a.bss_size);
    printf(")\n");

    free(source);
    free(a.text.data);
    free(a.data.data);
    if (a.lexer) lexer_free(a.lexer);
    symtab_free(a.syms);

    JmpReloc *jr = a.jmp_relocs;
    while (jr) { JmpReloc *n = jr->next; free(jr); jr = n; }

    return 0;
}
