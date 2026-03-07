// GPU Core - Simple kernel interface like Linux DRM
// GPU drivers register here, provide SIMPLE operations
// NO knowledge of NodGL/OpenGL/Vulkan - that's userspace!

#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// GPU info structure
typedef struct {
    char name[64];
    uint32_t vendor_id;
    uint32_t device_id;
    uint64_t vram_size;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint64_t framebuffer_phys;
} gpu_info_t;

// GPU buffer handle
typedef struct {
    uint32_t id;
    uint64_t size;
    uint64_t phys_addr;
    void *virt_addr;
} gpu_buffer_t;

// GPU command (generic command buffer submission)
typedef struct {
    uint32_t type;
    uint32_t size;
    void *data;
} gpu_command_t;

// GPU driver operations (what drivers implement)
typedef struct {
    int (*get_info)(gpu_info_t *info);
    int (*alloc_buffer)(uint64_t size, gpu_buffer_t *buf);
    void (*free_buffer)(gpu_buffer_t *buf);
    void *(*map_buffer)(gpu_buffer_t *buf);
    int (*submit_command)(const gpu_command_t *cmd);
    void (*wait_idle)(void);
    int (*present)(void);
} gpu_driver_ops_t;

// Register a GPU driver (called by GPU driver modules like qxl_gpu.sqrm)
int gpu_register_driver(const char *name, const gpu_driver_ops_t *ops, void *driver_data);

// Get active GPU driver
const gpu_driver_ops_t *gpu_get_driver(void);

#ifdef __cplusplus
}
#endif
