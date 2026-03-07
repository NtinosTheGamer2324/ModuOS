// GPU Core - Like Linux DRM but simpler
// Manages GPU drivers, routes syscalls to active driver

#include <stddef.h>
#include <moduos/kernel/gpu_core.h>
#include <moduos/kernel/COM/com.h>

static const gpu_driver_ops_t *g_gpu_driver = NULL;
static void *g_gpu_driver_data = NULL;
static char g_gpu_name[64] = {0};

// Register GPU driver
int gpu_register_driver(const char *name, const gpu_driver_ops_t *ops, void *driver_data) {
    if (!ops || !name) return -1;
    
    // Only one GPU driver active at a time (for now)
    if (g_gpu_driver) {
        com_write_string(COM1_PORT, "[GPU] Driver already registered\n");
        return -1;
    }
    
    g_gpu_driver = ops;
    g_gpu_driver_data = driver_data;
    
    // Copy name
    int i = 0;
    while (name[i] && i < 63) {
        g_gpu_name[i] = name[i];
        i++;
    }
    g_gpu_name[i] = '\0';
    
    com_write_string(COM1_PORT, "[GPU] Registered driver: ");
    com_write_string(COM1_PORT, g_gpu_name);
    com_write_string(COM1_PORT, "\n");
    
    return 0;
}

// Get active driver
const gpu_driver_ops_t *gpu_get_driver(void) {
    return g_gpu_driver;
}

// Get driver data
void *gpu_get_driver_data(void) {
    return g_gpu_driver_data;
}
