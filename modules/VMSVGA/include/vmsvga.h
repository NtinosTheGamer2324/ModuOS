#pragma once
#include <stdint.h>

// VMware SVGA II (VMSVGA) minimal register + FIFO definitions
// This is enough for basic framebuffer + UpdateRect in virtual machines.

#define VMSVGA_VENDOR_VMWARE 0x15AD
#define VMSVGA_DEVICE_SVGA2  0x0405

// IO ports (index/value). VMware SVGA uses base+0 (INDEX) and base+1 (VALUE)
// even for 32-bit in/outl.
#define SVGA_INDEX_PORT_OFF    0
#define SVGA_VALUE_PORT_OFF    1

// SVGA registers (index values)
// Reference: open-vm-tools / Linux vmwgfx (high-level)
// NOTE: This is a minimal subset.
#define SVGA_REG_ID              0
#define SVGA_REG_ENABLE          1
#define SVGA_REG_WIDTH           2
#define SVGA_REG_HEIGHT          3
#define SVGA_REG_MAX_WIDTH       4
#define SVGA_REG_MAX_HEIGHT      5
#define SVGA_REG_DEPTH           6
#define SVGA_REG_BITS_PER_PIXEL  7
#define SVGA_REG_PSEUDOCOLOR     8
#define SVGA_REG_RED_MASK        9
#define SVGA_REG_GREEN_MASK      10
#define SVGA_REG_BLUE_MASK       11
#define SVGA_REG_BYTES_PER_LINE  12
#define SVGA_REG_FB_START        13
#define SVGA_REG_FB_OFFSET       14
#define SVGA_REG_VRAM_SIZE       15
#define SVGA_REG_FB_SIZE         16
#define SVGA_REG_CAPABILITIES    17
#define SVGA_REG_MEM_START       18
#define SVGA_REG_MEM_SIZE        19
#define SVGA_REG_CONFIG_DONE     20
#define SVGA_REG_SYNC            21
#define SVGA_REG_BUSY            22

// IDs
// SVGA protocol IDs (must be 0x90000000 + version)
#define SVGA_ID_0 0x90000000
#define SVGA_ID_1 0x90000001
#define SVGA_ID_2 0x90000002

// FIFO commands (2D acceleration)
#define SVGA_CMD_UPDATE     1
#define SVGA_CMD_RECT_FILL  2
#define SVGA_CMD_RECT_COPY  3

// 3D commands (SVGA3D command set)
#define SVGA_CMD_DEFINE_GMRFB      4
#define SVGA_CMD_BLIT_GMRFB_TO_SCREEN 5
#define SVGA_CMD_BLIT_SCREEN_TO_GMRFB 6
#define SVGA_CMD_ANNOTATION_FILL   7
#define SVGA_CMD_ANNOTATION_COPY   8

// Capabilities
#define SVGA_CAP_RECT_FILL      (1 << 0)
#define SVGA_CAP_RECT_COPY      (1 << 1)
#define SVGA_CAP_3D             (1 << 2)  // Full 3D acceleration
#define SVGA_CAP_EXTENDED_FIFO  (1 << 3)

// FIFO registers (offsets in 32-bit words)
#define SVGA_FIFO_MIN          0
#define SVGA_FIFO_MAX          1
#define SVGA_FIFO_NEXT_CMD     2
#define SVGA_FIFO_STOP         3

// UPDATE command payload
typedef struct {
    uint32_t cmd;   // SVGA_CMD_UPDATE
    uint32_t x;
    uint32_t y;
    uint32_t w;
    uint32_t h;
} __attribute__((packed)) svga_fifo_update_t;

// RECT_FILL payload
typedef struct {
    uint32_t cmd;   // SVGA_CMD_RECT_FILL
    uint32_t color; // native 32bpp pixel (XRGB8888)
    uint32_t x;
    uint32_t y;
    uint32_t w;
    uint32_t h;
} __attribute__((packed)) svga_fifo_rect_fill_t;

// RECT_COPY payload
typedef struct {
    uint32_t cmd;   // SVGA_CMD_RECT_COPY
    uint32_t src_x;
    uint32_t src_y;
    uint32_t dst_x;
    uint32_t dst_y;
    uint32_t w;
    uint32_t h;
} __attribute__((packed)) svga_fifo_rect_copy_t;

// Simple polygon fill (uses RECT_FILL for triangle rasterization acceleration)
// For true 3D, would need full SVGA3D context setup, but for basic triangle
// drawing we can use accelerated rect fills to approximate
typedef struct {
    uint32_t cmd;   // SVGA_CMD_RECT_FILL
    uint32_t color;
    uint32_t x;
    uint32_t y;
    uint32_t w;
    uint32_t h;
} __attribute__((packed)) svga_fifo_polygon_fill_t;
