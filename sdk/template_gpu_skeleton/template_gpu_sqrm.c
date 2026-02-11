#include "moduos/kernel/sqrm.h"

/*
 * template_gpu_skeleton (GPU-side SQRM protocol tutorial)
 *
 * Audience:
 *   People who do NOT know ModuOS's SQRM GPU ABI yet.
 *   This is written as a "how to plug a GPU driver into ModuOS" tutorial.
 *
 * What you MUST do to become the active GPU:
 *   1) In sqrm_module_init(), find the device (usually PCI)
 *   2) Map the framebuffer (VRAM or scanout buffer) to a CPU virtual address
 *   3) Fill framebuffer_t correctly
 *   4) Fill sqrm_gpu_device_t (fb + optional callbacks)
 *   5) Call api->gfx_register_framebuffer(&g_dev)
 *
 * That's the "GPU-side SQRM protocol".
 *
 * -------------------------------------------------
 * Minimal framebuffer_t fields the kernel expects:
 * -------------------------------------------------
 *   fb.addr   : non-NULL CPU virtual pointer to pixels
 *   fb.width  : > 0
 *   fb.height : > 0
 *   fb.pitch  : bytes per scanline (>= width*(bpp/8))
 *   fb.bpp    : commonly 32
 *
 * Optional:
 *   fb.red_pos / red_mask_size / green_* / blue_* for correct color packing
 *
 * -------------------------------------------------
 * flush() callback (optional):
 * -------------------------------------------------
 * If the GPU scans out directly from fb.addr, set g_dev.flush = NULL.
 * If you draw into a shadow buffer and need a "present" command, implement flush().
 *
 * -------------------------------------------------
 * set_mode/enumerate_modes (optional):
 * -------------------------------------------------
 * Provide enumerate_modes() so userland can pick a resolution.
 * Provide set_mode() so userland can request a change.
 * After changing mode, call api->gfx_update_framebuffer(&new_fb).
 */

static sqrm_module_desc_t sqrm_module_desc = {
    .abi_version = 1,
    .type = SQRM_TYPE_GPU,
    .name = "template_gpu",
};

static sqrm_gpu_device_t g_dev;

// Optional: if your GPU needs explicit flush/present operations.
static void gpu_flush(const framebuffer_t *fb, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    (void)fb; (void)x; (void)y; (void)w; (void)h;
    // TODO:
    //  - for "dumb" linear framebuffers, this can be NULL
    //  - for command-queue GPUs, submit a present/flush command here
}

// Optional: enumerate modes.
static int gpu_enumerate_modes(gfx_mode_t *out_modes, uint32_t max_modes) {
    if (!out_modes || max_modes == 0) return -1;

    // TODO: fill from EDID / firmware tables / GPU registers.
    // Minimal example: advertise one mode.
    if (max_modes >= 1) {
        out_modes[0].width = 1024;
        out_modes[0].height = 768;
        out_modes[0].bpp = 32;
        return 1;
    }

    return 0;
}

// Optional: set a video mode.
static int gpu_set_mode(uint32_t width, uint32_t height, uint32_t bpp) {
    (void)width; (void)height; (void)bpp;

    // TODO (real driver):
    //  - program mode timing registers
    //  - allocate/configure scanout buffer
    //  - set fb.addr/fb.pitch/fb.bpp
    //  - then call api->gfx_update_framebuffer_from_sqrm(&fb)

    return -1;
}

int sqrm_module_init(const sqrm_kernel_api_t *api) {
    if (!api || api->abi_version != 1) return -1;

    if (api->com_write_string) {
        api->com_write_string(0x3F8, "[SQRM-GPU] template_gpu_skeleton loaded\n");
        api->com_write_string(0x3F8, "[SQRM-GPU] NOTE: skeleton does not bind hardware; returning -1 so autoload continues\n");
    }

    // This is a documentation template.
    // It must never claim the GPU slot in autoload.
    return -1;
}
