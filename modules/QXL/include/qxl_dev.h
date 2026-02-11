/*
 * QXL Device Interface Header
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

#ifndef QXL_DEV_H
#define QXL_DEV_H

#include <stdint.h>

// Needed for struct members in QXLRam
#include "qxl_draw.h"    // QXLRect
#include "qxl_surface.h" // QXLSurfaceCreate

#define QXL_DEVICE_ID_DEVEL 0x01ff
#define QXL_DEVICE_ID_STABLE 0x0100

#define QXL_REVISION_DEVEL 0x01
#define QXL_REVISION_STABLE 0x0c

#define QXL_ROM_MAGIC (*(uint32_t*)"QXRO")
#define QXL_RAM_MAGIC (*(uint32_t*)"QXRA")

/* PCI BAR Indices */
#define QXL_IO_RANGE_INDEX      0
#define QXL_ROM_RANGE_INDEX     1
#define QXL_RAM_RANGE_INDEX     2
#define QXL_VRAM_RANGE_INDEX    3

/* Interrupt flags */
#define QXL_INTERRUPT_DISPLAY (1 << 0)
#define QXL_INTERRUPT_CURSOR  (1 << 1)
#define QXL_INTERRUPT_IO_CMD  (1 << 2)
#define QXL_INTERRUPT_ERROR   (1 << 3)
#define QXL_INTERRUPT_CLIENT  (1 << 4)
#define QXL_INTERRUPT_CLIENT_MONITORS_CONFIG (1 << 5)

/* IO Port commands */
enum {
    QXL_IO_NOTIFY_CMD = 0,
    QXL_IO_NOTIFY_CURSOR,
    QXL_IO_UPDATE_AREA,
    QXL_IO_UPDATE_IRQ,
    QXL_IO_NOTIFY_OOM,
    QXL_IO_RESET,
    QXL_IO_SET_MODE,
    QXL_IO_LOG,
    QXL_IO_MEMSLOT_ADD,
    QXL_IO_MEMSLOT_DEL,
    QXL_IO_DETACH_PRIMARY,
    QXL_IO_ATTACH_PRIMARY,
    QXL_IO_CREATE_PRIMARY,
    QXL_IO_DESTROY_PRIMARY,
    QXL_IO_DESTROY_SURFACE_WAIT,
    QXL_IO_DESTROY_ALL_SURFACES,
    QXL_IO_UPDATE_AREA_ASYNC,
    QXL_IO_MEMSLOT_ADD_ASYNC,
    QXL_IO_CREATE_PRIMARY_ASYNC,
    QXL_IO_DESTROY_PRIMARY_ASYNC,
    QXL_IO_DESTROY_SURFACE_ASYNC,
    QXL_IO_DESTROY_ALL_SURFACES_ASYNC,
    QXL_IO_FLUSH_SURFACES_ASYNC,
    QXL_IO_FLUSH_RELEASE,
    QXL_IO_MONITORS_CONFIG_ASYNC,
    QXL_IO_RANGE_SIZE
};

/* Surface formats */
enum {
    QXL_SURF_FMT_INVALID = 0,
    QXL_SURF_FMT_16_555 = 16,
    QXL_SURF_FMT_32_xRGB = 32,
    QXL_SURF_FMT_16_565 = 80,
    QXL_SURF_FMT_8A_RGB = 88,
    QXL_SURF_FMT_8R_GBA = 89,
};

/* Device modes */
enum {
    QXL_MODE_UNDEFINED = 0,
    QXL_MODE_VGA,
    QXL_MODE_COMPAT,
    QXL_MODE_NATIVE
};

typedef struct QXLRom {
    uint32_t magic;
    uint32_t id;
    uint32_t update_id;
    uint32_t compression_level;
    uint32_t log_level;
    uint32_t mode;
    uint32_t modes_offset;
    uint32_t num_pages;
    uint32_t pages_offset;
    uint32_t draw_area_offset;
    uint32_t surface0_area_size;
    uint32_t ram_header_offset;
    uint32_t mm_clock;
    uint32_t n_surfaces;
    uint64_t flags;
    uint8_t slots_start;
    uint8_t slots_end;
    uint8_t slot_gen_bits;
    uint8_t slot_id_bits;
    uint8_t slot_generation;
    uint8_t client_present;
    uint8_t client_capabilities[58];
    uint32_t client_monitors_config_crc;
    struct {
        uint16_t count;
        uint16_t max_allowed;
    } client_monitors_config;
} QXLRom;

typedef struct QXLMode {
    uint32_t id;
    uint32_t x_res;
    uint32_t y_res;
    uint32_t bits;
    uint32_t stride;
    uint32_t x_mili;
    uint32_t y_mili;
    uint32_t orientation;
} QXLMode;

typedef struct QXLRam {
    uint32_t magic;
    uint32_t int_pending;
    uint32_t int_mask;

    /* Command submission rings (guest physical addresses)
     * Minimal subset for draw commands.
     */
    uint64_t cmd_ring;
    uint64_t cursor_ring;
    uint64_t release_ring;

    uint8_t log_buf[4096];
    uint64_t update_area[32];
    uint32_t update_surface;
    struct QXLRect update_area_rect;
    uint32_t create_surface_id;
    struct QXLSurfaceCreate create_surface;
    uint64_t flags;
    uint64_t mem_slot_start;
    uint64_t mem_slot_end;
    uint8_t monitors_config_crc[20];
} QXLRam;

#endif /* QXL_DEV_H */