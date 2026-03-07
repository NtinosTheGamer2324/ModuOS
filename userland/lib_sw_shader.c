// lib_sw_shader.c - Software shader compiler/interpreter (USERLAND)
// Simple shader execution on CPU for drivers that don't have GPU shader support
// This allows shader API to work on any GPU, with fallback to CPU execution

#define LIBC_NO_START
#include "libc.h"
#include "string.h"

#define MAX_SHADERS 64
#define MAX_PROGRAMS 32
#define MAX_UNIFORMS 32

/* Shader types (matching videoctl2_shader.h) */
typedef enum {
    VIDEOCTL2_SHADER_VERTEX = 0,
    VIDEOCTL2_SHADER_FRAGMENT = 1,
    VIDEOCTL2_SHADER_COMPUTE = 2,
} videoctl2_shader_type_t;

typedef enum {
    VIDEOCTL2_SHADER_LANG_GLSL = 0,
    VIDEOCTL2_SHADER_LANG_HLSL = 1,
    VIDEOCTL2_SHADER_LANG_SPIRV = 2,
    VIDEOCTL2_SHADER_LANG_MODUOS = 3,
} videoctl2_shader_lang_t;

typedef enum {
    VIDEOCTL2_UNIFORM_FLOAT = 0,
    VIDEOCTL2_UNIFORM_VEC2 = 1,
    VIDEOCTL2_UNIFORM_VEC3 = 2,
    VIDEOCTL2_UNIFORM_VEC4 = 3,
    VIDEOCTL2_UNIFORM_INT = 4,
    VIDEOCTL2_UNIFORM_MAT4 = 5,
    VIDEOCTL2_UNIFORM_SAMPLER2D = 6,
} videoctl2_uniform_type_t;

/* Shader bytecode operations */
typedef enum {
    SHADER_OP_NOP = 0,
    SHADER_OP_MOV,          /* Move/copy */
    SHADER_OP_ADD,          /* Add */
    SHADER_OP_MUL,          /* Multiply */
    SHADER_OP_DOT,          /* Dot product */
    SHADER_OP_TEXTURE,      /* Sample texture */
    SHADER_OP_RETURN,       /* Return from shader */
} shader_op_t;

/* Compiled shader */
typedef struct {
    uint32_t handle;
    uint8_t type;           /* Vertex or fragment */
    uint8_t *bytecode;      /* Compiled bytecode */
    uint32_t bytecode_len;
    char error_log[512];
    int valid;
} sw_shader_t;

/* Shader program */
typedef struct {
    uint32_t handle;
    uint32_t vertex_shader;
    uint32_t fragment_shader;
    float uniforms[MAX_UNIFORMS][4];  /* Up to 32 vec4 uniforms */
    int valid;
} sw_program_t;

static sw_shader_t g_shaders[MAX_SHADERS];
static sw_program_t g_programs[MAX_PROGRAMS];
static uint32_t g_next_shader_handle = 1;
static uint32_t g_next_program_handle = 1;

/* ========== Shader Compilation ========== */

/* Simple GLSL parser - recognizes basic patterns */
static int compile_shader_glsl(const char *source, sw_shader_t *shader) {
    /* For now, just validate and store source as "bytecode" */
    /* A real implementation would parse GLSL and generate IR */
    
    if (!source || !shader) return -1;
    
    /* Basic validation - check for required functions */
    if (shader->type == VIDEOCTL2_SHADER_VERTEX) {
        if (!strstr(source, "main")) {
            strcpy(shader->error_log, "ERROR: Vertex shader must have main() function");
            return -1;
        }
        if (!strstr(source, "gl_Position")) {
            strcpy(shader->error_log, "ERROR: Vertex shader must write gl_Position");
            return -1;
        }
    } else if (shader->type == VIDEOCTL2_SHADER_FRAGMENT) {
        if (!strstr(source, "main")) {
            strcpy(shader->error_log, "ERROR: Fragment shader must have main() function");
            return -1;
        }
    }
    
    /* Store source as bytecode (for now) */
    shader->bytecode_len = strlen(source) + 1;
    shader->bytecode = malloc(shader->bytecode_len);
    if (!shader->bytecode) {
        strcpy(shader->error_log, "ERROR: Out of memory");
        return -1;
    }
    
    memcpy(shader->bytecode, source, shader->bytecode_len);
    shader->valid = 1;
    
    return 0;
}

/* ========== Public API ========== */

/**
 * Create and compile a shader
 */
int sw_shader_create(uint8_t type, uint8_t language, const char *source, 
                     uint32_t *out_handle, char *error_log) {
    /* Find free slot */
    sw_shader_t *shader = NULL;
    for (int i = 0; i < MAX_SHADERS; i++) {
        if (!g_shaders[i].valid) {
            shader = &g_shaders[i];
            break;
        }
    }
    
    if (!shader) {
        if (error_log) strcpy(error_log, "ERROR: No free shader slots");
        return -1;
    }
    
    memset(shader, 0, sizeof(sw_shader_t));
    shader->handle = g_next_shader_handle++;
    shader->type = type;
    
    /* Compile based on language */
    int rc = -1;
    if (language == VIDEOCTL2_SHADER_LANG_GLSL) {
        rc = compile_shader_glsl(source, shader);
    } else {
        strcpy(shader->error_log, "ERROR: Unsupported shader language");
        rc = -1;
    }
    
    if (rc != 0) {
        if (error_log) strcpy(error_log, shader->error_log);
        shader->valid = 0;
        return -1;
    }
    
    *out_handle = shader->handle;
    return 0;
}

/**
 * Delete a shader
 */
void sw_shader_delete(uint32_t handle) {
    for (int i = 0; i < MAX_SHADERS; i++) {
        if (g_shaders[i].valid && g_shaders[i].handle == handle) {
            if (g_shaders[i].bytecode) {
                free(g_shaders[i].bytecode);
            }
            memset(&g_shaders[i], 0, sizeof(sw_shader_t));
            return;
        }
    }
}

/**
 * Create shader program by linking shaders
 */
int sw_program_create(uint32_t vertex_shader, uint32_t fragment_shader,
                      uint32_t *out_handle, char *error_log) {
    /* Validate shaders exist */
    sw_shader_t *vs = NULL, *fs = NULL;
    
    for (int i = 0; i < MAX_SHADERS; i++) {
        if (g_shaders[i].valid) {
            if (g_shaders[i].handle == vertex_shader) vs = &g_shaders[i];
            if (g_shaders[i].handle == fragment_shader) fs = &g_shaders[i];
        }
    }
    
    if (!vs || !fs) {
        if (error_log) strcpy(error_log, "ERROR: Invalid shader handles");
        return -1;
    }
    
    if (vs->type != VIDEOCTL2_SHADER_VERTEX || fs->type != VIDEOCTL2_SHADER_FRAGMENT) {
        if (error_log) strcpy(error_log, "ERROR: Wrong shader types");
        return -1;
    }
    
    /* Find free program slot */
    sw_program_t *prog = NULL;
    for (int i = 0; i < MAX_PROGRAMS; i++) {
        if (!g_programs[i].valid) {
            prog = &g_programs[i];
            break;
        }
    }
    
    if (!prog) {
        if (error_log) strcpy(error_log, "ERROR: No free program slots");
        return -1;
    }
    
    memset(prog, 0, sizeof(sw_program_t));
    prog->handle = g_next_program_handle++;
    prog->vertex_shader = vertex_shader;
    prog->fragment_shader = fragment_shader;
    prog->valid = 1;
    
    *out_handle = prog->handle;
    return 0;
}

/**
 * Delete program
 */
void sw_program_delete(uint32_t handle) {
    for (int i = 0; i < MAX_PROGRAMS; i++) {
        if (g_programs[i].valid && g_programs[i].handle == handle) {
            memset(&g_programs[i], 0, sizeof(sw_program_t));
            return;
        }
    }
}

/**
 * Set uniform value
 */
int sw_program_set_uniform(uint32_t program, int location, uint8_t type, const float *data) {
    if (location < 0 || location >= MAX_UNIFORMS) return -1;
    
    for (int i = 0; i < MAX_PROGRAMS; i++) {
        if (g_programs[i].valid && g_programs[i].handle == program) {
            /* Copy uniform data */
            int components = 1;
            if (type == VIDEOCTL2_UNIFORM_VEC2) components = 2;
            else if (type == VIDEOCTL2_UNIFORM_VEC3) components = 3;
            else if (type == VIDEOCTL2_UNIFORM_VEC4) components = 4;
            else if (type == VIDEOCTL2_UNIFORM_MAT4) components = 16;
            
            for (int j = 0; j < components && j < 4; j++) {
                g_programs[i].uniforms[location][j] = data[j];
            }
            
            return 0;
        }
    }
    
    return -1;
}

/**
 * Execute fragment shader (software rasterization fallback)
 * This is a stub - real implementation would interpret bytecode
 */
int sw_shader_execute_fragment(uint32_t program, float x, float y, 
                                float *uv, uint32_t *out_color) {
    (void)program; (void)x; (void)y; (void)uv;
    
    /* Default: output white */
    *out_color = 0xFFFFFFFF;
    
    return 0;
}
