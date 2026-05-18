/**
 * kzlib.c — Kernel-space DEFLATE/zlib decompressor for ModuOS
 *
 * Implements:
 *   - RFC 1950  zlib framing  (2-byte header + Adler-32 trailer)
 *   - RFC 1951  DEFLATE       (uncompressed, fixed Huffman, dynamic Huffman)
 *
 * Constraints:
 *   - Completely freestanding: zero libc calls.
 *   - No dynamic allocation: all tables are on the call stack of
 *     kzlib_decompress() (≈ 8 KB worst-case stack depth).
 *   - C99 compatible.
 *
 * Place at:  moduos/kernel/ilib/kzlib.c
 */

#include "moduos/lib/ilib/kzlib.h"

/* ── Portable types & helpers ──────────────────────────────────────── */

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int32_t  i32;
typedef size_t   usize;

static inline u8  kz_min_u8 (u8  a, u8  b) { return a < b ? a : b; }
static inline u32 kz_min_u32(u32 a, u32 b) { return a < b ? a : b; }
static inline usize kz_min_sz(usize a, usize b) { return a < b ? a : b; }

/* ── DEFLATE constants ─────────────────────────────────────────────── */

#define MAXBITS      15
#define MAXLCODES   286   /* literal/length codes   */
#define MAXDCODES    30   /* distance codes          */
#define MAXCODES    316   /* max codes in one table  */
#define FIXLCODES   288

/* Extra bits and base values for length codes 257-285 */
static const u8 LENGTH_EXTRA[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0
};
static const u16 LENGTH_BASE[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,
    35,43,51,59,67,83,99,115,131,163,195,227,258
};

/* Extra bits and base values for distance codes 0-29 */
static const u8 DIST_EXTRA[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13
};
static const u16 DIST_BASE[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,
    257,385,513,769,1025,1537,2049,3073,4097,6145,
    8193,12289,16385,24577
};

/* Code length order for dynamic Huffman header */
static const u8 CLCL_ORDER[19] = {
    16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
};

/* ── Huffman table ─────────────────────────────────────────────────── */
typedef struct {
    i32  count[MAXBITS + 1];   /* symbols per bit-length              */
    i32  symbol[MAXCODES];     /* symbols sorted by code              */
} Huffman;

/* ── Bit-stream state ──────────────────────────────────────────────── */
typedef struct {
    const u8 *src;
    usize     src_len;
    usize     src_pos;
    u32       bits;       /* bit buffer                              */
    u32       nbits;      /* valid bits in buffer                    */
    u8       *dst;
    usize     dst_cap;
    usize     dst_pos;
    int       err;        /* non-zero = error has occurred           */
} State;

/* ── Bit reading ───────────────────────────────────────────────────── */

static inline void fill_bits(State *s) {
    while (s->nbits < 16 && s->src_pos < s->src_len) {
        s->bits |= (u32)s->src[s->src_pos++] << s->nbits;
        s->nbits += 8;
    }
}

static inline u32 peek_bits(State *s, u32 n) {
    fill_bits(s);
    return s->bits & ((1u << n) - 1u);
}

static inline u32 read_bits(State *s, u32 n) {
    u32 v = peek_bits(s, n);
    s->bits  >>= n;
    s->nbits  -= n;
    return v;
}

/* Read n bits without reversing (used for literal block lengths) */
static inline u32 read_bits_rev(State *s, u32 n) {
    /* bits are already LSB-first in DEFLATE */
    return read_bits(s, n);
}

/* Align to next byte boundary and rewind src_pos to account for bytes
 * that fill_bits pre-fetched into the bit buffer but haven't been
 * consumed yet. This is required before reading the Adler-32 trailer
 * directly from src[]. */
static inline void byte_align(State *s) {
    u32 waste = s->nbits & 7u;
    s->bits  >>= waste;
    s->nbits  -= waste;
    /* Put back fully-buffered bytes that were pre-fetched but not used */
    u32 buffered_bytes = s->nbits >> 3;
    s->src_pos -= buffered_bytes;
    s->bits     = 0;
    s->nbits    = 0;
}

/* ── Output ────────────────────────────────────────────────────────── */

static inline void emit(State *s, u8 byte) {
    if (s->dst_pos >= s->dst_cap) { s->err = KZLIB_ERR_BUF; return; }
    s->dst[s->dst_pos++] = byte;
}

static inline void emit_copy(State *s, u32 dist, u32 len) {
    if (dist == 0 || dist > s->dst_pos) { s->err = KZLIB_ERR_DATA; return; }
    for (u32 i = 0; i < len; i++) {
        if (s->dst_pos >= s->dst_cap) { s->err = KZLIB_ERR_BUF; return; }
        s->dst[s->dst_pos] = s->dst[s->dst_pos - dist];
        s->dst_pos++;
    }
}

/* ── Huffman table construction ────────────────────────────────────── */

static int build_huffman(Huffman *h, const u8 *lengths, u32 n) {
    i32 offs[MAXBITS + 1];
    i32 left;
    u32 i;

    for (i = 0; i <= MAXBITS; i++) h->count[i] = 0;
    for (i = 0; i < n; i++) h->count[lengths[i]]++;

    h->count[0] = 0;

    left = 1;
    for (i = 1; i <= MAXBITS; i++) {
        left <<= 1;
        left  -= h->count[i];
        if (left < 0) return KZLIB_ERR_DATA;
    }

    offs[1] = 0;
    for (i = 1; i < MAXBITS; i++)
        offs[i + 1] = offs[i] + h->count[i];

    for (i = 0; i < n; i++)
        if (lengths[i] != 0)
            h->symbol[offs[lengths[i]]++] = (i32)i;

    return KZLIB_OK;
}

/* ── Huffman decode ────────────────────────────────────────────────── */

static i32 decode_symbol(State *s, const Huffman *h) {
    i32 code  = 0;
    i32 first = 0;
    i32 idx   = 0;

    for (i32 len = 1; len <= MAXBITS; len++) {
        code  |= (i32)read_bits(s, 1);
        i32 count = h->count[len];
        if (code - count < first) {
            return h->symbol[idx + (code - first)];
        }
        idx   += count;
        first  = (first + count) << 1;
        code <<= 1;
    }
    return KZLIB_ERR_DATA;
}

/* ── Block decoders ────────────────────────────────────────────────── */

static void decode_block_uncompressed(State *s) {
    byte_align(s);

    if (s->src_pos + 4 > s->src_len) { s->err = KZLIB_ERR_DATA; return; }
    u16 len  = (u16)(s->src[s->src_pos]     | ((u16)s->src[s->src_pos + 1] << 8));
    u16 nlen = (u16)(s->src[s->src_pos + 2] | ((u16)s->src[s->src_pos + 3] << 8));
    s->src_pos += 4;

    if ((u16)(~nlen) != len) { s->err = KZLIB_ERR_DATA; return; }
    if (s->src_pos + len > s->src_len) { s->err = KZLIB_ERR_DATA; return; }

    for (u16 i = 0; i < len; i++)
        emit(s, s->src[s->src_pos++]);
}

static void decode_block_huffman(State *s,
                                  const Huffman *lh,
                                  const Huffman *dh)
{
    while (!s->err) {
        i32 sym = decode_symbol(s, lh);
        if (sym < 0)   { s->err = KZLIB_ERR_DATA; return; }
        if (sym == 256) break;  /* end of block */

        if (sym < 256) {
            emit(s, (u8)sym);
        } else {
            /* Length/distance back-reference */
            u32 li = (u32)(sym - 257);
            if (li >= 29) { s->err = KZLIB_ERR_DATA; return; }
            u32 len = LENGTH_BASE[li] + read_bits(s, LENGTH_EXTRA[li]);

            i32 dsym = decode_symbol(s, dh);
            if (dsym < 0 || dsym >= 30) { s->err = KZLIB_ERR_DATA; return; }
            u32 dist = DIST_BASE[dsym] + read_bits(s, DIST_EXTRA[dsym]);

            emit_copy(s, dist, len);
        }
    }
}

/* Fixed Huffman trees (RFC 1951 §3.2.6) */
static void decode_block_fixed(State *s) {
    u8 lengths[FIXLCODES + MAXDCODES];
    u32 i;

    /* Literal/length: 0-143 → 8 bits, 144-255 → 9 bits,
                       256-279 → 7 bits, 280-287 → 8 bits */
    for (i = 0;   i <= 143; i++) lengths[i] = 8;
    for (i = 144; i <= 255; i++) lengths[i] = 9;
    for (i = 256; i <= 279; i++) lengths[i] = 7;
    for (i = 280; i <= 287; i++) lengths[i] = 8;

    /* Distance codes: all 5 bits */
    for (i = 0; i < MAXDCODES; i++) lengths[FIXLCODES + i] = 5;

    Huffman lh, dh;
    if (build_huffman(&lh, lengths,            FIXLCODES) != KZLIB_OK ||
        build_huffman(&dh, lengths + FIXLCODES, MAXDCODES) != KZLIB_OK)
    {
        s->err = KZLIB_ERR_DATA;
        return;
    }
    decode_block_huffman(s, &lh, &dh);
}

/* Dynamic Huffman trees */
static void decode_block_dynamic(State *s) {
    u32 nlit  = read_bits(s, 5) + 257;
    u32 ndist = read_bits(s, 5) + 1;
    u32 ncode = read_bits(s, 4) + 4;

    if (nlit > MAXLCODES || ndist > MAXDCODES) { s->err = KZLIB_ERR_DATA; return; }

    /* Code-length code lengths */
    u8 clengths[19] = {0};
    for (u32 i = 0; i < ncode; i++)
        clengths[CLCL_ORDER[i]] = (u8)read_bits(s, 3);

    Huffman clh;
    if (build_huffman(&clh, clengths, 19) != KZLIB_OK) { s->err = KZLIB_ERR_DATA; return; }

    /* Decode literal + distance code lengths */
    u8 lengths[MAXLCODES + MAXDCODES];
    u32 total = nlit + ndist;
    u32 i = 0;
    u8  prev = 0;

    while (i < total && !s->err) {
        i32 sym = decode_symbol(s, &clh);
        if (sym < 0) { s->err = KZLIB_ERR_DATA; return; }

        if (sym < 16) {
            lengths[i++] = prev = (u8)sym;
        } else if (sym == 16) {
            u32 rep = read_bits(s, 2) + 3;
            while (rep-- && i < total) lengths[i++] = prev;
        } else if (sym == 17) {
            u32 rep = read_bits(s, 3) + 3;
            while (rep-- && i < total) lengths[i++] = prev = 0;
        } else { /* sym == 18 */
            u32 rep = read_bits(s, 7) + 11;
            while (rep-- && i < total) lengths[i++] = prev = 0;
        }
    }
    if (i != total) { s->err = KZLIB_ERR_DATA; return; }

    Huffman lh, dh;
    if (build_huffman(&lh, lengths,        nlit)  != KZLIB_OK ||
        build_huffman(&dh, lengths + nlit, ndist) != KZLIB_OK)
    {
        s->err = KZLIB_ERR_DATA;
        return;
    }
    decode_block_huffman(s, &lh, &dh);
}

/* ── Adler-32 ──────────────────────────────────────────────────────── */

static u32 adler32(const u8 *data, usize len) {
    u32 s1 = 1, s2 = 0;
    for (usize i = 0; i < len; i++) {
        s1 = (s1 + data[i]) % 65521u;
        s2 = (s2 + s1)      % 65521u;
    }
    return (s2 << 16) | s1;
}

/* ── Public API ────────────────────────────────────────────────────── */

kzlib_result_t kzlib_decompress(const u8 *src, usize src_len,
                                 u8       *dst, usize  dst_cap,
                                 usize    *out_len)
{
    if (out_len) *out_len = 0;
    if (!src || !dst || !out_len || src_len < 6) return KZLIB_ERR_BAD_ARG;

    /* ── zlib header (RFC 1950) ── */
    u8 cmf = src[0];
    u8 flg = src[1];

    /* CMF: CM must be 8 (DEFLATE), CINFO ≤ 7 */
    if ((cmf & 0x0F) != 8)          return KZLIB_ERR_HEADER;
    if (((cmf << 8) | flg) % 31 != 0) return KZLIB_ERR_HEADER;
    /* FDICT not supported */
    if (flg & 0x20)                  return KZLIB_ERR_HEADER;

    State s;
    s.src     = src;
    s.src_len = src_len;
    s.src_pos = 2;          /* skip zlib header */
    s.bits    = 0;
    s.nbits   = 0;
    s.dst     = dst;
    s.dst_cap = dst_cap;
    s.dst_pos = 0;
    s.err     = KZLIB_OK;

    /* ── DEFLATE blocks ── */
    int bfinal = 0;
    while (!bfinal && !s.err) {
        bfinal     = (int)read_bits(&s, 1);
        u32 btype  =      read_bits(&s, 2);

        switch (btype) {
            case 0: decode_block_uncompressed(&s); break;
            case 1: decode_block_fixed(&s);        break;
            case 2: decode_block_dynamic(&s);      break;
            default: s.err = KZLIB_ERR_DATA;       break;
        }
    }

    if (s.err) return (kzlib_result_t)s.err;

    /* ── Adler-32 trailer ── */
    byte_align(&s);
    if (s.src_pos + 4 > src_len) return KZLIB_ERR_DATA;

    u32 stored_adler = ((u32)src[s.src_pos]     << 24)
                     | ((u32)src[s.src_pos + 1] << 16)
                     | ((u32)src[s.src_pos + 2] <<  8)
                     |  (u32)src[s.src_pos + 3];

    u32 computed_adler = adler32(dst, s.dst_pos);
    if (stored_adler != computed_adler) return KZLIB_ERR_CHECKSUM;

    *out_len = s.dst_pos;
    return KZLIB_OK;
}

const char *kzlib_strerror(kzlib_result_t r) {
    switch (r) {
        case KZLIB_OK:            return "OK";
        case KZLIB_ERR_DATA:      return "Corrupt or invalid DEFLATE stream";
        case KZLIB_ERR_BUF:       return "Output buffer too small";
        case KZLIB_ERR_HEADER:    return "Bad zlib stream header";
        case KZLIB_ERR_CHECKSUM:  return "Adler-32 checksum mismatch";
        case KZLIB_ERR_BAD_ARG:   return "Bad argument (NULL pointer or zero length)";
        default:                  return "Unknown error";
    }
}