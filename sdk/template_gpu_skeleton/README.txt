template_gpu_skeleton

This folder teaches the *GPU side* of the SQRM protocol in ModuOS.
It is meant for people who do NOT know the ModuOS GPU module ABI yet.

It does NOT implement a real GPU driver. It teaches how to:
  - discover a GPU via PCI
  - map its registers (BARs)
  - expose a linear framebuffer (framebuffer_t)
  - register it with the kernel using the SQRM API

Files:
  - template_gpu_sqrm.c   GPU SQRM skeleton (heavily commented)
  - build.sh              Builds template_gpu.sqrm

============================
SQRM GPU Protocol (Minimal)
============================

Your module must:
  1) export: sqrm_module_desc (name/type/abi)
  2) export: int sqrm_module_init(const sqrm_kernel_api_t *api)

To become the active GPU, your module calls:
  api->gfx_register_framebuffer(&sqrm_gpu_device_t)

The kernel will then:
  - switch into graphics mode
  - route VGA framebuffer backend to your framebuffer
  - optionally use a framebuffer console

============================
What is a framebuffer_t?
============================

framebuffer_t describes a LINEAR framebuffer:
  - fb.addr  : virtual address that the CPU can write pixels to
  - fb.width : pixels
  - fb.height: pixels
  - fb.pitch : bytes per scanline
  - fb.bpp   : bits per pixel (commonly 32)

Important rules:
  - fb.addr must be CPU-accessible (ioremap of VRAM / scanout buffer)
  - pitch must be >= width*(bpp/8)
  - If any required field is zero/NULL, registration should NOT happen.

============================
flush() hook
============================

Some GPUs are "dumb scanout":
  - writing to fb.addr immediately updates display
  - set flush = NULL

Some GPUs need explicit present/flush:
  - write pixels to a shadow buffer, then submit a blit/flip
  - implement flush(fb, x,y,w,h)

============================
Modes
============================

Optional but recommended:
  - enumerate_modes(): tell the kernel what modes you support
  - set_mode(w,h,bpp): program device timings/scanout and then call:
        api->gfx_update_framebuffer(&new_fb)

============================
Debug Tips
============================

  - Use api->com_write_string(COM1_PORT, "...") early and often.
  - First target "one fixed mode" (e.g. 1024x768x32).
  - Do NOT enable interrupts early; first get scanout working.

