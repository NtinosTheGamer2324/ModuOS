// NodGL Shader API - GPU-accelerated programmable rendering
// Allows custom shaders for effects, text rendering, compositing, etc.

#pragma once
#include <stdint.h>

/* Shader types */
typedef enum {
    NODGL_SHADER_VERTEX = 0,      /* Vertex shader */
    NODGL_SHADER_FRAGMENT = 1,    /* Fragment/pixel shader */
    NODGL_SHADER_COMPUTE = 2,     /* Compute shader (for GPGPU) */
} NodGL_ShaderType;

/* Shader handle (opaque) */
typedef uint32_t NodGL_Shader;
typedef uint32_t NodGL_Program;

/* Shader compilation result */
typedef struct {
    int success;
    char error_log[512];          /* Compilation errors if any */
} NodGL_ShaderCompileResult;

/* Vertex attribute types */
typedef enum {
    NODGL_ATTR_FLOAT = 0,
    NODGL_ATTR_VEC2 = 1,
    NODGL_ATTR_VEC3 = 2,
    NODGL_ATTR_VEC4 = 3,
    NODGL_ATTR_INT = 4,
} NodGL_AttributeType;

/* Vertex buffer descriptor */
typedef struct {
    const void *data;             /* Vertex data */
    uint32_t size;                /* Size in bytes */
    uint32_t stride;              /* Bytes between vertices */
    uint32_t count;               /* Number of vertices */
} NodGL_VertexBuffer;

/* Vertex attribute binding */
typedef struct {
    uint32_t location;            /* Shader attribute location */
    NodGL_AttributeType type;     /* Attribute type */
    uint32_t offset;              /* Offset in vertex structure */
    uint8_t normalized;           /* Normalize integers to [0,1] or [-1,1] */
} NodGL_VertexAttribute;

/* Uniform types */
typedef enum {
    NODGL_UNIFORM_FLOAT = 0,
    NODGL_UNIFORM_VEC2 = 1,
    NODGL_UNIFORM_VEC3 = 2,
    NODGL_UNIFORM_VEC4 = 3,
    NODGL_UNIFORM_INT = 4,
    NODGL_UNIFORM_MAT4 = 5,       /* 4x4 matrix */
    NODGL_UNIFORM_SAMPLER2D = 6,  /* Texture sampler */
} NodGL_UniformType;

/* Primitive types for drawing */
typedef enum {
    NODGL_TRIANGLES = 0,
    NODGL_TRIANGLE_STRIP = 1,
    NODGL_TRIANGLE_FAN = 2,
    NODGL_LINES = 3,
    NODGL_LINE_STRIP = 4,
    NODGL_POINTS = 5,
} NodGL_PrimitiveType;

/* ========== Shader Compilation ========== */

/**
 * Create a shader from source code
 * @param type Shader type (vertex, fragment, compute)
 * @param source Shader source code (GLSL-like)
 * @param result Compilation result (output)
 * @return Shader handle, or 0 on failure
 */
NodGL_Shader NodGL_CreateShader(NodGL_ShaderType type, 
                                const char *source,
                                NodGL_ShaderCompileResult *result);

/**
 * Delete a shader
 */
void NodGL_DeleteShader(NodGL_Shader shader);

/**
 * Create a shader program by linking vertex + fragment shaders
 * @param vertex_shader Compiled vertex shader
 * @param fragment_shader Compiled fragment shader
 * @param result Link result (output)
 * @return Program handle, or 0 on failure
 */
NodGL_Program NodGL_CreateProgram(NodGL_Shader vertex_shader,
                                  NodGL_Shader fragment_shader,
                                  NodGL_ShaderCompileResult *result);

/**
 * Delete a shader program
 */
void NodGL_DeleteProgram(NodGL_Program program);

/* ========== Shader Binding ========== */

/**
 * Use a shader program for rendering
 */
void NodGL_UseProgram(NodGL_Program program);

/**
 * Get uniform location by name
 */
int NodGL_GetUniformLocation(NodGL_Program program, const char *name);

/**
 * Get attribute location by name
 */
int NodGL_GetAttributeLocation(NodGL_Program program, const char *name);

/* ========== Uniform Setting ========== */

void NodGL_SetUniform1f(int location, float value);
void NodGL_SetUniform2f(int location, float x, float y);
void NodGL_SetUniform3f(int location, float x, float y, float z);
void NodGL_SetUniform4f(int location, float x, float y, float z, float w);
void NodGL_SetUniform1i(int location, int value);
void NodGL_SetUniformMatrix4fv(int location, const float *matrix);

/* ========== Vertex Buffers ========== */

/**
 * Create vertex buffer on GPU
 */
uint32_t NodGL_CreateVertexBuffer(const void *data, uint32_t size);

/**
 * Update vertex buffer data
 */
void NodGL_UpdateVertexBuffer(uint32_t buffer, const void *data, uint32_t size);

/**
 * Delete vertex buffer
 */
void NodGL_DeleteVertexBuffer(uint32_t buffer);

/**
 * Bind vertex buffer for rendering (shader API)
 */
void NodGL_Shader_BindVertexBuffer(uint32_t buffer);

/**
 * Set vertex attribute pointers
 */
void NodGL_SetVertexAttributes(const NodGL_VertexAttribute *attributes, int count);

/* ========== Drawing ========== */

/**
 * Draw primitives using current shader and vertex buffer
 */
void NodGL_DrawArrays(NodGL_PrimitiveType type, int first, int count);

/**
 * Draw indexed primitives
 */
void NodGL_DrawElements(NodGL_PrimitiveType type, int count, const uint32_t *indices);

/* ========== Built-in Shaders ========== */

/**
 * Get built-in shader program for common operations
 */
typedef enum {
    NODGL_BUILTIN_SIMPLE_TEXTURE = 0,  /* Simple textured quad */
    NODGL_BUILTIN_TEXT_RENDER = 1,     /* SDF text rendering */
    NODGL_BUILTIN_BLUR = 2,            /* Gaussian blur */
    NODGL_BUILTIN_COLOR_CORRECT = 3,   /* Color correction */
} NodGL_BuiltinShader;

NodGL_Program NodGL_GetBuiltinShader(NodGL_BuiltinShader shader);
