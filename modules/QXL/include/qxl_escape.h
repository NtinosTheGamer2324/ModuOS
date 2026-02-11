/*
 * QXL Escape Codes and Monitor Control
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

#ifndef QXL_ESCAPE_H
#define QXL_ESCAPE_H

#include <stdint.h>

/* Escape codes */
enum {
    QXL_ESCAPE_SET_CUSTOM_DISPLAY = 0x10001
};

typedef struct QXLEscape {
    uint64_t release_info;
    uint32_t code;
    uint32_t data_size;
    uint8_t data[0];
} QXLEscape;

#endif /* QXL_ESCAPE_H */