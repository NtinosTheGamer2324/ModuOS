#pragma once
// ilib.h — Userland ILIB image library loader for ModuOS
// Self-contained: includes a minimal zlib (RFC 1950) / DEFLATE decompressor.
// No external dependencies beyond libc.h (malloc, free, open, read, lseek, close).
//
// Usage:
//   ilib_t *lib = ilib_open("/ModuOS/shared/assets/mouse.ilib");
//   ilib_image_t img;
//   if (ilib_load_image(lib, 0, &img) == 0) {
//       // img.pixels  — RGBA pixel data (R,G,B,A order, top-down)
//       // img.width / img.height
//       ilib_free_image(&img);
//   }
//   ilib_close(lib);
//
// Copyright © 2026 New Technologies Software — GPL v2.0

#include <stdint.h>
#include <stddef.h>

// ============================================================
// Public API types
// ============================================================

typedef struct {
    uint16_t image_id;
    uint16_t width;
    uint16_t height;
    uint32_t raw_size;
    uint32_t cmp_size;
    uint32_t file_offset;
} ilib_entry_t;

typedef struct {
    uint16_t   width;
    uint16_t   height;
    uint8_t   *pixels;   // malloc'd RGBA, caller must ilib_free_image()
} ilib_image_t;

typedef struct {
    int          fd;
    uint16_t     count;
    ilib_entry_t entries[1]; // flexible — actually count entries
} ilib_t;

// ============================================================
// Error codes
// ============================================================
#define ILIB_OK            0
#define ILIB_ERR_IO       -1
#define ILIB_ERR_MAGIC    -2
#define ILIB_ERR_BADENTRY -3
#define ILIB_ERR_NOMEM    -4
#define ILIB_ERR_ZLIB     -5
#define ILIB_ERR_NOTFOUND -6

// ============================================================
// Minimal zlib / DEFLATE decompressor
// Supports:
//   - zlib wrapper (RFC 1950): CMF/FLG header, Adler-32 trailer
//   - DEFLATE (RFC 1951): uncompressed, fixed Huffman, dynamic Huffman
// ============================================================

// --- Bit-stream reader ---
typedef struct {
    const uint8_t *src;
    size_t         src_len;
    size_t         src_pos;
    uint32_t       bits;      // bit buffer
    int            bits_avail;
} ilib__bitstream_t;

static inline void ilib__bs_init(ilib__bitstream_t *bs, const uint8_t *src, size_t len) {
    bs->src        = src;
    bs->src_len    = len;
    bs->src_pos    = 0;
    bs->bits       = 0;
    bs->bits_avail = 0;
}

static inline int ilib__bs_fill(ilib__bitstream_t *bs) {
    while (bs->bits_avail <= 24) {
        if (bs->src_pos >= bs->src_len) break;
        bs->bits |= (uint32_t)bs->src[bs->src_pos++] << bs->bits_avail;
        bs->bits_avail += 8;
    }
    return bs->bits_avail;
}

static inline uint32_t ilib__bs_peek(ilib__bitstream_t *bs, int n) {
    ilib__bs_fill(bs);
    return bs->bits & ((1u << n) - 1u);
}

static inline void ilib__bs_consume(ilib__bitstream_t *bs, int n) {
    bs->bits >>= n;
    bs->bits_avail -= n;
}

static inline uint32_t ilib__bs_read(ilib__bitstream_t *bs, int n) {
    if (n == 0) return 0;
    uint32_t v = ilib__bs_peek(bs, n);
    ilib__bs_consume(bs, n);
    return v;
}

// Align to next byte boundary (used for uncompressed blocks)
static inline void ilib__bs_align(ilib__bitstream_t *bs) {
    int leftover = bs->bits_avail & 7;
    if (leftover) { bs->bits >>= leftover; bs->bits_avail -= leftover; }
}

// Read a full byte from the byte stream (byte-aligned)
static inline int ilib__bs_byte(ilib__bitstream_t *bs) {
    ilib__bs_align(bs);
    if (bs->src_pos >= bs->src_len) return -1;
    return (int)bs->src[bs->src_pos++];
}

// --- Huffman tables ---
#define ILIB__MAXCODES 288
#define ILIB__MAXBITS  15

typedef struct {
    uint16_t count[ILIB__MAXBITS + 1]; // symbols per bit-length
    uint16_t symbol[ILIB__MAXCODES];   // symbols sorted by code
} ilib__huffman_t;

// Build a canonical Huffman table from code lengths
static int ilib__huff_build(ilib__huffman_t *h, const uint8_t *lengths, int n) {
    int offsets[ILIB__MAXBITS + 2];
    for (int i = 0; i <= ILIB__MAXBITS; i++) h->count[i] = 0;
    for (int i = 0; i < n; i++) if (lengths[i]) h->count[lengths[i]]++;

    // Verify the code lengths are valid
    int left = 1;
    for (int i = 1; i <= ILIB__MAXBITS; i++) {
        left <<= 1;
        left -= h->count[i];
        if (left < 0) return ILIB_ERR_ZLIB;
    }

    offsets[1] = 0;
    for (int i = 1; i < ILIB__MAXBITS; i++)
        offsets[i + 1] = offsets[i] + h->count[i];

    for (int i = 0; i < n; i++)
        if (lengths[i])
            h->symbol[offsets[lengths[i]]++] = (uint16_t)i;

    return ILIB_OK;
}

// Decode one symbol using a canonical Huffman table
static int ilib__huff_decode(ilib__bitstream_t *bs, const ilib__huffman_t *h) {
    int code  = 0;
    int first = 0;
    int index = 0;
    for (int len = 1; len <= ILIB__MAXBITS; len++) {
        ilib__bs_fill(bs);
        // Read one bit (LSB-first)
        code |= (int)(bs->bits & 1);
        bs->bits >>= 1;
        bs->bits_avail--;

        int count = h->count[len];
        if (code - count < first) {
            return h->symbol[index + (code - first)];
        }
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return ILIB_ERR_ZLIB;
}

// --- Fixed Huffman tables (RFC 1951  3.2.6) ---
static ilib__huffman_t ilib__g_litlen_fixed;
static ilib__huffman_t ilib__g_dist_fixed;
static int             ilib__g_fixed_built = 0;

static void ilib__build_fixed(void) {
    if (ilib__g_fixed_built) return;
    uint8_t lengths[288];
    int i;
    for (i =   0; i < 144; i++) lengths[i] = 8;
    for (i = 144; i < 256; i++) lengths[i] = 9;
    for (i = 256; i < 280; i++) lengths[i] = 7;
    for (i = 280; i < 288; i++) lengths[i] = 8;
    ilib__huff_build(&ilib__g_litlen_fixed, lengths, 288);
    for (i = 0; i < 32; i++) lengths[i] = 5;
    ilib__huff_build(&ilib__g_dist_fixed, lengths, 32);
    ilib__g_fixed_built = 1;
}

// --- Length / distance extra-bit tables ---
static const uint16_t ilib__len_base[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,
    35,43,51,59,67,83,99,115,131,163,195,227,258
};
static const uint8_t ilib__len_extra[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,
    3,3,3,3,4,4,4,4,5,5,5,5,0
};
static const uint16_t ilib__dist_base[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,
    257,385,513,769,1025,1537,2049,3073,4097,6145,
    8193,12289,16385,24577
};
static const uint8_t ilib__dist_extra[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,
    7,7,8,8,9,9,10,10,11,11,12,12,13,13
};

// Code-length alphabet ordering (RFC 1951  3.2.7)
static const uint8_t ilib__clorder[19] = {
    16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
};

// --- Core DEFLATE decompressor ---
static int ilib__deflate(ilib__bitstream_t *bs,
                          uint8_t *out, size_t out_cap, size_t *out_written)
{
    size_t pos = 0;
    int bfinal;

    do {
        bfinal = (int)ilib__bs_read(bs, 1);
        int btype = (int)ilib__bs_read(bs, 2);

        if (btype == 0) {
            // --- Uncompressed block ---
            ilib__bs_align(bs);
            int lo = ilib__bs_byte(bs);
            int hi = ilib__bs_byte(bs);
            int nc_lo = ilib__bs_byte(bs);
            int nc_hi = ilib__bs_byte(bs);
            if (lo < 0 || hi < 0 || nc_lo < 0 || nc_hi < 0) return ILIB_ERR_ZLIB;
            uint16_t len  = (uint16_t)(lo | (hi << 8));
            uint16_t nlen = (uint16_t)(nc_lo | (nc_hi << 8));
            if ((uint16_t)(len ^ nlen) != 0xFFFF) return ILIB_ERR_ZLIB;
            for (uint16_t k = 0; k < len; k++) {
                int b = ilib__bs_byte(bs);
                if (b < 0) return ILIB_ERR_ZLIB;
                if (pos >= out_cap) return ILIB_ERR_ZLIB;
                out[pos++] = (uint8_t)b;
            }

        } else if (btype == 1 || btype == 2) {
            ilib__huffman_t  dyn_litlen, dyn_dist;
            ilib__huffman_t *hl, *hd;

            if (btype == 1) {
                // Fixed Huffman
                ilib__build_fixed();
                hl = &ilib__g_litlen_fixed;
                hd = &ilib__g_dist_fixed;
            } else {
                // Dynamic Huffman: read code-length tables
                int hlit  = (int)ilib__bs_read(bs, 5) + 257;
                int hdist = (int)ilib__bs_read(bs, 5) + 1;
                int hclen = (int)ilib__bs_read(bs, 4) + 4;

                uint8_t cl_lengths[19] = {0};
                for (int i = 0; i < hclen; i++)
                    cl_lengths[ilib__clorder[i]] = (uint8_t)ilib__bs_read(bs, 3);

                ilib__huffman_t hcl;
                int r = ilib__huff_build(&hcl, cl_lengths, 19);
                if (r != ILIB_OK) return r;

                uint8_t all_lengths[288 + 32];
                int total = hlit + hdist;
                for (int i = 0; i < total; ) {
                    int sym = ilib__huff_decode(bs, &hcl);
                    if (sym < 0) return ILIB_ERR_ZLIB;
                    if (sym < 16) {
                        all_lengths[i++] = (uint8_t)sym;
                    } else if (sym == 16) {
                        if (i == 0) return ILIB_ERR_ZLIB;
                        uint8_t prev = all_lengths[i - 1];
                        int rep = (int)ilib__bs_read(bs, 2) + 3;
                        for (int k = 0; k < rep && i < total; k++) all_lengths[i++] = prev;
                    } else if (sym == 17) {
                        int rep = (int)ilib__bs_read(bs, 3) + 3;
                        for (int k = 0; k < rep && i < total; k++) all_lengths[i++] = 0;
                    } else if (sym == 18) {
                        int rep = (int)ilib__bs_read(bs, 7) + 11;
                        for (int k = 0; k < rep && i < total; k++) all_lengths[i++] = 0;
                    } else {
                        return ILIB_ERR_ZLIB;
                    }
                }
                r = ilib__huff_build(&dyn_litlen, all_lengths,          hlit);
                if (r != ILIB_OK) return r;
                r = ilib__huff_build(&dyn_dist,   all_lengths + hlit,   hdist);
                if (r != ILIB_OK) return r;
                hl = &dyn_litlen;
                hd = &dyn_dist;
            }

            // Decode symbols
            for (;;) {
                int sym = ilib__huff_decode(bs, hl);
                if (sym < 0) return ILIB_ERR_ZLIB;
                if (sym < 256) {
                    if (pos >= out_cap) return ILIB_ERR_ZLIB;
                    out[pos++] = (uint8_t)sym;
                } else if (sym == 256) {
                    break; // end of block
                } else {
                    // Back-reference
                    int lcode = sym - 257;
                    if (lcode >= 29) return ILIB_ERR_ZLIB;
                    uint32_t length = ilib__len_base[lcode]
                                    + ilib__bs_read(bs, ilib__len_extra[lcode]);

                    int dsym = ilib__huff_decode(bs, hd);
                    if (dsym < 0 || dsym >= 30) return ILIB_ERR_ZLIB;
                    uint32_t dist = ilib__dist_base[dsym]
                                  + ilib__bs_read(bs, ilib__dist_extra[dsym]);

                    if (dist > pos) return ILIB_ERR_ZLIB;
                    if (pos + length > out_cap) return ILIB_ERR_ZLIB;
                    size_t src_off = pos - dist;
                    for (uint32_t k = 0; k < length; k++)
                        out[pos++] = out[src_off++];
                }
            }
        } else {
            return ILIB_ERR_ZLIB; // reserved block type
        }
    } while (!bfinal);

    *out_written = pos;
    return ILIB_OK;
}

// --- Adler-32 (zlib trailer check) ---
static uint32_t ilib__adler32(const uint8_t *data, size_t len) {
    uint32_t s1 = 1, s2 = 0;
    for (size_t i = 0; i < len; i++) {
        s1 = (s1 + data[i]) % 65521;
        s2 = (s2 + s1)       % 65521;
    }
    return (s2 << 16) | s1;
}

// --- Top-level zlib (RFC 1950) decompressor ---
// Decompresses `src_len` bytes at `src` into a malloc'd buffer.
// Sets *out_data and *out_len on success. Caller must free(*out_data).
static int ilib__zlib_decompress(const uint8_t *src, size_t src_len,
                                  uint8_t **out_data, size_t *out_len,
                                  size_t expected_raw_size)
{
    if (src_len < 6) return ILIB_ERR_ZLIB;

    // zlib header
    uint8_t cmf = src[0];
    uint8_t flg = src[1];
    if ((cmf & 0x0F) != 8)         return ILIB_ERR_ZLIB; // CM must be 8 (deflate)
    if (((cmf * 256 + flg) % 31) != 0) return ILIB_ERR_ZLIB; // FCHECK
    if (flg & 0x20)                return ILIB_ERR_ZLIB; // FDICT not supported

    uint8_t *buf = (uint8_t *)malloc(expected_raw_size);
    if (!buf) return ILIB_ERR_NOMEM;

    ilib__bitstream_t bs;
    ilib__bs_init(&bs, src + 2, src_len - 6); // skip CMF+FLG, skip 4-byte Adler trailer
    size_t written = 0;
    int r = ilib__deflate(&bs, buf, expected_raw_size, &written);
    if (r != ILIB_OK) { free(buf); return r; }
    if (written != expected_raw_size) { free(buf); return ILIB_ERR_ZLIB; }

    // Verify Adler-32 trailer (big-endian, last 4 bytes of zlib stream)
    uint32_t stored_adler = ((uint32_t)src[src_len-4] << 24)
                          | ((uint32_t)src[src_len-3] << 16)
                          | ((uint32_t)src[src_len-2] <<  8)
                          | ((uint32_t)src[src_len-1]);
    uint32_t computed = ilib__adler32(buf, written);
    if (stored_adler != computed) { free(buf); return ILIB_ERR_ZLIB; }

    *out_data = buf;
    *out_len  = written;
    return ILIB_OK;
}

// ============================================================
// ILIB file I/O helpers  (uses libc.h open/read/lseek/close)
// ============================================================

#define ILIB_MAGIC "ILIB"

static int ilib__read_exact(int fd, void *buf, size_t n) {
    uint8_t *p = (uint8_t *)buf;
    size_t done = 0;
    while (done < n) {
        ssize_t r = read(fd, p + done, n - done);
        if (r <= 0) return ILIB_ERR_IO;
        done += (size_t)r;
    }
    return ILIB_OK;
}

// ============================================================
// Public API implementation
// ============================================================

// Open an .ilib file. Returns a heap-allocated ilib_t or NULL on error.
// The file descriptor is kept open until ilib_close().
static ilib_t *ilib_open(const char *path) {
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return NULL;

    // Read header: 4-byte magic + 2-byte count
    uint8_t hdr[6];
    if (ilib__read_exact(fd, hdr, 6) != ILIB_OK) { close(fd); return NULL; }
    if (hdr[0]!='I'||hdr[1]!='L'||hdr[2]!='I'||hdr[3]!='B') { close(fd); return NULL; }
    uint16_t count = (uint16_t)(hdr[4] | (hdr[5] << 8));

    // Allocate ilib_t with embedded entry array
    size_t alloc = sizeof(ilib_t) + (count > 1 ? (count - 1) : 0) * sizeof(ilib_entry_t);
    ilib_t *lib = (ilib_t *)malloc(alloc);
    if (!lib) { close(fd); return NULL; }
    lib->fd    = fd;
    lib->count = count;

    // Read resource table
    for (uint16_t i = 0; i < count; i++) {
        uint8_t e[18];
        if (ilib__read_exact(fd, e, 18) != ILIB_OK) { free(lib); close(fd); return NULL; }
        ilib_entry_t *en = &lib->entries[i];
        en->image_id    = (uint16_t)(e[0]  | (e[1]  << 8));
        en->width       = (uint16_t)(e[2]  | (e[3]  << 8));
        en->height      = (uint16_t)(e[4]  | (e[5]  << 8));
        en->raw_size    = (uint32_t)(e[6]  | (e[7]  << 8) | (e[8]  << 16) | (e[9]  << 24));
        en->cmp_size    = (uint32_t)(e[10] | (e[11] << 8) | (e[12] << 16) | (e[13] << 24));
        en->file_offset = (uint32_t)(e[14] | (e[15] << 8) | (e[16] << 16) | (e[17] << 24));

        // Validate raw_size invariant
        if (en->raw_size != (uint32_t)en->width * en->height * 4) {
            free(lib); close(fd); return NULL;
        }
    }
    return lib;
}

// Close and free an ilib_t.
static void ilib_close(ilib_t *lib) {
    if (!lib) return;
    close(lib->fd);
    free(lib);
}

// Load a single image by ID into img. img->pixels is malloc'd; call ilib_free_image() when done.
static int ilib_load_image(ilib_t *lib, uint16_t image_id, ilib_image_t *img) {
    if (!lib || !img) return ILIB_ERR_NOTFOUND;

    ilib_entry_t *en = NULL;
    for (uint16_t i = 0; i < lib->count; i++) {
        if (lib->entries[i].image_id == image_id) { en = &lib->entries[i]; break; }
    }
    if (!en) return ILIB_ERR_NOTFOUND;

    // Seek to payload
    if (lseek(lib->fd, (long)en->file_offset, 0 /*SEEK_SET*/) < 0) return ILIB_ERR_IO;

    // Read compressed payload
    uint8_t *cmp = (uint8_t *)malloc(en->cmp_size);
    if (!cmp) return ILIB_ERR_NOMEM;
    if (ilib__read_exact(lib->fd, cmp, en->cmp_size) != ILIB_OK) { free(cmp); return ILIB_ERR_IO; }

    // Decompress
    uint8_t *pixels = NULL;
    size_t   raw_len = 0;
    int r = ilib__zlib_decompress(cmp, en->cmp_size, &pixels, &raw_len, en->raw_size);
    free(cmp);
    if (r != ILIB_OK) return r;

    img->width  = en->width;
    img->height = en->height;
    img->pixels = pixels;
    return ILIB_OK;
}

// Free pixel data previously loaded by ilib_load_image().
static void ilib_free_image(ilib_image_t *img) {
    if (img && img->pixels) {
        free(img->pixels);
        img->pixels = NULL;
    }
}