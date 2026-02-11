/*
 * QXL Drawing Primitives and Structures
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

#ifndef QXL_DRAW_H
#define QXL_DRAW_H

#include <stdint.h>

/* Clip types */
enum {
    QXL_CLIP_TYPE_NONE = 0,
    QXL_CLIP_TYPE_RECTS,
    QXL_CLIP_TYPE_PATH
};

/* Brush types */
enum {
    QXL_BRUSH_TYPE_NONE = 0,
    QXL_BRUSH_TYPE_SOLID,
    QXL_BRUSH_TYPE_PATTERN
};

/* Line styles */
enum {
    QXL_LINE_CAP_ROUND = 0,
    QXL_LINE_CAP_SQUARE,
    QXL_LINE_CAP_BUTT
};

enum {
    QXL_LINE_JOIN_ROUND = 0,
    QXL_LINE_JOIN_BEVEL,
    QXL_LINE_JOIN_MITER
};

/* String flags */
#define QXL_STRING_FLAGS_RASTER_A1     (1 << 0)
#define QXL_STRING_FLAGS_RASTER_A4     (1 << 1)
#define QXL_STRING_FLAGS_RASTER_A8     (1 << 2)
#define QXL_STRING_FLAGS_RASTER_TOP_DOWN (1 << 3)

/* Path flags */
#define QXL_PATH_BEGIN      (1 << 0)
#define QXL_PATH_END        (1 << 1)
#define QXL_PATH_CLOSE      (1 << 3)
#define QXL_PATH_BEZIER     (1 << 4)

typedef struct QXLPoint {
    int32_t x;
    int32_t y;
} QXLPoint;

typedef struct QXLPoint16 {
    int16_t x;
    int16_t y;
} QXLPoint16;

typedef struct QXLRect {
    int32_t top;
    int32_t left;
    int32_t bottom;
    int32_t right;
} QXLRect;

typedef struct QXLClipRects {
    uint32_t num_rects;
    uint64_t chunk;
} QXLClipRects;

typedef struct QXLPath {
    uint64_t data;
} QXLPath;

typedef struct QXLClip {
    uint32_t type;
    union {
        struct QXLClipRects rects;
        struct QXLPath path;
    } data;
} QXLClip;

typedef struct QXLPalette {
    uint64_t unique;
    uint16_t num_ents;
    uint32_t ents[0];
} QXLPalette;

typedef struct QXLBitmap {
    uint8_t format;
    uint8_t flags;
    uint32_t x;
    uint32_t y;
    uint32_t stride;
    uint64_t palette;
    uint64_t data;
} QXLBitmap;

typedef struct QXLQUICData {
    uint32_t data_size;
    uint64_t data;
} QXLQUICData;

typedef struct QXLSurfaceId {
    uint32_t surface_id;
} QXLSurfaceId;

typedef struct QXLImage {
    uint64_t descriptor;
    union {
        struct QXLBitmap bitmap;
        struct QXLQUICData quic;
        struct QXLSurfaceId surface_image;
    } u;
} QXLImage;

typedef struct QXLImageDescriptor {
    uint64_t id;
    uint8_t type;
    uint8_t flags;
    uint32_t width;
    uint32_t height;
} QXLImageDescriptor;

typedef struct QXLBrush {
    uint32_t type;
    union {
        uint32_t color;
        struct {
            uint64_t pattern;
            struct QXLPoint pos;
        } pattern;
    } u;
} QXLBrush;

typedef struct QXLMask {
    uint8_t flags;
    struct QXLPoint pos;
    uint64_t bitmap;
} QXLMask;

typedef struct QXLFill {
    struct QXLBrush brush;
    uint16_t rop_descriptor;
    struct QXLMask mask;
} QXLFill;

typedef struct QXLOpaque {
    uint64_t src_bitmap;
    struct QXLRect src_area;
    struct QXLBrush brush;
    uint16_t rop_descriptor;
    uint8_t scale_mode;
    struct QXLMask mask;
} QXLOpaque;

typedef struct QXLCopy {
    uint64_t src_bitmap;
    struct QXLRect src_area;
    uint16_t rop_descriptor;
    uint8_t scale_mode;
    struct QXLMask mask;
} QXLCopy;

typedef struct QXLTransparent {
    uint64_t src_bitmap;
    struct QXLRect src_area;
    uint32_t src_color;
    uint32_t true_color;
} QXLTransparent;

typedef struct QXLAlphaBlend {
    uint16_t alpha_flags;
    uint8_t alpha;
    uint64_t src_bitmap;
    struct QXLRect src_area;
} QXLAlphaBlend;

typedef struct QXLCopyBits {
    struct QXLPoint src_pos;
} QXLCopyBits;

typedef struct QXLBlend {
    uint64_t src_bitmap;
    struct QXLRect src_area;
    uint16_t rop_descriptor;
    uint8_t scale_mode;
    struct QXLMask mask;
} QXLBlend;

typedef struct QXLRop3 {
    uint64_t src_bitmap;
    struct QXLRect src_area;
    struct QXLBrush brush;
    uint8_t rop3;
    uint8_t scale_mode;
    struct QXLMask mask;
} QXLRop3;

typedef struct QXLLineAttr {
    uint8_t flags;
    uint8_t join_style;
    uint8_t end_style;
    uint8_t style_nseg;
    int32_t width;
    int32_t miter_limit;
    uint64_t style;
} QXLLineAttr;

typedef struct QXLStroke {
    uint64_t path;
    struct QXLLineAttr attr;
    struct QXLBrush brush;
    uint16_t fore_mode;
    uint16_t back_mode;
} QXLStroke;

typedef struct QXLString {
    uint32_t data_size;
    uint16_t length;
    uint16_t flags;
    uint64_t data;
} QXLString;

typedef struct QXLText {
    uint64_t str;
    struct QXLRect back_area;
    struct QXLBrush fore_brush;
    struct QXLBrush back_brush;
    uint16_t fore_mode;
    uint16_t back_mode;
} QXLText;

typedef struct QXLComposite {
    uint32_t flags;
    uint64_t src;
    uint64_t src_transform;
    uint64_t mask;
    uint64_t mask_transform;
    struct QXLPoint16 src_origin;
    struct QXLPoint16 mask_origin;
} QXLComposite;

#endif /* QXL_DRAW_H */