// Shader Demo - Demonstrates NodGL shader API
// Shows GPU-accelerated text rendering and effects

#define LIBC_NO_START
#include "libc.h"
#include "NodGL.h"
#include "NodGL_shader.h"

/* Example: GPU-accelerated text rendering with SDF (Signed Distance Field) */
static const char *TEXT_VERTEX_SHADER =
    "#version 110\n"
    "attribute vec2 a_position;\n"
    "attribute vec2 a_texcoord;\n"
    "varying vec2 v_texcoord;\n"
    "void main() {\n"
    "    gl_Position = vec4(a_position, 0.0, 1.0);\n"
    "    v_texcoord = a_texcoord;\n"
    "}\n";

/* SDF text fragment shader - smooth text at any size! */
static const char *TEXT_FRAGMENT_SHADER =
    "#version 110\n"
    "varying vec2 v_texcoord;\n"
    "uniform sampler2D u_font_atlas;\n"
    "uniform vec4 u_text_color;\n"
    "uniform float u_smoothness;\n"
    "void main() {\n"
    "    float distance = texture2D(u_font_atlas, v_texcoord).a;\n"
    "    float alpha = smoothstep(0.5 - u_smoothness, 0.5 + u_smoothness, distance);\n"
    "    gl_FragColor = vec4(u_text_color.rgb, u_text_color.a * alpha);\n"
    "}\n";

/* Example: Gaussian blur shader */
static const char *BLUR_FRAGMENT_SHADER =
    "#version 110\n"
    "varying vec2 v_texcoord;\n"
    "uniform sampler2D u_texture;\n"
    "uniform vec2 u_resolution;\n"
    "uniform float u_blur_amount;\n"
    "void main() {\n"
    "    vec2 pixel = 1.0 / u_resolution;\n"
    "    vec4 color = vec4(0.0);\n"
    "    \n"
    "    // 9-tap blur kernel\n"
    "    color += texture2D(u_texture, v_texcoord + vec2(-pixel.x, -pixel.y)) * 0.0625;\n"
    "    color += texture2D(u_texture, v_texcoord + vec2(0.0, -pixel.y)) * 0.125;\n"
    "    color += texture2D(u_texture, v_texcoord + vec2(pixel.x, -pixel.y)) * 0.0625;\n"
    "    \n"
    "    color += texture2D(u_texture, v_texcoord + vec2(-pixel.x, 0.0)) * 0.125;\n"
    "    color += texture2D(u_texture, v_texcoord) * 0.25;\n"
    "    color += texture2D(u_texture, v_texcoord + vec2(pixel.x, 0.0)) * 0.125;\n"
    "    \n"
    "    color += texture2D(u_texture, v_texcoord + vec2(-pixel.x, pixel.y)) * 0.0625;\n"
    "    color += texture2D(u_texture, v_texcoord + vec2(0.0, pixel.y)) * 0.125;\n"
    "    color += texture2D(u_texture, v_texcoord + vec2(pixel.x, pixel.y)) * 0.0625;\n"
    "    \n"
    "    gl_FragColor = color;\n"
    "}\n";

int md_main(long argc, char **argv) {
    (void)argc; (void)argv;
    
    printf("=== NodGL Shader Demo ===\n");
    
    /* Create NodGL device */
    NodGL_Device device;
    NodGL_Context context;
    uint32_t feature_level;
    
    int rc = NodGL_CreateDevice(NodGL_FEATURE_LEVEL_1_0, &device, &context, &feature_level);
    if (rc != 0) {
        printf("ERROR: Failed to create NodGL device\n");
        return 1;
    }
    
    printf("NodGL device created!\n");
    
    /* ========== Compile Text Shader ========== */
    printf("\n[1] Compiling SDF text shader...\n");
    
    NodGL_ShaderCompileResult result;
    
    NodGL_Shader text_vs = NodGL_CreateShader(NODGL_SHADER_VERTEX, TEXT_VERTEX_SHADER, &result);
    if (!text_vs) {
        printf("ERROR: Vertex shader compilation failed:\n%s\n", result.error_log);
        goto cleanup;
    }
    printf("  ✓ Vertex shader compiled (handle: %u)\n", text_vs);
    
    NodGL_Shader text_fs = NodGL_CreateShader(NODGL_SHADER_FRAGMENT, TEXT_FRAGMENT_SHADER, &result);
    if (!text_fs) {
        printf("ERROR: Fragment shader compilation failed:\n%s\n", result.error_log);
        NodGL_DeleteShader(text_vs);
        goto cleanup;
    }
    printf("  ✓ Fragment shader compiled (handle: %u)\n", text_fs);
    
    NodGL_Program text_program = NodGL_CreateProgram(text_vs, text_fs, &result);
    if (!text_program) {
        printf("ERROR: Program linking failed:\n%s\n", result.error_log);
        NodGL_DeleteShader(text_vs);
        NodGL_DeleteShader(text_fs);
        goto cleanup;
    }
    printf("  ✓ Program linked (handle: %u)\n", text_program);
    
    /* ========== Compile Blur Shader ========== */
    printf("\n[2] Compiling gaussian blur shader...\n");
    
    /* Reuse vertex shader */
    NodGL_Shader blur_fs = NodGL_CreateShader(NODGL_SHADER_FRAGMENT, BLUR_FRAGMENT_SHADER, &result);
    if (!blur_fs) {
        printf("ERROR: Blur shader compilation failed:\n%s\n", result.error_log);
    } else {
        printf("  ✓ Blur shader compiled (handle: %u)\n", blur_fs);
        
        NodGL_Program blur_program = NodGL_CreateProgram(text_vs, blur_fs, &result);
        if (blur_program) {
            printf("  ✓ Blur program linked (handle: %u)\n", blur_program);
            
            /* Demonstrate setting uniforms */
            printf("\n[3] Setting blur uniforms...\n");
            NodGL_UseProgram(blur_program);
            NodGL_SetUniform2f(0, 1024.0f, 768.0f);  /* u_resolution */
            NodGL_SetUniform1f(1, 2.0f);             /* u_blur_amount */
            printf("  ✓ Uniforms set\n");
            
            NodGL_DeleteProgram(blur_program);
        }
        
        NodGL_DeleteShader(blur_fs);
    }
    
    /* ========== Test Built-in Shaders ========== */
    printf("\n[4] Loading built-in texture shader...\n");
    
    NodGL_Program builtin = NodGL_GetBuiltinShader(NODGL_BUILTIN_SIMPLE_TEXTURE);
    if (builtin) {
        printf("  ✓ Built-in shader loaded (handle: %u)\n", builtin);
    } else {
        printf("  ✗ Built-in shader not available\n");
    }
    
    /* ========== Cleanup ========== */
    printf("\n[5] Cleaning up...\n");
    
    NodGL_DeleteProgram(text_program);
    NodGL_DeleteShader(text_fs);
    NodGL_DeleteShader(text_vs);
    
    printf("  ✓ Shaders deleted\n");
    
cleanup:
    NodGL_ReleaseDevice(device);
    
    printf("\n=== Shader Demo Complete ===\n");
    printf("\nNodGL now has GPU shader support!\n");
    printf("Apps can use custom shaders for:\n");
    printf("  • SDF text rendering (smooth at any size)\n");
    printf("  • Post-processing effects (blur, bloom, etc.)\n");
    printf("  • Custom rendering (particles, procedural, etc.)\n");
    printf("  • Compute shaders (GPGPU operations)\n");
    
    return 0;
}
