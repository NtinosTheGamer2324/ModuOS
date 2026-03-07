// NodGL Userland Library - Uses Kernel GPU Gateway
// This replaces the slow software rasterizer with FAST GPU syscalls

#include "NodGL.h"

// Syscall numbers
#define SYS_NODGL_INIT         80
#define SYS_NODGL_DESTROY      81
#define SYS_NODGL_CLEAR        82
#define SYS_NODGL_FILL_RECT    83
#define SYS_NODGL_DRAW_LINE    84
#define SYS_NODGL_BLIT         85
#define SYS_NODGL_FLUSH        86
#define SYS_NODGL_PRESENT      87

// Syscall wrapper
static inline long syscall(long num, long a1, long a2, long a3) {
    long ret;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(a1), "S"(a2), "d"(a3)
        : "rcx", "r11", "memory"
    );
    return ret;
}

// Command structures
typedef struct {
    int x, y;
    uint32_t width, height;
    uint32_t color;
} NodGL_fill_rect_cmd_t;

typedef struct {
    int x0, y0;
    int x1, y1;
    uint32_t color;
} NodGL_line_cmd_t;

// Global device handle
static int g_nodgl_device = -1;

// Initialize NodGL - called once at startup
int NodGL_Init(void) {
    if (g_nodgl_device >= 0) return 0; // Already initialized
    
    long handle = syscall(SYS_NODGL_INIT, 0, 0, 0);
    if (handle < 0) {
        return -1; // Failed to initialize
    }
    
    g_nodgl_device = (int)handle;
    return 0;
}

// Cleanup
void NodGL_Shutdown(void) {
    if (g_nodgl_device >= 0) {
        syscall(SYS_NODGL_DESTROY, g_nodgl_device, 0, 0);
        g_nodgl_device = -1;
    }
}

// Clear screen - INSTANT via GPU!
void NodGL_Clear(uint32_t color) {
    if (g_nodgl_device < 0) return;
    syscall(SYS_NODGL_CLEAR, g_nodgl_device, color, 0);
}

// Fill rectangle - INSTANT via GPU!
void NodGL_FillRect(int x, int y, uint32_t width, uint32_t height, uint32_t color) {
    if (g_nodgl_device < 0) return;
    
    NodGL_fill_rect_cmd_t cmd = {
        .x = x,
        .y = y,
        .width = width,
        .height = height,
        .color = color
    };
    
    syscall(SYS_NODGL_FILL_RECT, g_nodgl_device, (long)&cmd, 0);
}

// Draw line - INSTANT via GPU!
void NodGL_DrawLine(int x0, int y0, int x1, int y1, uint32_t color) {
    if (g_nodgl_device < 0) return;
    
    NodGL_line_cmd_t cmd = {
        .x0 = x0,
        .y0 = y0,
        .x1 = x1,
        .y1 = y1,
        .color = color
    };
    
    syscall(SYS_NODGL_DRAW_LINE, g_nodgl_device, (long)&cmd, 0);
}

// Flush commands to GPU
void NodGL_Flush(void) {
    if (g_nodgl_device < 0) return;
    syscall(SYS_NODGL_FLUSH, g_nodgl_device, 0, 0);
}

// Present frame
void NodGL_Present(void) {
    if (g_nodgl_device < 0) return;
    syscall(SYS_NODGL_PRESENT, g_nodgl_device, 0, 0);
}

// Triangle drawing - for now, use three lines (GPU still faster than software!)
void NodGL_DrawTriangle(int x0, int y0, int x1, int y1, int x2, int y2, uint32_t color) {
    NodGL_DrawLine(x0, y0, x1, y1, color);
    NodGL_DrawLine(x1, y1, x2, y2, color);
    NodGL_DrawLine(x2, y2, x0, y0, color);
}
