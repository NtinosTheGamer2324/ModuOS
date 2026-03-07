// lib_sw_shader.h - Userland software shader API
// Software shader compiler/interpreter for CPU fallback

#pragma once
#include <stdint.h>

// Shader compilation API (runs in userland)
int sw_shader_create(uint8_t type, uint8_t language, const char *source, 
                     uint32_t *out_handle, char *error_log);
void sw_shader_delete(uint32_t handle);

int sw_program_create(uint32_t vertex_shader, uint32_t fragment_shader,
                      uint32_t *out_handle, char *error_log);
void sw_program_delete(uint32_t handle);

int sw_program_set_uniform(uint32_t program, int location, uint8_t type, const float *data);
int sw_shader_execute_fragment(uint32_t program, float x, float y, 
                                float *uv, uint32_t *out_color);
