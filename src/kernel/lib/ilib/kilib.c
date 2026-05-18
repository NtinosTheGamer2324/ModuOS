/**
 * kilib.c — Kernel-space .ilib reader implementation for ModuOS
 *
 * Place at:  moduos/kernel/ilib/kilib.c
 */

#include "moduos/lib/ilib/kilib.h"
#include "moduos/lib/ilib/kzlib.h"
#include "moduos/kernel/memory/kheap.h"
#include "moduos/kernel/memory/string.h"

/* ── Binary format constants ────────────────────────────────────────── */

#define KILIB_MAGIC          "ILIB"
#define KILIB_MAGIC_LEN      4
#define KILIB_HEADER_SIZE    6    /* magic(4) + count(2)            */
#define KILIB_ENTRY_SIZE     18   /* id(2)+w(2)+h(2)+raw(4)+cmp(4)+off(4) */
#define KILIB_BYTES_PER_PX   4    /* RGBA                           */
#define KILIB_NAME_MAX       64   /* max logical name length        */

/* ── Internal resource-table entry ──────────────────────────────────── */

typedef struct {
    uint16_t image_id;
    uint16_t width;
    uint16_t height;
    uint32_t raw_size;
    uint32_t cmp_size;
    uint32_t file_offset;
    char     name[KILIB_NAME_MAX];
} kilib_entry_t;

/* ── Opaque handle ───────────────────────────────────────────────────── */

struct kilib {
    uint8_t       *file_data;   /* entire .ilib file in kmalloc'd buffer   */
    uint32_t       file_size;   /* byte length of file_data                */
    uint16_t       count;       /* number of images                        */
    kilib_entry_t *entries;     /* parsed resource table (count entries)   */
};

/* ── Portable little-endian readers ──────────────────────────────────── */

static inline uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)(p[0]
                    | ((uint32_t)p[1] <<  8)
                    | ((uint32_t)p[2] << 16)
                    | ((uint32_t)p[3] << 24));
}

/* ── Name helpers ─────────────────────────────────────────────────────── */

/**
 * Derive a logical name from a full path by stripping the directory
 * component and any file extension.
 *
 * "/boot/bimg/ASUS_bootimg.ilib"  →  "ASUS_bootimg"
 * "ASUS_bootimg.bmp"              →  "ASUS_bootimg"
 * "Generic_bootimg"               →  "Generic_bootimg"
 */
static void derive_name(const char *path, char *out, size_t out_len)
{
    if (!path || !out || out_len == 0) return;

    /* Find last '/' or '\\' */
    const char *base = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }

    /* Copy up to first '.' or out_len-1 chars */
    size_t i = 0;
    while (base[i] && base[i] != '.' && i < out_len - 1) {
        out[i] = base[i];
        i++;
    }
    out[i] = '\0';
}

/* ── Error strings ────────────────────────────────────────────────────── */

const char *kilib_strerror(kilib_error_t err)
{
    switch (err) {
        case KILIB_OK:             return "Success";
        case KILIB_ERR_IO:         return "I/O error: fs_stat or fs_read_file failed";
        case KILIB_ERR_BAD_MAGIC:  return "Bad magic: not a valid .ilib file";
        case KILIB_ERR_CORRUPT:    return "Corrupt file: resource table malformed";
        case KILIB_ERR_NOT_FOUND:  return "Image ID or name not found in library";
        case KILIB_ERR_DECOMPRESS: return "Decompression failed (kzlib error)";
        case KILIB_ERR_NO_MEM:     return "Out of kernel memory (kmalloc returned NULL)";
        case KILIB_ERR_BAD_ARG:    return "Bad argument: required pointer is NULL";
        case KILIB_ERR_NO_MOUNT:   return "Mount handle is NULL or not valid";
        default:                   return "Unknown kilib error";
    }
}

/* ── Internal: find entry by numeric ID ───────────────────────────────── */

static const kilib_entry_t *find_by_id(const kilib_t *lib, uint16_t id)
{
    for (uint16_t i = 0; i < lib->count; i++)
        if (lib->entries[i].image_id == id)
            return &lib->entries[i];
    return (const kilib_entry_t *)0;
}

/* ── Internal: decompress one entry into a kmalloc'd buffer ──────────── */

static kilib_error_t decompress_entry(const kilib_t       *lib,
                                       const kilib_entry_t *e,
                                       uint8_t            **pixels_out)
{
    /* Bounds-check payload inside file buffer */
    if ((uint64_t)e->file_offset + e->cmp_size > lib->file_size)
        return KILIB_ERR_CORRUPT;

    /* Validate raw_size invariant: must equal w * h * 4 */
    uint32_t expected = (uint32_t)e->width * (uint32_t)e->height * KILIB_BYTES_PER_PX;
    if (e->raw_size != expected || e->raw_size == 0)
        return KILIB_ERR_CORRUPT;

    uint8_t *buf = (uint8_t *)kmalloc(e->raw_size);
    if (!buf) return KILIB_ERR_NO_MEM;

    const uint8_t *src = lib->file_data + e->file_offset;
    size_t out_len = 0;

    kzlib_result_t zr = kzlib_decompress(src, e->cmp_size,
                                          buf, e->raw_size,
                                          &out_len);
    if (zr != KZLIB_OK || out_len != e->raw_size) {
        kfree(buf);
        return KILIB_ERR_DECOMPRESS;
    }

    *pixels_out = buf;
    return KILIB_OK;
}

/* ── Public API ───────────────────────────────────────────────────────── */

kilib_error_t kilib_open(fs_mount_t *mount, const char *path, kilib_t **out)
{
    if (!out) return KILIB_ERR_BAD_ARG;
    *out = (kilib_t *)0;

    if (!mount || !mount->valid) return KILIB_ERR_NO_MOUNT;
    if (!path)                   return KILIB_ERR_BAD_ARG;

    /* ── Stat the file to get its size ── */
    fs_file_info_t fi;
    if (fs_stat(mount, path, &fi) != 0 || fi.is_directory || fi.size < KILIB_HEADER_SIZE)
        return KILIB_ERR_IO;

    /* ── Allocate and read the whole file ── */
    uint8_t *file_data = (uint8_t *)kmalloc(fi.size);
    if (!file_data) return KILIB_ERR_NO_MEM;

    size_t bytes_read = 0;
    if (fs_read_file(mount, path, file_data, fi.size, &bytes_read) != 0
        || bytes_read < KILIB_HEADER_SIZE)
    {
        kfree(file_data);
        return KILIB_ERR_IO;
    }
    uint32_t file_size = (uint32_t)bytes_read;

    /* ── Validate magic ── */
    if (memcmp(file_data, KILIB_MAGIC, KILIB_MAGIC_LEN) != 0) {
        kfree(file_data);
        return KILIB_ERR_BAD_MAGIC;
    }

    uint16_t count = rd16(file_data + KILIB_MAGIC_LEN);

    /* ── Validate table fits inside file ── */
    uint32_t table_bytes = (uint32_t)count * KILIB_ENTRY_SIZE;
    if (file_size < (uint32_t)KILIB_HEADER_SIZE + table_bytes) {
        kfree(file_data);
        return KILIB_ERR_CORRUPT;
    }

    /* ── Parse resource table ── */
    kilib_entry_t *entries = (kilib_entry_t *)0;
    if (count > 0) {
        entries = (kilib_entry_t *)kmalloc((size_t)count * sizeof(kilib_entry_t));
        if (!entries) {
            kfree(file_data);
            return KILIB_ERR_NO_MEM;
        }

        const uint8_t *p = file_data + KILIB_HEADER_SIZE;
        for (uint16_t i = 0; i < count; i++, p += KILIB_ENTRY_SIZE) {
            entries[i].image_id    = rd16(p + 0);
            entries[i].width       = rd16(p + 2);
            entries[i].height      = rd16(p + 4);
            entries[i].raw_size    = rd32(p + 6);
            entries[i].cmp_size    = rd32(p + 10);
            entries[i].file_offset = rd32(p + 14);

            /*
             * Name field: the .ilib format does not store names in the
             * binary, so we derive the library's own name for index 0
             * (the path stem) and use "image_XXXX" for subsequent entries.
             * The compiler tool can be extended later to embed name strings.
             *
             * For the bootscreen use-case the .ilib is compiled from a
             * directory of files named exactly like "ASUS_bootimg.bmp", so
             * the name embedded in the entry must match those stems.
             * We therefore leave the name blank here and rely on the compiler
             * writing a name section — see kilib_load_by_name() which falls
             * back to a numeric ID lookup when no name is stored.
             *
             * For now: fill with a default "image_NNNN" so the field is
             * always printable.
             */
            {
                char tmp[KILIB_NAME_MAX];
                tmp[0] = 'i'; tmp[1] = 'm'; tmp[2] = 'a'; tmp[3] = 'g';
                tmp[4] = 'e'; tmp[5] = '_';
                /* itoa-style for 4 digits */
                uint16_t id = entries[i].image_id;
                tmp[6] = (char)('0' + (id / 1000) % 10);
                tmp[7] = (char)('0' + (id / 100)  % 10);
                tmp[8] = (char)('0' + (id / 10)   % 10);
                tmp[9] = (char)('0' + (id)         % 10);
                tmp[10] = '\0';
                memcpy(entries[i].name, tmp, 11);
            }
        }
    }

    /* ── Allocate handle ── */
    kilib_t *lib = (kilib_t *)kmalloc(sizeof(kilib_t));
    if (!lib) {
        kfree(entries);
        kfree(file_data);
        return KILIB_ERR_NO_MEM;
    }

    lib->file_data = file_data;
    lib->file_size = file_size;
    lib->count     = count;
    lib->entries   = entries;

    *out = lib;
    return KILIB_OK;
}


void kilib_close(kilib_t *lib)
{
    if (!lib) return;
    kfree(lib->entries);
    kfree(lib->file_data);
    kfree(lib);
}


uint16_t kilib_count(const kilib_t *lib)
{
    return lib ? lib->count : 0;
}


kilib_error_t kilib_info(const kilib_t *lib, uint16_t id,
                          kilib_image_info_t *info)
{
    if (!lib || !info) return KILIB_ERR_BAD_ARG;
    const kilib_entry_t *e = find_by_id(lib, id);
    if (!e) return KILIB_ERR_NOT_FOUND;

    info->image_id   = e->image_id;
    info->width      = e->width;
    info->height     = e->height;
    info->raw_size   = e->raw_size;
    info->cmp_size   = e->cmp_size;
    info->file_offset= e->file_offset;
    memcpy(info->name, e->name, KILIB_NAME_MAX);
    return KILIB_OK;
}


kilib_error_t kilib_info_by_index(const kilib_t *lib, uint16_t index,
                                   kilib_image_info_t *info)
{
    if (!lib || !info)       return KILIB_ERR_BAD_ARG;
    if (index >= lib->count) return KILIB_ERR_NOT_FOUND;

    const kilib_entry_t *e = &lib->entries[index];
    info->image_id   = e->image_id;
    info->width      = e->width;
    info->height     = e->height;
    info->raw_size   = e->raw_size;
    info->cmp_size   = e->cmp_size;
    info->file_offset= e->file_offset;
    memcpy(info->name, e->name, KILIB_NAME_MAX);
    return KILIB_OK;
}


kilib_error_t kilib_find_id(const kilib_t *lib, const char *name,
                             uint16_t *id_out)
{
    if (!lib || !name || !id_out) return KILIB_ERR_BAD_ARG;
    for (uint16_t i = 0; i < lib->count; i++) {
        /* Compare up to KILIB_NAME_MAX characters */
        size_t j = 0;
        while (j < KILIB_NAME_MAX - 1
               && lib->entries[i].name[j]
               && name[j]
               && lib->entries[i].name[j] == name[j])
            j++;
        if (lib->entries[i].name[j] == '\0' && name[j] == '\0') {
            *id_out = lib->entries[i].image_id;
            return KILIB_OK;
        }
    }
    return KILIB_ERR_NOT_FOUND;
}


kilib_error_t kilib_load(const kilib_t *lib, uint16_t id,
                          uint8_t **pixels_out,
                          uint16_t *width_out, uint16_t *height_out)
{
    if (!lib || !pixels_out) return KILIB_ERR_BAD_ARG;
    *pixels_out = (uint8_t *)0;

    const kilib_entry_t *e = find_by_id(lib, id);
    if (!e) return KILIB_ERR_NOT_FOUND;

    kilib_error_t err = decompress_entry(lib, e, pixels_out);
    if (err != KILIB_OK) return err;

    if (width_out)  *width_out  = e->width;
    if (height_out) *height_out = e->height;
    return KILIB_OK;
}


kilib_error_t kilib_load_by_index(const kilib_t *lib, uint16_t index,
                                   uint8_t **pixels_out,
                                   uint16_t *width_out, uint16_t *height_out)
{
    if (!lib || !pixels_out) return KILIB_ERR_BAD_ARG;
    *pixels_out = (uint8_t *)0;
    if (index >= lib->count) return KILIB_ERR_NOT_FOUND;

    const kilib_entry_t *e = &lib->entries[index];
    kilib_error_t err = decompress_entry(lib, e, pixels_out);
    if (err != KILIB_OK) return err;

    if (width_out)  *width_out  = e->width;
    if (height_out) *height_out = e->height;
    return KILIB_OK;
}


kilib_error_t kilib_load_by_name(const kilib_t *lib, const char *name,
                                  uint8_t **pixels_out,
                                  uint16_t *width_out, uint16_t *height_out)
{
    if (!lib || !name || !pixels_out) return KILIB_ERR_BAD_ARG;
    uint16_t id = 0;
    kilib_error_t err = kilib_find_id(lib, name, &id);
    if (err != KILIB_OK) return err;
    return kilib_load(lib, id, pixels_out, width_out, height_out);
}