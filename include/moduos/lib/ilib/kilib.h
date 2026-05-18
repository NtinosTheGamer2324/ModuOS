/**
 * kilib.h — Kernel-space .ilib image library reader for ModuOS
 *
 * Kernel-adapted version of the userland ilib API.
 * Differences from userland ilib:
 *   - Uses kmalloc/kfree  instead of malloc/free
 *   - Uses fs_read_file() instead of fopen/fread
 *   - Uses kzlib         instead of zlib
 *   - No FILE* or libc I/O whatsoever
 *
 * Typical kernel usage:
 *
 *   fs_mount_t *mnt = kernel_get_boot_mount();
 *
 *   kilib_t *lib = NULL;
 *   kilib_error_t err = kilib_open(mnt, "/boot/bimg/bootimgs.ilib", &lib);
 *   if (err != KILIB_OK) { ... }
 *
 *   uint8_t  *pixels = NULL;
 *   uint16_t  w, h;
 *   err = kilib_load_by_name(lib, "Generic_bootimg", &pixels, &w, &h);
 *   // -- or by numeric ID --
 *   err = kilib_load(lib, 0, &pixels, &w, &h);
 *
 *   // ... blit pixels to framebuffer ...
 *
 *   kfree(pixels);      // caller frees
 *   kilib_close(lib);   // release handle
 *
 * Place at:  moduos/kernel/ilib/kilib.h
 */

#ifndef MODUOS_KERNEL_ILIB_KILIB_H
#define MODUOS_KERNEL_ILIB_KILIB_H

#include <stdint.h>
#include <stddef.h>
#include "moduos/fs/fs.h"

#ifdef __cplusplus
extern "C" {
#endif


/* ─── Error codes ──────────────────────────────────────────────────── */

typedef enum {
    KILIB_OK              =  0,
    KILIB_ERR_IO          = -1,  /* fs_read_file / fs_stat failed          */
    KILIB_ERR_BAD_MAGIC   = -2,  /* File does not start with "ILIB"        */
    KILIB_ERR_CORRUPT     = -3,  /* Resource table truncated or malformed  */
    KILIB_ERR_NOT_FOUND   = -4,  /* Image ID / name not in library         */
    KILIB_ERR_DECOMPRESS  = -5,  /* kzlib decompression failed             */
    KILIB_ERR_NO_MEM      = -6,  /* kmalloc returned NULL                  */
    KILIB_ERR_BAD_ARG     = -7,  /* Required pointer argument is NULL      */
    KILIB_ERR_NO_MOUNT    = -8,  /* Mount handle is NULL or invalid        */
} kilib_error_t;

/** Static human-readable description of an error code. Never NULL. */
const char *kilib_strerror(kilib_error_t err);


/* ─── Public types ─────────────────────────────────────────────────── */

/** Opaque kernel library handle. */
typedef struct kilib kilib_t;

/** Per-image metadata (mirrors the binary resource-table entry). */
typedef struct {
    uint16_t image_id;       /* Numeric image ID (0-based, compile order)  */
    uint16_t width;          /* Width in pixels                            */
    uint16_t height;         /* Height in pixels                           */
    uint32_t raw_size;       /* Uncompressed RGBA bytes  (w * h * 4)       */
    uint32_t cmp_size;       /* Compressed payload bytes                   */
    uint32_t file_offset;    /* Absolute byte offset of payload in file    */
    char     name[64];       /* Logical name (stem of original filename)   */
} kilib_image_info_t;


/* ─── Lifecycle ────────────────────────────────────────────────────── */

/**
 * kilib_open — read, validate, and parse an .ilib file from a mounted FS.
 *
 * Allocates one contiguous kmalloc block for the file data and a second
 * for the resource table. The FS is not accessed again after open.
 *
 * @param mount   Active mount handle (e.g. kernel_get_boot_mount()).
 * @param path    Absolute path within the mount (e.g. "/boot/bimg/imgs.ilib").
 * @param out     Receives the handle pointer on success; NULL on failure.
 * @return        KILIB_OK or a negative kilib_error_t.
 */
kilib_error_t kilib_open(fs_mount_t *mount, const char *path, kilib_t **out);

/**
 * kilib_close — release all kernel memory for a handle.
 * Safe to call with NULL (no-op).
 */
void kilib_close(kilib_t *lib);


/* ─── Queries ──────────────────────────────────────────────────────── */

/** Number of images in the library, or 0 if lib is NULL. */
uint16_t kilib_count(const kilib_t *lib);

/**
 * kilib_info — metadata for an image by numeric ID.
 * @return KILIB_OK, KILIB_ERR_NOT_FOUND, or KILIB_ERR_BAD_ARG.
 */
kilib_error_t kilib_info(const kilib_t *lib, uint16_t id,
                          kilib_image_info_t *info);

/**
 * kilib_info_by_index — metadata for the Nth image (0-based).
 * Useful when iterating without knowing IDs in advance.
 */
kilib_error_t kilib_info_by_index(const kilib_t *lib, uint16_t index,
                                   kilib_image_info_t *info);

/**
 * kilib_find_id — look up the numeric ID for a logical name.
 *
 * The name is matched against the stored entry name (case-sensitive).
 * Example: kilib_find_id(lib, "Generic_bootimg", &id);
 *
 * @param name   Logical name to search for (no path, no extension).
 * @param id_out Receives the image ID on success.
 * @return       KILIB_OK or KILIB_ERR_NOT_FOUND.
 */
kilib_error_t kilib_find_id(const kilib_t *lib, const char *name,
                             uint16_t *id_out);


/* ─── Image decoding ───────────────────────────────────────────────── */

/**
 * kilib_load — decompress an image by numeric ID.
 *
 * Allocates (width * height * 4) bytes with kmalloc().
 * The caller MUST call kfree(*pixels_out) when done.
 *
 * Pixel layout: top-down, R G B A, 1 byte per channel, no padding.
 *
 * @param lib         Valid kilib handle.
 * @param id          Image ID to decode.
 * @param pixels_out  Receives the kmalloc'd RGBA buffer. NULL on failure.
 * @param width_out   Receives width in pixels  (may be NULL).
 * @param height_out  Receives height in pixels (may be NULL).
 * @return            KILIB_OK or negative kilib_error_t.
 */
kilib_error_t kilib_load(const kilib_t *lib, uint16_t id,
                          uint8_t **pixels_out,
                          uint16_t *width_out, uint16_t *height_out);

/**
 * kilib_load_by_index — decompress the Nth image (0-based).
 * Identical to kilib_load() but selects by position, not ID.
 */
kilib_error_t kilib_load_by_index(const kilib_t *lib, uint16_t index,
                                   uint8_t **pixels_out,
                                   uint16_t *width_out, uint16_t *height_out);

/**
 * kilib_load_by_name — decompress an image by logical name.
 *
 * Combines kilib_find_id() + kilib_load() in one call.
 * Useful for the bootscreen: kilib_load_by_name(lib, "ASUS_bootimg", ...)
 */
kilib_error_t kilib_load_by_name(const kilib_t *lib, const char *name,
                                  uint8_t **pixels_out,
                                  uint16_t *width_out, uint16_t *height_out);


#ifdef __cplusplus
}
#endif
#endif /* MODUOS_KERNEL_ILIB_KILIB_H */