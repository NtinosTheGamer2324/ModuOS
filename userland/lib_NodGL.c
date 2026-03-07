// NodGL - ModuOS Advanced Graphics API Implementation
// A modern graphics library for ModuOS
//
// LEGAL NOTICE:
// This is an independent, clean-room implementation using common graphics
// programming patterns. No proprietary code or algorithms were used.
//
// Copyright © 2025-2026 ModuOS Project Contributors
// Licensed under GPL v2.0 - See LICENSE.md

#include "NodGL.h"
#include "gfx2d.h"
#include "libc.h"

#define MAX_RESOURCES 256
#define MAX_MAPPED_RESOURCES 32

typedef struct {
    uint32_t in_use;
    uint32_t handle;
    NodGL_Format format;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    void *mapped_addr;
} NodGL_resource_entry_t;

struct NodGL_device {
    gfx2d_t gfx2d;
    NodGL_DeviceCaps caps;
    NodGL_FeatureLevel feature_level;
    NodGL_resource_entry_t resources[MAX_RESOURCES];
    uint32_t next_resource_id;
};

struct NodGL_context {
    NodGL_Device device;
    NodGL_Viewport viewport;
    NodGL_Rect scissor;
    NodGL_BlendMode blend_mode;
    NodGL_Topology topology;
    uint32_t clear_color;
};

const char* NodGL_GetErrorString(int error_code) {
    switch (error_code) {
        case NodGL_OK: return "Success";
        case NodGL_ERROR_INVALID_ARGS: return "Invalid arguments";
        case NodGL_ERROR_NO_DEVICE: return "No graphics device available";
        case NodGL_ERROR_OUT_OF_MEMORY: return "Out of memory";
        case NodGL_ERROR_UNSUPPORTED: return "Operation not supported";
        case NodGL_ERROR_DEVICE_LOST: return "Graphics device lost";
        default: return "Unknown error";
    }
}

uint32_t NodGL_ColorARGB(uint8_t a, uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

static NodGL_resource_entry_t* NodGL_find_resource(NodGL_Device device, NodGL_Resource id) {
    if (!device) return NULL;
    for (uint32_t i = 0; i < MAX_RESOURCES; i++) {
        if (device->resources[i].in_use && device->resources[i].handle == id) {
            return &device->resources[i];
        }
    }
    return NULL;
}

static NodGL_resource_entry_t* NodGL_alloc_resource_slot(NodGL_Device device) {
    if (!device) return NULL;
    for (uint32_t i = 0; i < MAX_RESOURCES; i++) {
        if (!device->resources[i].in_use) {
            memset(&device->resources[i], 0, sizeof(NodGL_resource_entry_t));
            device->resources[i].in_use = 1;
            device->resources[i].handle = ++device->next_resource_id;
            return &device->resources[i];
        }
    }
    return NULL;
}

int NodGL_CreateDevice(
    NodGL_FeatureLevel min_feature_level,
    NodGL_Device *out_device,
    NodGL_Context *out_context,
    NodGL_FeatureLevel *out_actual_level
) {
    if (!out_device || !out_context) return NodGL_ERROR_INVALID_ARGS;

    NodGL_Device device = (NodGL_Device)malloc(sizeof(struct NodGL_device));
    if (!device) return NodGL_ERROR_OUT_OF_MEMORY;

    memset(device, 0, sizeof(struct NodGL_device));

    if (gfx2d_open(&device->gfx2d) != 0) {
        free(device);
        return NodGL_ERROR_NO_DEVICE;
    }

    gfx2d_info_t info;
    if (gfx2d_get_info(&device->gfx2d, &info) != 0) {
        gfx2d_close(&device->gfx2d);
        free(device);
        return NodGL_ERROR_NO_DEVICE;
    }

    device->caps.capabilities = 0;
    if (info.caps & VIDEOCTL2_CAP_ENQUEUE_FILL_RECT) {
        device->caps.capabilities |= NodGL_CAP_HARDWARE_ACCEL;
    }
    if (info.caps & VIDEOCTL2_CAP_ENQUEUE_BLIT_BUF) {
        device->caps.capabilities |= NodGL_CAP_ALPHA_BLEND;
    }

    device->caps.max_texture_width = 4096;
    device->caps.max_texture_height = 4096;
    device->caps.screen_width = info.width;
    device->caps.screen_height = info.height;
    device->caps.max_buffers = MAX_RESOURCES;
    device->caps.video_memory_mb = 64;
    
    for (uint32_t i = 0; i < 63 && info.driver[i]; i++) {
        device->caps.adapter_name[i] = info.driver[i];
    }
    device->caps.adapter_name[63] = 0;

    if (device->caps.capabilities & NodGL_CAP_HARDWARE_ACCEL) {
        device->feature_level = NodGL_FEATURE_LEVEL_2_0;
    } else {
        device->feature_level = NodGL_FEATURE_LEVEL_1_0;
    }

    if (device->feature_level < min_feature_level) {
        gfx2d_close(&device->gfx2d);
        free(device);
        return NodGL_ERROR_UNSUPPORTED;
    }

    device->caps.feature_level = device->feature_level;

    NodGL_Context ctx = (NodGL_Context)malloc(sizeof(struct NodGL_context));
    if (!ctx) {
        gfx2d_close(&device->gfx2d);
        free(device);
        return NodGL_ERROR_OUT_OF_MEMORY;
    }

    memset(ctx, 0, sizeof(struct NodGL_context));
    ctx->device = device;
    ctx->viewport.x = 0;
    ctx->viewport.y = 0;
    ctx->viewport.width = (float)info.width;
    ctx->viewport.height = (float)info.height;
    ctx->viewport.min_depth = 0.0f;
    ctx->viewport.max_depth = 1.0f;
    ctx->scissor.x = 0;
    ctx->scissor.y = 0;
    ctx->scissor.width = info.width;
    ctx->scissor.height = info.height;
    ctx->blend_mode = NodGL_BLEND_NONE;
    ctx->topology = NodGL_TOPOLOGY_TRIANGLES;
    ctx->clear_color = 0xFF000000;

    *out_device = device;
    *out_context = ctx;
    if (out_actual_level) *out_actual_level = device->feature_level;

    return NodGL_OK;
}

void NodGL_ReleaseDevice(NodGL_Device device) {
    if (!device) return;
    
    for (uint32_t i = 0; i < MAX_RESOURCES; i++) {
        if (device->resources[i].in_use) {
            device->resources[i].in_use = 0;
        }
    }
    
    gfx2d_close(&device->gfx2d);
    free(device);
}

int NodGL_GetDeviceCaps(NodGL_Device device, NodGL_DeviceCaps *out_caps) {
    if (!device || !out_caps) return NodGL_ERROR_INVALID_ARGS;
    *out_caps = device->caps;
    return NodGL_OK;
}

int NodGL_GetScreenResolution(NodGL_Device device, uint32_t *out_width, uint32_t *out_height) {
    if (!device || !out_width || !out_height) return NodGL_ERROR_INVALID_ARGS;
    *out_width = device->caps.screen_width;
    *out_height = device->caps.screen_height;
    return NodGL_OK;
}

int NodGL_PresentContext(NodGL_Context ctx, uint32_t sync_interval) {
    if (!ctx || !ctx->device) return NodGL_ERROR_INVALID_ARGS;
    
    (void)sync_interval;
    
    int result = gfx2d_flush(&ctx->device->gfx2d, 0, 0, 0, 0);
    return (result == 0) ? NodGL_OK : NodGL_ERROR_DEVICE_LOST;
}

int NodGL_CreateTexture(
    NodGL_Device device,
    const NodGL_TextureDesc *desc,
    NodGL_Texture *out_texture
) {
    if (!device || !desc || !out_texture) return NodGL_ERROR_INVALID_ARGS;
    if (desc->width == 0 || desc->height == 0) return NodGL_ERROR_INVALID_ARGS;

    NodGL_resource_entry_t *res = NodGL_alloc_resource_slot(device);
    if (!res) return NodGL_ERROR_OUT_OF_MEMORY;

    uint32_t fmt = 0;
    switch (desc->format) {
        case NodGL_FORMAT_R8G8B8A8_UNORM: fmt = 1; break;
        case NodGL_FORMAT_B8G8R8A8_UNORM: fmt = 2; break;
        case NodGL_FORMAT_R5G6B5_UNORM: fmt = 3; break;
        default: fmt = 1; break;
    }

    uint32_t gfx_handle = 0;
    uint32_t pitch = 0;
    uint32_t size = desc->width * desc->height * 4;

    int result = gfx2d_alloc_buf(&device->gfx2d, size, fmt, &gfx_handle, &pitch);
    if (result != 0) {
        res->in_use = 0;
        return NodGL_ERROR_OUT_OF_MEMORY;
    }

    res->handle = gfx_handle;
    res->format = desc->format;
    res->width = desc->width;
    res->height = desc->height;
    res->pitch = pitch;
    res->mapped_addr = NULL;

    if (desc->initial_data && desc->initial_data_size > 0) {
        void *mapped = NULL;
        uint32_t mapped_size = 0, mapped_pitch = 0, mapped_fmt = 0;
        if (gfx2d_map_buf(&device->gfx2d, gfx_handle, &mapped, &mapped_size, &mapped_pitch, &mapped_fmt) == 0) {
            uint32_t copy_size = desc->initial_data_size;
            if (copy_size > mapped_size) copy_size = mapped_size;
            memcpy(mapped, desc->initial_data, copy_size);
        }
    }

    *out_texture = res->handle;
    return NodGL_OK;
}

int NodGL_CreateBuffer(
    NodGL_Device device,
    const NodGL_BufferDesc *desc,
    NodGL_Buffer *out_buffer
) {
    if (!device || !desc || !out_buffer) return NodGL_ERROR_INVALID_ARGS;
    if (desc->size_bytes == 0) return NodGL_ERROR_INVALID_ARGS;

    NodGL_resource_entry_t *res = NodGL_alloc_resource_slot(device);
    if (!res) return NodGL_ERROR_OUT_OF_MEMORY;

    uint32_t gfx_handle = 0;
    uint32_t pitch = 0;

    int result = gfx2d_alloc_buf(&device->gfx2d, desc->size_bytes, 1, &gfx_handle, &pitch);
    if (result != 0) {
        res->in_use = 0;
        return NodGL_ERROR_OUT_OF_MEMORY;
    }

    res->handle = gfx_handle;
    res->format = desc->format;
    res->width = 0;
    res->height = 0;
    res->pitch = pitch;
    res->mapped_addr = NULL;

    *out_buffer = res->handle;
    return NodGL_OK;
}

int NodGL_MapResource(
    NodGL_Context ctx,
    NodGL_Resource resource,
    void **out_data,
    uint32_t *out_pitch
) {
    if (!ctx || !ctx->device || !out_data) return NodGL_ERROR_INVALID_ARGS;

    NodGL_resource_entry_t *res = NodGL_find_resource(ctx->device, resource);
    if (!res) return NodGL_ERROR_INVALID_ARGS;

    if (res->mapped_addr) {
        *out_data = res->mapped_addr;
        if (out_pitch) *out_pitch = res->pitch;
        return NodGL_OK;
    }

    void *mapped = NULL;
    uint32_t size = 0, pitch = 0, fmt = 0;
    int result = gfx2d_map_buf(&ctx->device->gfx2d, res->handle, &mapped, &size, &pitch, &fmt);
    if (result != 0) return NodGL_ERROR_DEVICE_LOST;

    res->mapped_addr = mapped;
    res->pitch = pitch;
    *out_data = mapped;
    if (out_pitch) *out_pitch = pitch;

    return NodGL_OK;
}

void NodGL_UnmapResource(NodGL_Context ctx, NodGL_Resource resource) {
    if (!ctx || !ctx->device) return;

    NodGL_resource_entry_t *res = NodGL_find_resource(ctx->device, resource);
    if (!res) return;

    res->mapped_addr = NULL;
}

void NodGL_ReleaseResource(NodGL_Device device, NodGL_Resource resource) {
    if (!device) return;

    NodGL_resource_entry_t *res = NodGL_find_resource(device, resource);
    if (!res) return;

    res->in_use = 0;
    res->mapped_addr = NULL;
}

int NodGL_ClearContext(
    NodGL_Context ctx,
    uint32_t flags,
    uint32_t color,
    float depth,
    uint8_t stencil
) {
    if (!ctx || !ctx->device) return NodGL_ERROR_INVALID_ARGS;

    (void)depth;
    (void)stencil;

    if (flags & NodGL_CLEAR_COLOR) {
        ctx->clear_color = color;
        uint32_t w = (uint32_t)ctx->viewport.width;
        uint32_t h = (uint32_t)ctx->viewport.height;
        int result = gfx2d_fill_rect(&ctx->device->gfx2d, 0, 0, w, h, color);
        if (result != 0) return NodGL_ERROR_DEVICE_LOST;
    }

    return NodGL_OK;
}

int NodGL_SetViewport(NodGL_Context ctx, const NodGL_Viewport *viewport) {
    if (!ctx || !viewport) return NodGL_ERROR_INVALID_ARGS;
    ctx->viewport = *viewport;
    return NodGL_OK;
}

int NodGL_SetScissorRect(NodGL_Context ctx, const NodGL_Rect *rect) {
    if (!ctx || !rect) return NodGL_ERROR_INVALID_ARGS;
    ctx->scissor = *rect;
    return NodGL_OK;
}

int NodGL_SetBlendMode(NodGL_Context ctx, NodGL_BlendMode mode) {
    if (!ctx) return NodGL_ERROR_INVALID_ARGS;
    ctx->blend_mode = mode;
    return NodGL_OK;
}

int NodGL_FillRectContext(
    NodGL_Context ctx,
    int32_t x, int32_t y,
    uint32_t width, uint32_t height,
    uint32_t color
) {
    if (!ctx || !ctx->device) return NodGL_ERROR_INVALID_ARGS;

    int result = gfx2d_fill_rect(&ctx->device->gfx2d, (uint32_t)x, (uint32_t)y, width, height, color);
    return (result == 0) ? NodGL_OK : NodGL_ERROR_DEVICE_LOST;
}

int NodGL_DrawTexture(
    NodGL_Context ctx,
    NodGL_Texture texture,
    int32_t src_x, int32_t src_y,
    int32_t dst_x, int32_t dst_y,
    uint32_t width, uint32_t height
) {
    if (!ctx || !ctx->device) return NodGL_ERROR_INVALID_ARGS;

    NodGL_resource_entry_t *res = NodGL_find_resource(ctx->device, texture);
    if (!res) return NodGL_ERROR_INVALID_ARGS;

    uint32_t fmt = 1;
    switch (res->format) {
        case NodGL_FORMAT_R8G8B8A8_UNORM: fmt = 1; break;
        case NodGL_FORMAT_B8G8R8A8_UNORM: fmt = 2; break;
        case NodGL_FORMAT_R5G6B5_UNORM: fmt = 3; break;
        default: fmt = 1; break;
    }

    int result = gfx2d_blit_buf(
        &ctx->device->gfx2d,
        res->handle,
        (uint32_t)src_x, (uint32_t)src_y,
        (uint32_t)dst_x, (uint32_t)dst_y,
        width, height,
        res->pitch, fmt
    );

    return (result == 0) ? NodGL_OK : NodGL_ERROR_DEVICE_LOST;
}

int NodGL_DrawLineContext(
    NodGL_Context ctx,
    int32_t x0, int32_t y0,
    int32_t x1, int32_t y1,
    uint32_t color,
    uint32_t thickness
) {
    if (!ctx || !ctx->device) return NodGL_ERROR_INVALID_ARGS;

    if (thickness == 0) thickness = 1;

    int dx = x1 - x0;
    int dy = y1 - y0;
    
    // Check for horizontal or vertical lines - draw with single rect (FAST!)
    if (dy == 0) {
        // Horizontal line
        int x_min = (x0 < x1) ? x0 : x1;
        int width = (dx < 0) ? -dx : dx;
        return gfx2d_fill_rect(&ctx->device->gfx2d, x_min, y0, width + 1, thickness, color);
    }
    if (dx == 0) {
        // Vertical line
        int y_min = (y0 < y1) ? y0 : y1;
        int height = (dy < 0) ? -dy : dy;
        return gfx2d_fill_rect(&ctx->device->gfx2d, x0, y_min, thickness, height + 1, color);
    }
    
    // Diagonal lines - use optimized Bresenham with horizontal span batching
    int abs_dx = (dx < 0) ? -dx : dx;
    int abs_dy = (dy < 0) ? -dy : dy;
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    
    // Batch consecutive pixels on same scanline
    int x = x0;
    int y = y0;
    int err = abs_dx - abs_dy;
    
    int span_start_x = x;
    int span_y = y;
    int span_len = 0;
    
    while (1) {
        if (y == span_y) {
            // Extend current span
            if (span_len == 0) span_start_x = x;
            span_len++;
        } else {
            // Draw accumulated span and start new one
            if (span_len > 0) {
                int draw_x = (span_len == 1) ? span_start_x : ((span_start_x < x) ? span_start_x : x);
                gfx2d_fill_rect(&ctx->device->gfx2d, draw_x, span_y, span_len, thickness, color);
            }
            span_start_x = x;
            span_y = y;
            span_len = 1;
        }
        
        if (x == x1 && y == y1) break;
        
        int e2 = err * 2;
        if (e2 > -abs_dy) {
            err -= abs_dy;
            x += sx;
        }
        if (e2 < abs_dx) {
            err += abs_dx;
            y += sy;
        }
    }
    
    // Draw final span
    if (span_len > 0) {
        int draw_x = span_start_x;
        if (sx < 0 && span_len > 1) draw_x = x;
        gfx2d_fill_rect(&ctx->device->gfx2d, draw_x, span_y, span_len, thickness, color);
    }
    
    return NodGL_OK;
}

int NodGL_DrawSprite(
    NodGL_Context ctx,
    NodGL_Texture texture,
    float x, float y,
    float scale_x, float scale_y,
    float rotation,
    uint32_t tint
) {
    if (!ctx || !ctx->device) return NodGL_ERROR_INVALID_ARGS;

    (void)rotation;
    (void)tint;

    NodGL_resource_entry_t *res = NodGL_find_resource(ctx->device, texture);
    if (!res) return NodGL_ERROR_INVALID_ARGS;

    uint32_t width = (uint32_t)((float)res->width * scale_x);
    uint32_t height = (uint32_t)((float)res->height * scale_y);

    return NodGL_DrawTexture(ctx, texture, 0, 0, (int32_t)x, (int32_t)y, width, height);
}

int NodGL_SetTopology(NodGL_Context ctx, NodGL_Topology topology) {
    if (!ctx) return NodGL_ERROR_INVALID_ARGS;
    ctx->topology = topology;
    return NodGL_OK;
}

int NodGL_Draw(NodGL_Context ctx, uint32_t vertex_count, uint32_t start_vertex) {
    if (!ctx || !ctx->device) return NodGL_ERROR_INVALID_ARGS;
    (void)vertex_count;
    (void)start_vertex;
    return NodGL_ERROR_UNSUPPORTED;
}

int NodGL_DrawIndexed(
    NodGL_Context ctx,
    uint32_t index_count,
    uint32_t start_index,
    int32_t base_vertex
) {
    if (!ctx || !ctx->device) return NodGL_ERROR_INVALID_ARGS;
    (void)index_count;
    (void)start_index;
    (void)base_vertex;
    return NodGL_ERROR_UNSUPPORTED;
}

int NodGL_BindVertexBuffer(
    NodGL_Context ctx,
    NodGL_Buffer buffer,
    uint32_t stride,
    uint32_t offset
) {
    if (!ctx || !ctx->device) return NodGL_ERROR_INVALID_ARGS;
    (void)buffer;
    (void)stride;
    (void)offset;
    return NodGL_ERROR_UNSUPPORTED;
}