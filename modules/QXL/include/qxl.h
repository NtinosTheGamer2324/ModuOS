/*
 * QXL Main Header - Include all QXL subsystem headers
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
#ifndef QXL_H
#define QXL_H

#include "qxl_dev.h"
#include "qxl_cmd.h"
#include "qxl_draw.h"
#include "qxl_cursor.h"
#include "qxl_surface.h"
#include "qxl_ring.h"
#include "qxl_mem.h"
#include "qxl_monitors.h"
#include "qxl_escape.h"

/* Version information */
#define QXL_DRIVER_VERSION_MAJOR 0
#define QXL_DRIVER_VERSION_MINOR 1
#define QXL_DRIVER_VERSION_PATCH 0

/* Feature capabilities */
#define QXL_CAP_RESIZE              0
#define QXL_CAP_MULTIPLE_MONITORS   1
#define QXL_CAP_NAMED_SURFACES      2
#define QXL_CAP_MONITORS_CONFIG     3
#define QXL_CAP_COMPOSITE           4
#define QXL_CAP_GL_SCANOUT          5

/* Client capabilities (mirrored from ROM) */
#define QXL_CLIENT_CAP_MULTI_MONITOR 0

/* Helper macros */
#define QXL_GET_ADDRESS(obj) \
    ((uint64_t)(uintptr_t)(obj))

#define QXL_ALIGN(val, align) \
    (((val) + ((align) - 1)) & ~((align) - 1))

#define QXL_INTERRUPT_MASK \
    (QXL_INTERRUPT_DISPLAY | \
     QXL_INTERRUPT_CURSOR  | \
     QXL_INTERRUPT_IO_CMD  | \
     QXL_INTERRUPT_ERROR   | \
     QXL_INTERRUPT_CLIENT  | \
     QXL_INTERRUPT_CLIENT_MONITORS_CONFIG)

#endif /* QXL_H */