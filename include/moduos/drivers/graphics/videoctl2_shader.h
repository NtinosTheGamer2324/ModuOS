// videoctl2_shader.h - Shader support for videoctl2
// GPU programmable rendering pipeline

#pragma once
#include <stdint.h>
#include "videoctl.h"

/* Shader types */
typedef enum {
    VIDEOCTL2_SHADER_VERTEX = 0,
    VIDEOCTL2_SHADER_FRAGMENT = 1,
    VIDEOCTL2_SHADER_COMPUTE = 2,
} videoctl2_shader_type_t;

/* Shader languages */
typedef enum {
    VIDEOCTL2_SHADER_LANG_GLSL = 0,      /* GLSL (OpenGL-style) */
    VIDEOCTL2_SHADER_LANG_HLSL = 1,      /* HLSL (DirectX-style) */
    VIDEOCTL2_SHADER_LANG_SPIRV = 2,     /* SPIR-V bytecode */
    VIDEOCTL2_SHADER_LANG_MODUOS = 3,    /* ModuOS native shader IR */
} videoctl2_shader_lang_t;

/* Primitive types */
typedef enum {
    VIDEOCTL2_PRIM_TRIANGLES = 0,
    VIDEOCTL2_PRIM_TRIANGLE_STRIP = 1,
    VIDEOCTL2_PRIM_TRIANGLE_FAN = 2,
    VIDEOCTL2_PRIM_LINES = 3,
    VIDEOCTL2_PRIM_LINE_STRIP = 4,
    VIDEOCTL2_PRIM_POINTS = 5,
} videoctl2_primitive_t;

/* Uniform types */
typedef enum {
    VIDEOCTL2_UNIFORM_FLOAT = 0,
    VIDEOCTL2_UNIFORM_VEC2 = 1,
    VIDEOCTL2_UNIFORM_VEC3 = 2,
    VIDEOCTL2_UNIFORM_VEC4 = 3,
    VIDEOCTL2_UNIFORM_INT = 4,
    VIDEOCTL2_UNIFORM_MAT4 = 5,
    VIDEOCTL2_UNIFORM_SAMPLER2D = 6,
} videoctl2_uniform_type_t;

/* ========== Shader Create ========== */
typedef struct {
    videoctl2_hdr_t hdr;
    uint8_t shader_type;        /* videoctl2_shader_type_t */
    uint8_t language;           /* videoctl2_shader_lang_t */
    uint32_t source_len;        /* Length of shader source */
    /* Source code follows */
} videoctl2_shader_create_t;

/* Shader create response */
typedef struct {
    videoctl2_hdr_t hdr;
    uint32_t shader_handle;     /* GPU shader handle */
    int32_t success;            /* 1 = success, 0 = failure */
    char error_log[512];        /* Compilation errors */
} videoctl2_shader_create_resp_t;

/* ========== Program Create/Link ========== */
typedef struct {
    videoctl2_hdr_t hdr;
    uint32_t vertex_shader;     /* Vertex shader handle */
    uint32_t fragment_shader;   /* Fragment shader handle */
} videoctl2_program_create_t;

typedef struct {
    videoctl2_hdr_t hdr;
    uint32_t program_handle;    /* GPU program handle */
    int32_t success;
    char error_log[512];
} videoctl2_program_create_resp_t;

/* ========== Program Use ========== */
typedef struct {
    videoctl2_hdr_t hdr;
    uint32_t program_handle;
} videoctl2_program_use_t;

/* ========== Shader/Program Delete ========== */
typedef struct {
    videoctl2_hdr_t hdr;
    uint32_t handle;            /* Shader or program handle */
} videoctl2_shader_delete_t;

/* ========== Vertex Buffer Create ========== */
typedef struct {
    videoctl2_hdr_t hdr;
    uint32_t size;              /* Size in bytes */
    uint32_t usage;             /* 0=static, 1=dynamic, 2=stream */
    /* Vertex data follows if static */
} videoctl2_vbuf_create_t;

typedef struct {
    videoctl2_hdr_t hdr;
    uint32_t vbuf_handle;       /* GPU buffer handle */
} videoctl2_vbuf_create_resp_t;

/* ========== Vertex Buffer Update ========== */
typedef struct {
    videoctl2_hdr_t hdr;
    uint32_t vbuf_handle;
    uint32_t offset;            /* Offset in buffer */
    uint32_t size;              /* Size to update */
    /* New data follows */
} videoctl2_vbuf_update_t;

/* ========== Vertex Buffer Bind ========== */
typedef struct {
    videoctl2_hdr_t hdr;
    uint32_t vbuf_handle;
} videoctl2_vbuf_bind_t;

/* ========== Set Uniform ========== */
typedef struct {
    videoctl2_hdr_t hdr;
    uint32_t program_handle;
    int32_t location;           /* Uniform location */
    uint8_t type;               /* videoctl2_uniform_type_t */
    uint8_t count;              /* Array count (1 for single) */
    /* Data follows (16 floats max for mat4) */
    float data[16];
} videoctl2_set_uniform_t;

/* ========== Set Vertex Attribute ========== */
typedef struct {
    videoctl2_hdr_t hdr;
    uint32_t index;             /* Attribute index */
    uint8_t size;               /* 1-4 components */
    uint8_t type;               /* 0=float, 1=int, 2=ubyte */
    uint8_t normalized;         /* Normalize to [0,1] */
    uint32_t stride;            /* Bytes between vertices */
    uint32_t offset;            /* Offset in buffer */
} videoctl2_set_attribute_t;

/* ========== Draw Arrays ========== */
typedef struct {
    videoctl2_hdr_t hdr;
    uint8_t primitive;          /* videoctl2_primitive_t */
    uint32_t first;             /* First vertex */
    uint32_t count;             /* Number of vertices */
} videoctl2_draw_arrays_t;

/* ========== Draw Elements (Indexed) ========== */
typedef struct {
    videoctl2_hdr_t hdr;
    uint8_t primitive;
    uint32_t count;             /* Number of indices */
    uint32_t index_buffer;      /* Index buffer handle */
} videoctl2_draw_elements_t;

#endif /* VIDEOCTL2_SHADER_H */
