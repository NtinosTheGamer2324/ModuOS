/*
 * QXL Cursor Definitions and Structures
 *
 * Copyright (c) 2026 NTSoftware
 *
 * These headers were reverse-engineered and implemented based on
 * incomplete documentation and device behavior.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the "Software"),
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, under the condition that the above copyright notice
 * and this permission notice are included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.
 *
 * Authorship: NTSoftware
 */

 #ifndef QXL_CMD_H
#define QXL_CMD_H

#include <stdint.h>

/* Command types */
enum {
    QXL_CMD_NOP = 0,
    QXL_CMD_DRAW,
    QXL_CMD_UPDATE,
    QXL_CMD_CURSOR,
    QXL_CMD_MESSAGE,
    QXL_CMD_SURFACE
};

/* Drawing command types */
enum {
    QXL_DRAW_NOP = 0,
    QXL_DRAW_FILL,
    QXL_DRAW_OPAQUE,
    QXL_DRAW_COPY,
    QXL_DRAW_TRANSPARENT,
    QXL_DRAW_ALPHA_BLEND,
    QXL_DRAW_COPY_BITS,
    QXL_DRAW_BLEND,
    QXL_DRAW_BLACKNESS,
    QXL_DRAW_WHITENESS,
    QXL_DRAW_INVERS,
    QXL_DRAW_ROP3,
    QXL_DRAW_STROKE,
    QXL_DRAW_TEXT,
    QXL_DRAW_MASK,
    QXL_DRAW_COMPOSITE
};

/* Cursor command types */
enum {
    QXL_CURSOR_SET = 0,
    QXL_CURSOR_MOVE,
    QXL_CURSOR_HIDE,
    QXL_CURSOR_TRAIL
};

/* Surface command types */
enum {
    QXL_SURFACE_CMD_CREATE = 0,
    QXL_SURFACE_CMD_DESTROY
};

/* Bitmap formats */
enum {
    QXL_BITMAP_FMT_INVALID = 0,
    QXL_BITMAP_FMT_1BIT_LE,
    QXL_BITMAP_FMT_1BIT_BE,
    QXL_BITMAP_FMT_4BIT_LE,
    QXL_BITMAP_FMT_4BIT_BE,
    QXL_BITMAP_FMT_8BIT,
    QXL_BITMAP_FMT_16BIT,
    QXL_BITMAP_FMT_24BIT,
    QXL_BITMAP_FMT_32BIT,
    QXL_BITMAP_FMT_RGBA
};

/* Bitmap flags */
#define QXL_BITMAP_DIRECT     (1 << 0)
#define QXL_BITMAP_TOP_DOWN   (1 << 1)
#define QXL_BITMAP_UNSTABLE   (1 << 2)

/* ROP descriptors */
enum {
    QXL_ROP_COPY = 0xCC,
    QXL_ROP_XOR  = 0x5A,
    QXL_ROP_AND  = 0x88,
    QXL_ROP_OR   = 0xEE
};

/* Image types */
enum {
    QXL_IMAGE_TYPE_BITMAP = 0,
    QXL_IMAGE_TYPE_QUIC,
    QXL_IMAGE_TYPE_LZ_RGB,
    QXL_IMAGE_TYPE_GLZ_RGB,
    QXL_IMAGE_TYPE_FROM_CACHE,
    QXL_IMAGE_TYPE_SURFACE,
    QXL_IMAGE_TYPE_JPEG,
    QXL_IMAGE_TYPE_FROM_CACHE_LOSSLESS,
    QXL_IMAGE_TYPE_ZLIB_GLZ_RGB,
    QXL_IMAGE_TYPE_JPEG_ALPHA,
    QXL_IMAGE_TYPE_LZ4
};

typedef struct QXLCommand {
    uint64_t data;
    uint32_t type;
    uint32_t padding;
} QXLCommand;

typedef struct QXLCommandRing {
    uint32_t notify_on_prod;
    uint32_t notify_on_cons;
    uint32_t cons;
    uint32_t prod;
} QXLCommandRing;

typedef struct QXLDrawable {
    uint64_t release_info;
    uint32_t surface_id;
    uint8_t effect;
    uint8_t type;
    uint16_t self_bitmap_area;
    struct QXLRect bbox;
    struct QXLClip clip;
    uint32_t mm_time;
    int32_t surfaces_dest[3];
    struct QXLRect surfaces_rects[3];
    union {
        struct QXLFill fill;
        struct QXLOpaque opaque;
        struct QXLCopy copy;
        struct QXLTransparent transparent;
        struct QXLAlphaBlend alpha_blend;
        struct QXLCopyBits copy_bits;
        struct QXLBlend blend;
        struct QXLRop3 rop3;
        struct QXLStroke stroke;
        struct QXLText text;
        struct QXLMask mask;
        struct QXLComposite composite;
    } u;
} QXLDrawable;

typedef struct QXLUpdateCmd {
    uint64_t release_info;
    struct QXLRect area;
    uint32_t update_id;
    uint32_t surface_id;
} QXLUpdateCmd;

typedef struct QXLCursorCmd {
    uint64_t release_info;
    uint8_t type;
    union {
        struct {
            struct QXLPoint position;
            uint8_t visible;
            uint64_t shape;
        } set;
        struct {
            uint16_t length;
            uint16_t frequency;
        } trail;
        struct QXLPoint position;
    } u;
    uint8_t device_data[128];
} QXLCursorCmd;

typedef struct QXLSurfaceCmd {
    uint64_t release_info;
    uint32_t surface_id;
    uint8_t type;
    uint32_t flags;
    union {
        struct QXLSurfaceCreate surface_create;
    } u;
} QXLSurfaceCmd;

typedef struct QXLMessage {
    uint64_t release_info;
    uint64_t data;
    uint32_t len;
} QXLMessage;

typedef struct QXLCompatUpdateCmd {
    uint64_t release_info;
    struct QXLRect area;
    uint32_t update_id;
} QXLCompatUpdateCmd;

typedef struct QXLCompatDrawable {
    uint64_t release_info;
    uint8_t effect;
    uint8_t type;
    uint16_t bitmap_offset;
    struct QXLRect bbox;
    struct QXLClip clip;
    uint32_t mm_time;
    union {
        struct QXLFill fill;
        struct QXLOpaque opaque;
        struct QXLCopy copy;
        struct QXLTransparent transparent;
        struct QXLAlphaBlend alpha_blend;
        struct QXLCopyBits copy_bits;
        struct QXLBlend blend;
        struct QXLRop3 rop3;
        struct QXLStroke stroke;
        struct QXLText text;
        struct QXLMask mask;
    } u;
} QXLCompatDrawable;

typedef struct QXLCompatCursorCmd {
    uint64_t release_info;
    uint8_t type;
    union {
        struct {
            struct QXLPoint position;
            uint8_t visible;
            uint64_t shape;
        } set;
        struct {
            uint16_t length;
            uint16_t frequency;
        } trail;
        struct QXLPoint position;
    } u;
} QXLCompatCursorCmd;

#endif /* QXL_CMD_H */