#pragma once
// NodGL - ModuOS Advanced Graphics API (userland interface)
// A modern graphics library for ModuOS with device/context architecture
//
// LEGAL NOTICE:
// NodGL is an independent, clean-room implementation developed for ModuOS.
// It uses common industry patterns but is not derived from any proprietary API.
// See LICENSE_RDX.md for full legal notice and implementation details.
//
// Copyright © 2025-2026 ModuOS Project Contributors
// Licensed under GPL v2.0 - See LICENSE.md

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handles */
typedef struct NodGL_device* NodGL_Device;
typedef struct NodGL_context* NodGL_Context;
typedef uint32_t NodGL_Resource;
typedef uint32_t NodGL_Texture;
typedef uint32_t NodGL_Buffer;

/* Error codes */
#define NodGL_OK                  0
#define NodGL_ERROR_INVALID_ARGS  -1
#define NodGL_ERROR_NO_DEVICE     -2
#define NodGL_ERROR_OUT_OF_MEMORY -3
#define NodGL_ERROR_UNSUPPORTED   -4
#define NodGL_ERROR_DEVICE_LOST   -5

/* Feature levels (progressive capability enhancement) */
typedef enum {
    NodGL_FEATURE_LEVEL_1_0 = 0x1000,  /* Basic 2D */
    NodGL_FEATURE_LEVEL_2_0 = 0x2000,  /* Hardware accel 2D */
    NodGL_FEATURE_LEVEL_3_0 = 0x3000,  /* 3D pipeline */
} NodGL_FeatureLevel;

/* Capabilities */
#define NodGL_CAP_HARDWARE_ACCEL  (1u << 0)
#define NodGL_CAP_ALPHA_BLEND     (1u << 1)
#define NodGL_CAP_3D_PIPELINE     (1u << 2)
#define NodGL_CAP_SHADER_SUPPORT  (1u << 3)
#define NodGL_CAP_VSYNC           (1u << 4)

/* Formats */
typedef enum {
    NodGL_FORMAT_UNKNOWN = 0,
    NodGL_FORMAT_R8G8B8A8_UNORM = 1,  /* 0xAABBGGRR */
    NodGL_FORMAT_B8G8R8A8_UNORM = 2,  /* 0xAARRGGBB */
    NodGL_FORMAT_R5G6B5_UNORM = 3,
    NodGL_FORMAT_R8_UNORM = 4,
} NodGL_Format;

/* Blend modes */
typedef enum {
    NodGL_BLEND_NONE = 0,
    NodGL_BLEND_ALPHA = 1,
    NodGL_BLEND_ADDITIVE = 2,
    NodGL_BLEND_MODULATE = 3,
} NodGL_BlendMode;

/* Primitive topology */
typedef enum {
    NodGL_TOPOLOGY_POINTS = 1,
    NodGL_TOPOLOGY_LINES = 2,
    NodGL_TOPOLOGY_TRIANGLES = 3,
} NodGL_Topology;

/* Clear flags */
#define NodGL_CLEAR_COLOR   (1u << 0)
#define NodGL_CLEAR_DEPTH   (1u << 1)
#define NodGL_CLEAR_STENCIL (1u << 2)

/* Structures */
typedef struct {
    uint32_t capabilities;
    uint32_t max_texture_width;
    uint32_t max_texture_height;
    uint32_t max_buffers;
    uint32_t screen_width;
    uint32_t screen_height;
    char adapter_name[64];
    uint32_t video_memory_mb;
    NodGL_FeatureLevel feature_level;
} NodGL_DeviceCaps;

typedef struct {
    float x;
    float y;
    float width;
    float height;
    float min_depth;
    float max_depth;
} NodGL_Viewport;

typedef struct {
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
} NodGL_Rect;

typedef struct {
    float x, y, z;
    float u, v;
    uint32_t color;
} NodGL_Vertex;

typedef struct {
    uint32_t width;
    uint32_t height;
    NodGL_Format format;
    uint32_t mip_levels;
    void *initial_data;
    uint32_t initial_data_size;
} NodGL_TextureDesc;

typedef struct {
    uint32_t size_bytes;
    NodGL_Format format;
    void *initial_data;
} NodGL_BufferDesc;

/* ============================================================
 * Device & Context Management
 * ============================================================ */

/* Create a NodGL device and immediate context */
int NodGL_CreateDevice(
    NodGL_FeatureLevel min_feature_level,
    NodGL_Device *out_device,
    NodGL_Context *out_context,
    NodGL_FeatureLevel *out_actual_level
);

/* Release device and all resources */
void NodGL_ReleaseDevice(NodGL_Device device);

/* Get device capabilities */
int NodGL_GetDeviceCaps(NodGL_Device device, NodGL_DeviceCaps *out_caps);
int NodGL_GetScreenResolution(NodGL_Device device, uint32_t *out_width, uint32_t *out_height);

/* Present backbuffer to screen (context API) */
int NodGL_PresentContext(NodGL_Context ctx, uint32_t sync_interval);

/* ============================================================
 * Simple Global API (for simple apps like ttyman)
 * ============================================================ */

/* Initialize NodGL with default device */
int NodGL_Init(void);

/* Shutdown NodGL */
void NodGL_Shutdown(void);

/* Clear screen (simple API) */
void NodGL_Clear(uint32_t color);

/* Fill rectangle (simple API) */
void NodGL_FillRect(int x, int y, uint32_t width, uint32_t height, uint32_t color);

/* Draw line (simple API) */
void NodGL_DrawLine(int x0, int y0, int x1, int y1, uint32_t color);

/* Present frame (simple API) */
void NodGL_Present(void);

/* ============================================================
 * Resource Management
 * ============================================================ */

/* Create texture resource */
int NodGL_CreateTexture(
    NodGL_Device device,
    const NodGL_TextureDesc *desc,
    NodGL_Texture *out_texture
);

/* Create buffer resource */
int NodGL_CreateBuffer(
    NodGL_Device device,
    const NodGL_BufferDesc *desc,
    NodGL_Buffer *out_buffer
);

/* Map resource for CPU access */
int NodGL_MapResource(
    NodGL_Context ctx,
    NodGL_Resource resource,
    void **out_data,
    uint32_t *out_pitch
);

/* Unmap resource */
void NodGL_UnmapResource(NodGL_Context ctx, NodGL_Resource resource);

/* Release resource */
void NodGL_ReleaseResource(NodGL_Device device, NodGL_Resource resource);

/* ============================================================
 * Rendering Commands (Context API)
 * ============================================================ */

/* Clear render target */
int NodGL_ClearContext(
    NodGL_Context ctx,
    uint32_t flags,
    uint32_t color,
    float depth,
    uint8_t stencil
);

/* Set viewport */
int NodGL_SetViewport(NodGL_Context ctx, const NodGL_Viewport *viewport);

/* Set scissor rectangle */
int NodGL_SetScissorRect(NodGL_Context ctx, const NodGL_Rect *rect);

/* Set blend mode */
int NodGL_SetBlendMode(NodGL_Context ctx, NodGL_BlendMode mode);

/* ============================================================
 * 2D Drawing API (Context API)
 * ============================================================ */

/* Fill rectangle with solid color */
int NodGL_FillRectContext(
    NodGL_Context ctx,
    int32_t x, int32_t y,
    uint32_t width, uint32_t height,
    uint32_t color
);

/* Draw textured rectangle (blit) */
int NodGL_DrawTexture(
    NodGL_Context ctx,
    NodGL_Texture texture,
    int32_t src_x, int32_t src_y,
    int32_t dst_x, int32_t dst_y,
    uint32_t width, uint32_t height
);

/* Draw line */
int NodGL_DrawLineContext(
    NodGL_Context ctx,
    int32_t x0, int32_t y0,
    int32_t x1, int32_t y1,
    uint32_t color,
    uint32_t thickness
);

/* Draw transformed sprite with rotation/scale */
int NodGL_DrawSprite(
    NodGL_Context ctx,
    NodGL_Texture texture,
    float x, float y,
    float scale_x, float scale_y,
    float rotation,
    uint32_t tint
);

/* ============================================================
 * 3D Rendering (Future expansion)
 * ============================================================ */

/* Set primitive topology */
int NodGL_SetTopology(NodGL_Context ctx, NodGL_Topology topology);

/* Draw primitives from vertex buffer */
int NodGL_Draw(
    NodGL_Context ctx,
    uint32_t vertex_count,
    uint32_t start_vertex
);

/* Draw indexed primitives */
int NodGL_DrawIndexed(
    NodGL_Context ctx,
    uint32_t index_count,
    uint32_t start_index,
    int32_t base_vertex
);

/* Bind vertex buffer */
int NodGL_BindVertexBuffer(
    NodGL_Context ctx,
    NodGL_Buffer buffer,
    uint32_t stride,
    uint32_t offset
);

/* ============================================================
 * Utility Functions
 * ============================================================ */

/* Get error string */
const char* NodGL_GetErrorString(int error_code);

/* Convert ARGB to device-native format */
uint32_t NodGL_ColorARGB(uint8_t a, uint8_t r, uint8_t g, uint8_t b);

#ifdef __cplusplus
}
#endif


