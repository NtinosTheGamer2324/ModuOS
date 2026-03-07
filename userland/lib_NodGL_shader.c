// NodGL Shader API Implementation
// GPU-accelerated programmable rendering

#define LIBC_NO_START
#include "NodGL_shader.h"
#include "gfx2d.h"
#include "lib_sw_shader.h"
#include <string.h>

/* ========== Shader Compilation ========== */

NodGL_Shader NodGL_CreateShader(NodGL_ShaderType type, 
                                const char *source,
                                NodGL_ShaderCompileResult *result) {
    if (!source || !result) return 0;
    
    /* Compile shader in userspace - no kernel involvement */
    uint32_t shader_handle = 0;
    int rc = sw_shader_create((uint8_t)type, 0 /* GLSL */, source, 
                             &shader_handle, result->error_log);
    
    if (rc == 0) {
        result->success = 1;
        return (NodGL_Shader)shader_handle;
    } else {
        result->success = 0;
        return 0;
    }
}

void NodGL_DeleteShader(NodGL_Shader shader) {
    /* Delete shader in userspace */
    sw_shader_delete(shader);
}

/* ========== Shader Programs ========== */

NodGL_Program NodGL_CreateProgram(NodGL_Shader vertex_shader,
                                  NodGL_Shader fragment_shader,
                                  NodGL_ShaderCompileResult *result) {
    if (!result) return 0;
    
    /* Link program in userspace */
    uint32_t program_handle = 0;
    int rc = sw_program_create(vertex_shader, fragment_shader,
                              &program_handle, result->error_log);
    
    if (rc == 0) {
        result->success = 1;
        return (NodGL_Program)program_handle;
    } else {
        result->success = 0;
        return 0;
    }
}

void NodGL_DeleteProgram(NodGL_Program program) {
    /* Delete program in userspace */
    sw_program_delete(program);
}

void NodGL_UseProgram(NodGL_Program program) {
    /* Program is already active in userspace - nothing to do */
    (void)program;
}

/* ========== Uniforms ========== */

void NodGL_SetUniform4f(int location, float x, float y, float z, float w) {
    /* Set uniform in userspace shader system */
    float data[4] = {x, y, z, w};
    /* TODO: Get current active program and call sw_program_set_uniform */
    (void)location;
    (void)data;
}

void NodGL_SetUniform1f(int location, float value) {
    NodGL_SetUniform4f(location, value, 0, 0, 0);
}

void NodGL_SetUniform2f(int location, float x, float y) {
    NodGL_SetUniform4f(location, x, y, 0, 0);
}

void NodGL_SetUniform3f(int location, float x, float y, float z) {
    NodGL_SetUniform4f(location, x, y, z, 0);
}

/* ========== Built-in Shaders ========== */

/* Simple texture shader */
static const char *BUILTIN_VERTEX_TEXTURE =
    "#version 110\n"
    "attribute vec2 a_position;\n"
    "attribute vec2 a_texcoord;\n"
    "varying vec2 v_texcoord;\n"
    "void main() {\n"
    "    gl_Position = vec4(a_position, 0.0, 1.0);\n"
    "    v_texcoord = a_texcoord;\n"
    "}\n";

static const char *BUILTIN_FRAGMENT_TEXTURE =
    "#version 110\n"
    "varying vec2 v_texcoord;\n"
    "uniform sampler2D u_texture;\n"
    "void main() {\n"
    "    gl_FragColor = texture2D(u_texture, v_texcoord);\n"
    "}\n";

static NodGL_Program g_builtin_programs[4] = {0};

NodGL_Program NodGL_GetBuiltinShader(NodGL_BuiltinShader shader) {
    if (shader >= 4) return 0;
    
    /* Lazy compile */
    if (g_builtin_programs[shader] == 0 && shader == NODGL_BUILTIN_SIMPLE_TEXTURE) {
        NodGL_ShaderCompileResult result;
        
        NodGL_Shader vs = NodGL_CreateShader(NODGL_SHADER_VERTEX, BUILTIN_VERTEX_TEXTURE, &result);
        if (!vs) return 0;
        
        NodGL_Shader fs = NodGL_CreateShader(NODGL_SHADER_FRAGMENT, BUILTIN_FRAGMENT_TEXTURE, &result);
        if (!fs) {
            NodGL_DeleteShader(vs);
            return 0;
        }
        
        g_builtin_programs[shader] = NodGL_CreateProgram(vs, fs, &result);
    }
    
    return g_builtin_programs[shader];
}
