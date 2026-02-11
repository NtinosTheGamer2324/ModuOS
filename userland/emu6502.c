// emu6502.c
#include "libc.h"
#include "emu6502.h"

/* Official 6502 core (no decimal mode behavior; SED/CLD still set flag but ADC/SBC ignore it).
 * This is good for Atari 2600 (6507) and NES (2A03).
 */

/* Flags */
#define F_C 0x01
#define F_Z 0x02
#define F_I 0x04
#define F_D 0x08
#define F_B 0x10
#define F_U 0x20
#define F_V 0x40
#define F_N 0x80

static inline void set_zn(emu6502_t *c, uint8_t v) {
    if (v == 0) c->p |= F_Z; else c->p &= ~F_Z;
    if (v & 0x80) c->p |= F_N; else c->p &= ~F_N;
}

static inline uint8_t r8(emu6502_t *c, uint16_t a) { return c->read8(c->bus_user, a); }
static inline void w8(emu6502_t *c, uint16_t a, uint8_t v) { c->write8(c->bus_user, a, v); }

static inline void push8(emu6502_t *c, uint8_t v) {
    w8(c, (uint16_t)(0x0100 | c->sp), v);
    c->sp--;
}
static inline uint8_t pop8(emu6502_t *c) {
    c->sp++;
    return r8(c, (uint16_t)(0x0100 | c->sp));
}

static inline uint16_t r16(emu6502_t *c, uint16_t a) {
    uint8_t lo = r8(c, a);
    uint8_t hi = r8(c, (uint16_t)(a + 1));
    return (uint16_t)(lo | ((uint16_t)hi << 8));
}

/* 6502 indirect JMP page-wrap bug */
static inline uint16_t r16_wrap(emu6502_t *c, uint16_t a) {
    uint8_t lo = r8(c, a);
    uint16_t ah = (uint16_t)((a & 0xFF00) | ((a + 1) & 0x00FF));
    uint8_t hi = r8(c, ah);
    return (uint16_t)(lo | ((uint16_t)hi << 8));
}

void emu6502_init(emu6502_t *c, void *bus_user, emu6502_read8_fn rr, emu6502_write8_fn ww) {
    memset(c, 0, sizeof(*c));
    c->bus_user = bus_user;
    c->read8 = rr;
    c->write8 = ww;
    c->p = F_U | F_I;
    c->sp = 0xFD;
}

void emu6502_reset(emu6502_t *c) {
    c->p = F_U | F_I;
    c->sp = 0xFD;
    c->pc = r16(c, 0xFFFC);
}

/* Addressing helpers return effective address; some modes return a special marker for accumulator. */
#define ADDR_ACC 0xFFFFu

static inline uint16_t imm(emu6502_t *c) { return c->pc++; }
static inline uint16_t zp(emu6502_t *c) { return (uint16_t)r8(c, c->pc++); }
static inline uint16_t zpx(emu6502_t *c) { return (uint16_t)((r8(c, c->pc++) + c->x) & 0xFF); }
static inline uint16_t zpy(emu6502_t *c) { return (uint16_t)((r8(c, c->pc++) + c->y) & 0xFF); }
static inline uint16_t abs16(emu6502_t *c) { uint16_t a = r16(c, c->pc); c->pc += 2; return a; }
static inline uint16_t absx(emu6502_t *c) { return (uint16_t)(abs16(c) + c->x); }
static inline uint16_t absy(emu6502_t *c) { return (uint16_t)(abs16(c) + c->y); }
static inline uint16_t ind(emu6502_t *c) { return r16_wrap(c, abs16(c)); }
static inline uint16_t indx(emu6502_t *c) {
    uint8_t zpaddr = (uint8_t)(r8(c, c->pc++) + c->x);
    uint16_t a = (uint16_t)(r8(c, zpaddr) | ((uint16_t)r8(c, (uint8_t)(zpaddr + 1)) << 8));
    return a;
}
static inline uint16_t indy(emu6502_t *c) {
    uint8_t zpaddr = r8(c, c->pc++);
    uint16_t base = (uint16_t)(r8(c, zpaddr) | ((uint16_t)r8(c, (uint8_t)(zpaddr + 1)) << 8));
    return (uint16_t)(base + c->y);
}

static inline int8_t rel8(emu6502_t *c) { return (int8_t)r8(c, c->pc++); }

/* ALU helpers */
static inline void adc_bin(emu6502_t *c, uint8_t v) {
    uint16_t sum = (uint16_t)c->a + (uint16_t)v + (uint16_t)((c->p & F_C) ? 1 : 0);
    uint8_t res = (uint8_t)sum;
    /* carry */
    if (sum > 0xFF) c->p |= F_C; else c->p &= ~F_C;
    /* overflow */
    if (((c->a ^ res) & (v ^ res) & 0x80) != 0) c->p |= F_V; else c->p &= ~F_V;
    c->a = res;
    set_zn(c, c->a);
}

static inline void sbc_bin(emu6502_t *c, uint8_t v) {
    adc_bin(c, (uint8_t)(~v));
}

static inline uint8_t asl8(emu6502_t *c, uint8_t v) {
    if (v & 0x80) c->p |= F_C; else c->p &= ~F_C;
    v <<= 1;
    set_zn(c, v);
    return v;
}
static inline uint8_t lsr8(emu6502_t *c, uint8_t v) {
    if (v & 0x01) c->p |= F_C; else c->p &= ~F_C;
    v >>= 1;
    set_zn(c, v);
    return v;
}
static inline uint8_t rol8(emu6502_t *c, uint8_t v) {
    uint8_t carry = (c->p & F_C) ? 1 : 0;
    if (v & 0x80) c->p |= F_C; else c->p &= ~F_C;
    v = (uint8_t)((v << 1) | carry);
    set_zn(c, v);
    return v;
}
static inline uint8_t ror8(emu6502_t *c, uint8_t v) {
    uint8_t carry = (c->p & F_C) ? 0x80 : 0;
    if (v & 0x01) c->p |= F_C; else c->p &= ~F_C;
    v = (uint8_t)((v >> 1) | carry);
    set_zn(c, v);
    return v;
}

/* Helpers for read/modify/write instructions */
static inline uint8_t read_op(emu6502_t *c, uint16_t a, int acc, uint8_t *accv) {
    if (acc) return *accv;
    return r8(c, a);
}
static inline void write_op(emu6502_t *c, uint16_t a, int acc, uint8_t *accv, uint8_t v) {
    if (acc) *accv = v;
    else w8(c, a, v);
}

/* Base cycles table for official opcodes (rough; good enough initially). */
static const uint8_t base_cycles[256] = {
    /* generated-ish minimal: default 2 */
    [0 ... 255] = 2,
    [0x00] = 7, [0x20] = 6, [0x60] = 6, [0x40] = 6,
    [0x4C] = 3, [0x6C] = 5,
};

uint32_t emu6502_step(emu6502_t *c) {
    uint8_t op = r8(c, c->pc++);
    uint32_t cyc = base_cycles[op];

    switch (op) {
        /* --- Loads --- */
        case 0xA9: c->a = r8(c, imm(c)); set_zn(c, c->a); break;
        case 0xA5: c->a = r8(c, zp(c)); set_zn(c, c->a); cyc = 3; break;
        case 0xB5: c->a = r8(c, zpx(c)); set_zn(c, c->a); cyc = 4; break;
        case 0xAD: c->a = r8(c, abs16(c)); set_zn(c, c->a); cyc = 4; break;
        case 0xBD: c->a = r8(c, absx(c)); set_zn(c, c->a); cyc = 4; break;
        case 0xB9: c->a = r8(c, absy(c)); set_zn(c, c->a); cyc = 4; break;
        case 0xA1: c->a = r8(c, indx(c)); set_zn(c, c->a); cyc = 6; break;
        case 0xB1: c->a = r8(c, indy(c)); set_zn(c, c->a); cyc = 5; break;

        case 0xA2: c->x = r8(c, imm(c)); set_zn(c, c->x); break;
        case 0xA6: c->x = r8(c, zp(c)); set_zn(c, c->x); cyc = 3; break;
        case 0xB6: c->x = r8(c, zpy(c)); set_zn(c, c->x); cyc = 4; break;
        case 0xAE: c->x = r8(c, abs16(c)); set_zn(c, c->x); cyc = 4; break;
        case 0xBE: c->x = r8(c, absy(c)); set_zn(c, c->x); cyc = 4; break;

        case 0xA0: c->y = r8(c, imm(c)); set_zn(c, c->y); break;
        case 0xA4: c->y = r8(c, zp(c)); set_zn(c, c->y); cyc = 3; break;
        case 0xB4: c->y = r8(c, zpx(c)); set_zn(c, c->y); cyc = 4; break;
        case 0xAC: c->y = r8(c, abs16(c)); set_zn(c, c->y); cyc = 4; break;
        case 0xBC: c->y = r8(c, absx(c)); set_zn(c, c->y); cyc = 4; break;

        /* --- Stores --- */
        case 0x85: w8(c, zp(c), c->a); cyc = 3; break;
        case 0x95: w8(c, zpx(c), c->a); cyc = 4; break;
        case 0x8D: w8(c, abs16(c), c->a); cyc = 4; break;
        case 0x9D: w8(c, absx(c), c->a); cyc = 5; break;
        case 0x99: w8(c, absy(c), c->a); cyc = 5; break;
        case 0x81: w8(c, indx(c), c->a); cyc = 6; break;
        case 0x91: w8(c, indy(c), c->a); cyc = 6; break;

        case 0x86: w8(c, zp(c), c->x); cyc = 3; break;
        case 0x96: w8(c, zpy(c), c->x); cyc = 4; break;
        case 0x8E: w8(c, abs16(c), c->x); cyc = 4; break;

        case 0x84: w8(c, zp(c), c->y); cyc = 3; break;
        case 0x94: w8(c, zpx(c), c->y); cyc = 4; break;
        case 0x8C: w8(c, abs16(c), c->y); cyc = 4; break;

        /* --- Transfers --- */
        case 0xAA: c->x = c->a; set_zn(c, c->x); break;
        case 0xA8: c->y = c->a; set_zn(c, c->y); break;
        case 0x8A: c->a = c->x; set_zn(c, c->a); break;
        case 0x98: c->a = c->y; set_zn(c, c->a); break;
        case 0xBA: c->x = c->sp; set_zn(c, c->x); break;
        case 0x9A: c->sp = c->x; break;

        /* --- Inc/Dec --- */
        case 0xE8: c->x++; set_zn(c, c->x); break;
        case 0xCA: c->x--; set_zn(c, c->x); break;
        case 0xC8: c->y++; set_zn(c, c->y); break;
        case 0x88: c->y--; set_zn(c, c->y); break;

        /* --- Jumps/Calls --- */
        case 0x4C: c->pc = abs16(c); cyc = 3; break;
        case 0x6C: c->pc = ind(c); cyc = 5; break;
        case 0x20: {
            uint16_t target = abs16(c);
            uint16_t ret = (uint16_t)(c->pc - 1);
            push8(c, (uint8_t)(ret >> 8));
            push8(c, (uint8_t)(ret & 0xFF));
            c->pc = target;
            cyc = 6;
            break;
        }
        case 0x60: {
            uint8_t lo = pop8(c);
            uint8_t hi = pop8(c);
            c->pc = (uint16_t)(((uint16_t)hi << 8) | lo);
            c->pc++;
            cyc = 6;
            break;
        }

        /* --- Stack --- */
        case 0x48: push8(c, c->a); cyc = 3; break;
        case 0x68: c->a = pop8(c); set_zn(c, c->a); cyc = 4; break;
        case 0x08: push8(c, (uint8_t)(c->p | F_B | F_U)); cyc = 3; break;
        case 0x28: c->p = (uint8_t)((pop8(c) | F_U) & ~F_B); cyc = 4; break;

        /* --- Flags --- */
        case 0x18: c->p &= ~F_C; break;
        case 0x38: c->p |= F_C; break;
        case 0x58: c->p &= ~F_I; break;
        case 0x78: c->p |= F_I; break;
        case 0xB8: c->p &= ~F_V; break;
        case 0xD8: c->p &= ~F_D; break;
        case 0xF8: c->p |= F_D; break;

        /* --- ALU (all main addressing modes) --- */
        case 0x69: adc_bin(c, r8(c, imm(c))); cyc = 2; break;
        case 0x65: adc_bin(c, r8(c, zp(c))); cyc = 3; break;
        case 0x75: adc_bin(c, r8(c, zpx(c))); cyc = 4; break;
        case 0x6D: adc_bin(c, r8(c, abs16(c))); cyc = 4; break;
        case 0x7D: adc_bin(c, r8(c, absx(c))); cyc = 4; break;
        case 0x79: adc_bin(c, r8(c, absy(c))); cyc = 4; break;
        case 0x61: adc_bin(c, r8(c, indx(c))); cyc = 6; break;
        case 0x71: adc_bin(c, r8(c, indy(c))); cyc = 5; break;

        case 0xE9: sbc_bin(c, r8(c, imm(c))); cyc = 2; break;
        case 0xE5: sbc_bin(c, r8(c, zp(c))); cyc = 3; break;
        case 0xF5: sbc_bin(c, r8(c, zpx(c))); cyc = 4; break;
        case 0xED: sbc_bin(c, r8(c, abs16(c))); cyc = 4; break;
        case 0xFD: sbc_bin(c, r8(c, absx(c))); cyc = 4; break;
        case 0xF9: sbc_bin(c, r8(c, absy(c))); cyc = 4; break;
        case 0xE1: sbc_bin(c, r8(c, indx(c))); cyc = 6; break;
        case 0xF1: sbc_bin(c, r8(c, indy(c))); cyc = 5; break;

        case 0x29: c->a &= r8(c, imm(c)); set_zn(c, c->a); cyc = 2; break;
        case 0x25: c->a &= r8(c, zp(c)); set_zn(c, c->a); cyc = 3; break;
        case 0x35: c->a &= r8(c, zpx(c)); set_zn(c, c->a); cyc = 4; break;
        case 0x2D: c->a &= r8(c, abs16(c)); set_zn(c, c->a); cyc = 4; break;
        case 0x3D: c->a &= r8(c, absx(c)); set_zn(c, c->a); cyc = 4; break;
        case 0x39: c->a &= r8(c, absy(c)); set_zn(c, c->a); cyc = 4; break;
        case 0x21: c->a &= r8(c, indx(c)); set_zn(c, c->a); cyc = 6; break;
        case 0x31: c->a &= r8(c, indy(c)); set_zn(c, c->a); cyc = 5; break;

        case 0x09: c->a |= r8(c, imm(c)); set_zn(c, c->a); cyc = 2; break;
        case 0x05: c->a |= r8(c, zp(c)); set_zn(c, c->a); cyc = 3; break;
        case 0x15: c->a |= r8(c, zpx(c)); set_zn(c, c->a); cyc = 4; break;
        case 0x0D: c->a |= r8(c, abs16(c)); set_zn(c, c->a); cyc = 4; break;
        case 0x1D: c->a |= r8(c, absx(c)); set_zn(c, c->a); cyc = 4; break;
        case 0x19: c->a |= r8(c, absy(c)); set_zn(c, c->a); cyc = 4; break;
        case 0x01: c->a |= r8(c, indx(c)); set_zn(c, c->a); cyc = 6; break;
        case 0x11: c->a |= r8(c, indy(c)); set_zn(c, c->a); cyc = 5; break;

        case 0x49: c->a ^= r8(c, imm(c)); set_zn(c, c->a); cyc = 2; break;
        case 0x45: c->a ^= r8(c, zp(c)); set_zn(c, c->a); cyc = 3; break;
        case 0x55: c->a ^= r8(c, zpx(c)); set_zn(c, c->a); cyc = 4; break;
        case 0x4D: c->a ^= r8(c, abs16(c)); set_zn(c, c->a); cyc = 4; break;
        case 0x5D: c->a ^= r8(c, absx(c)); set_zn(c, c->a); cyc = 4; break;
        case 0x59: c->a ^= r8(c, absy(c)); set_zn(c, c->a); cyc = 4; break;
        case 0x41: c->a ^= r8(c, indx(c)); set_zn(c, c->a); cyc = 6; break;
        case 0x51: c->a ^= r8(c, indy(c)); set_zn(c, c->a); cyc = 5; break;

        /* --- Branches --- */
        case 0xF0: { int8_t d = rel8(c); if (c->p & F_Z) c->pc = (uint16_t)(c->pc + d); cyc = 2; break; } /* BEQ */
        case 0xD0: { int8_t d = rel8(c); if (!(c->p & F_Z)) c->pc = (uint16_t)(c->pc + d); cyc = 2; break; } /* BNE */
        case 0xB0: { int8_t d = rel8(c); if (c->p & F_C) c->pc = (uint16_t)(c->pc + d); cyc = 2; break; } /* BCS */
        case 0x90: { int8_t d = rel8(c); if (!(c->p & F_C)) c->pc = (uint16_t)(c->pc + d); cyc = 2; break; } /* BCC */
        case 0x30: { int8_t d = rel8(c); if (c->p & F_N) c->pc = (uint16_t)(c->pc + d); cyc = 2; break; } /* BMI */
        case 0x10: { int8_t d = rel8(c); if (!(c->p & F_N)) c->pc = (uint16_t)(c->pc + d); cyc = 2; break; } /* BPL */
        case 0x70: { int8_t d = rel8(c); if (c->p & F_V) c->pc = (uint16_t)(c->pc + d); cyc = 2; break; } /* BVS */
        case 0x50: { int8_t d = rel8(c); if (!(c->p & F_V)) c->pc = (uint16_t)(c->pc + d); cyc = 2; break; } /* BVC */

        /* --- Shifts accumulator --- */
        case 0x0A: c->a = asl8(c, c->a); cyc = 2; break;
        case 0x4A: c->a = lsr8(c, c->a); cyc = 2; break;
        case 0x2A: c->a = rol8(c, c->a); cyc = 2; break;
        case 0x6A: c->a = ror8(c, c->a); cyc = 2; break;

        /* --- NOP --- */
        case 0xEA: cyc = 2; break;

        /* --- CMP/CPX/CPY (complete common modes) --- */
        case 0xC9: { uint8_t v = r8(c, imm(c)); uint16_t t = (uint16_t)c->a - v; if (c->a >= v) c->p |= F_C; else c->p &= ~F_C; set_zn(c, (uint8_t)t); cyc = 2; break; }
        case 0xC5: { uint8_t v = r8(c, zp(c)); uint16_t t = (uint16_t)c->a - v; if (c->a >= v) c->p |= F_C; else c->p &= ~F_C; set_zn(c, (uint8_t)t); cyc = 3; break; }
        case 0xD5: { uint8_t v = r8(c, zpx(c)); uint16_t t = (uint16_t)c->a - v; if (c->a >= v) c->p |= F_C; else c->p &= ~F_C; set_zn(c, (uint8_t)t); cyc = 4; break; }
        case 0xCD: { uint8_t v = r8(c, abs16(c)); uint16_t t = (uint16_t)c->a - v; if (c->a >= v) c->p |= F_C; else c->p &= ~F_C; set_zn(c, (uint8_t)t); cyc = 4; break; }
        case 0xDD: { uint8_t v = r8(c, absx(c)); uint16_t t = (uint16_t)c->a - v; if (c->a >= v) c->p |= F_C; else c->p &= ~F_C; set_zn(c, (uint8_t)t); cyc = 4; break; }
        case 0xD9: { uint8_t v = r8(c, absy(c)); uint16_t t = (uint16_t)c->a - v; if (c->a >= v) c->p |= F_C; else c->p &= ~F_C; set_zn(c, (uint8_t)t); cyc = 4; break; }
        case 0xC1: { uint8_t v = r8(c, indx(c)); uint16_t t = (uint16_t)c->a - v; if (c->a >= v) c->p |= F_C; else c->p &= ~F_C; set_zn(c, (uint8_t)t); cyc = 6; break; }
        case 0xD1: { uint8_t v = r8(c, indy(c)); uint16_t t = (uint16_t)c->a - v; if (c->a >= v) c->p |= F_C; else c->p &= ~F_C; set_zn(c, (uint8_t)t); cyc = 5; break; }

        case 0xE0: { uint8_t v = r8(c, imm(c)); uint16_t t = (uint16_t)c->x - v; if (c->x >= v) c->p |= F_C; else c->p &= ~F_C; set_zn(c, (uint8_t)t); cyc = 2; break; }
        case 0xE4: { uint8_t v = r8(c, zp(c)); uint16_t t = (uint16_t)c->x - v; if (c->x >= v) c->p |= F_C; else c->p &= ~F_C; set_zn(c, (uint8_t)t); cyc = 3; break; }
        case 0xEC: { uint8_t v = r8(c, abs16(c)); uint16_t t = (uint16_t)c->x - v; if (c->x >= v) c->p |= F_C; else c->p &= ~F_C; set_zn(c, (uint8_t)t); cyc = 4; break; }

        case 0xC0: { uint8_t v = r8(c, imm(c)); uint16_t t = (uint16_t)c->y - v; if (c->y >= v) c->p |= F_C; else c->p &= ~F_C; set_zn(c, (uint8_t)t); cyc = 2; break; }
        case 0xC4: { uint8_t v = r8(c, zp(c)); uint16_t t = (uint16_t)c->y - v; if (c->y >= v) c->p |= F_C; else c->p &= ~F_C; set_zn(c, (uint8_t)t); cyc = 3; break; }
        case 0xCC: { uint8_t v = r8(c, abs16(c)); uint16_t t = (uint16_t)c->y - v; if (c->y >= v) c->p |= F_C; else c->p &= ~F_C; set_zn(c, (uint8_t)t); cyc = 4; break; }

        /* --- BIT --- */
        case 0x24: { uint8_t v = r8(c, zp(c)); if (v & 0x80) c->p |= F_N; else c->p &= ~F_N; if (v & 0x40) c->p |= F_V; else c->p &= ~F_V; if ((v & c->a) == 0) c->p |= F_Z; else c->p &= ~F_Z; cyc = 3; break; }
        case 0x2C: { uint8_t v = r8(c, abs16(c)); if (v & 0x80) c->p |= F_N; else c->p &= ~F_N; if (v & 0x40) c->p |= F_V; else c->p &= ~F_V; if ((v & c->a) == 0) c->p |= F_Z; else c->p &= ~F_Z; cyc = 4; break; }

        /* --- INC/DEC memory --- */
        case 0xE6: { uint16_t a = zp(c); uint8_t v = (uint8_t)(r8(c,a)+1); w8(c,a,v); set_zn(c,v); cyc=5; break; }
        case 0xF6: { uint16_t a = zpx(c); uint8_t v = (uint8_t)(r8(c,a)+1); w8(c,a,v); set_zn(c,v); cyc=6; break; }
        case 0xEE: { uint16_t a = abs16(c); uint8_t v = (uint8_t)(r8(c,a)+1); w8(c,a,v); set_zn(c,v); cyc=6; break; }
        case 0xFE: { uint16_t a = absx(c); uint8_t v = (uint8_t)(r8(c,a)+1); w8(c,a,v); set_zn(c,v); cyc=7; break; }

        case 0xC6: { uint16_t a = zp(c); uint8_t v = (uint8_t)(r8(c,a)-1); w8(c,a,v); set_zn(c,v); cyc=5; break; }
        case 0xD6: { uint16_t a = zpx(c); uint8_t v = (uint8_t)(r8(c,a)-1); w8(c,a,v); set_zn(c,v); cyc=6; break; }
        case 0xCE: { uint16_t a = abs16(c); uint8_t v = (uint8_t)(r8(c,a)-1); w8(c,a,v); set_zn(c,v); cyc=6; break; }
        case 0xDE: { uint16_t a = absx(c); uint8_t v = (uint8_t)(r8(c,a)-1); w8(c,a,v); set_zn(c,v); cyc=7; break; }

        /* --- RMW shifts on memory --- */
        case 0x06: { uint16_t a = zp(c); uint8_t v = asl8(c, r8(c,a)); w8(c,a,v); cyc=5; break; }
        case 0x16: { uint16_t a = zpx(c); uint8_t v = asl8(c, r8(c,a)); w8(c,a,v); cyc=6; break; }
        case 0x0E: { uint16_t a = abs16(c); uint8_t v = asl8(c, r8(c,a)); w8(c,a,v); cyc=6; break; }
        case 0x1E: { uint16_t a = absx(c); uint8_t v = asl8(c, r8(c,a)); w8(c,a,v); cyc=7; break; }

        case 0x46: { uint16_t a = zp(c); uint8_t v = lsr8(c, r8(c,a)); w8(c,a,v); cyc=5; break; }
        case 0x56: { uint16_t a = zpx(c); uint8_t v = lsr8(c, r8(c,a)); w8(c,a,v); cyc=6; break; }
        case 0x4E: { uint16_t a = abs16(c); uint8_t v = lsr8(c, r8(c,a)); w8(c,a,v); cyc=6; break; }
        case 0x5E: { uint16_t a = absx(c); uint8_t v = lsr8(c, r8(c,a)); w8(c,a,v); cyc=7; break; }

        case 0x26: { uint16_t a = zp(c); uint8_t v = rol8(c, r8(c,a)); w8(c,a,v); cyc=5; break; }
        case 0x36: { uint16_t a = zpx(c); uint8_t v = rol8(c, r8(c,a)); w8(c,a,v); cyc=6; break; }
        case 0x2E: { uint16_t a = abs16(c); uint8_t v = rol8(c, r8(c,a)); w8(c,a,v); cyc=6; break; }
        case 0x3E: { uint16_t a = absx(c); uint8_t v = rol8(c, r8(c,a)); w8(c,a,v); cyc=7; break; }

        case 0x66: { uint16_t a = zp(c); uint8_t v = ror8(c, r8(c,a)); w8(c,a,v); cyc=5; break; }
        case 0x76: { uint16_t a = zpx(c); uint8_t v = ror8(c, r8(c,a)); w8(c,a,v); cyc=6; break; }
        case 0x6E: { uint16_t a = abs16(c); uint8_t v = ror8(c, r8(c,a)); w8(c,a,v); cyc=6; break; }
        case 0x7E: { uint16_t a = absx(c); uint8_t v = ror8(c, r8(c,a)); w8(c,a,v); cyc=7; break; }

        /* --- BRK/RTI --- */
        case 0x00: {
            /* BRK: push PC+P, set I, jump IRQ vector. */
            uint16_t pc = c->pc;
            push8(c, (uint8_t)(pc >> 8));
            push8(c, (uint8_t)(pc & 0xFF));
            push8(c, (uint8_t)(c->p | F_B | F_U));
            c->p |= F_I;
            c->pc = r16(c, 0xFFFE);
            cyc = 7;
            break;
        }
        case 0x40:
            /* RTI (basic) */
            c->p = (uint8_t)((pop8(c) | F_U) & ~F_B);
            {
                uint8_t lo = pop8(c);
                uint8_t hi = pop8(c);
                c->pc = (uint16_t)(((uint16_t)hi << 8) | lo);
            }
            cyc = 6;
            break;

        default:
            /* Illegal/unknown: treat as NOP so ROM keeps running. */
            cyc = 2;
            break;
    }

    c->cycles += cyc;
    return cyc;
}
