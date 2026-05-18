/**
 * kzlib.h — Minimal kernel-space zlib (DEFLATE) decompressor for ModuOS
 *
 * Self-contained, freestanding inflate implementation. No dynamic memory allocation
 * or any libc function. All workspace memory is internal (stack-allocated
 * fixed-size tables inside the decompressor state).
 *
 * Only RFC 1950 (zlib wrapper) + RFC 1951 (DEFLATE) decompression.
 * Compression is not supported.
 */

#ifndef MODUOS_KERNEL_ILIB_KZLIB_H
#define MODUOS_KERNEL_ILIB_KZLIB_H

#include <stdint.h>
#include <stddef.h>

/* ── Result codes ─────────────────────────────────────────────────── */
typedef enum {
    KZLIB_OK           =  0,  /* Decompression successful                */
    KZLIB_ERR_DATA     = -1,  /* Corrupt / invalid compressed data       */
    KZLIB_ERR_BUF      = -2,  /* Output buffer too small                 */
    KZLIB_ERR_HEADER   = -3,  /* Bad zlib stream header (CMF/FLG)        */
    KZLIB_ERR_CHECKSUM = -4,  /* Adler-32 mismatch                       */
    KZLIB_ERR_BAD_ARG  = -5,  /* NULL pointer or zero-length input       */
} kzlib_result_t;

/**
 * kzlib_decompress — decompress a zlib (RFC 1950) stream in one shot.
 *
 * @param src      Pointer to the compressed zlib data.
 * @param src_len  Byte length of the compressed stream.
 * @param dst      Output buffer receiving decompressed bytes.
 * @param dst_cap  Capacity of dst in bytes.
 * @param out_len  On KZLIB_OK: bytes written to dst. 0 on failure.
 * @return         KZLIB_OK, or a negative kzlib_result_t on error.
 */
kzlib_result_t kzlib_decompress(const uint8_t *src, size_t src_len,
                                 uint8_t       *dst, size_t  dst_cap,
                                 size_t        *out_len);

/** Static human-readable string for a result code. Never NULL. */
const char *kzlib_strerror(kzlib_result_t r);

#endif /* MODUOS_KERNEL_ILIB_KZLIB_H */