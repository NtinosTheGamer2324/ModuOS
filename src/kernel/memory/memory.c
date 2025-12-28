//memory.c
#include "moduos/kernel/memory/memory.h"
#include "moduos/kernel/memory/paging.h"
#include "moduos/kernel/memory/phys.h"
#include "moduos/kernel/COM/com.h"
#include "moduos/kernel/multiboot2.h"
#include "moduos/drivers/graphics/VGA.h"
#include "moduos/kernel/memory/string.h" /* itoa */

void memory_system_init(void *mb2)
{
    com_write_string(COM1_PORT, "[MEM] Starting memory system initialization...\n");
    
    com_write_string(COM1_PORT, "[MEM] Step 1: Parsing Multiboot2 info and initializing physical allocator...\n");
    memory_init(mb2);
    
    com_write_string(COM1_PORT, "[MEM] Step 2: Using bootloader's identity mapping...\n");
    early_identity_map();
    
    com_write_string(COM1_PORT, "[MEM] Step 3: Initializing paging system (copying bootloader mappings)...\n");
    paging_init();

    // Optional: initialize linear framebuffer (graphics mode) if GRUB provided it.
    // Fallback: stay in VGA text mode.
    {
        struct multiboot_tag *t = multiboot2_find_tag(mb2, MULTIBOOT_TAG_TYPE_FRAMEBUFFER);
        if (t) {
            // Parse tag with explicit offsets (Multiboot2 spec):
            //  +0 type (u32)
            //  +4 size (u32)
            //  +8 framebuffer_addr (u64)
            // +16 pitch (u32)
            // +20 width (u32)
            // +24 height (u32)
            // +28 bpp (u8)
            // +29 framebuffer_type (u8)
            uint8_t *b = (uint8_t*)t;
            uint64_t fb_phys = *(uint64_t*)(b + 8);
            uint32_t pitch = *(uint32_t*)(b + 16);
            uint32_t width = *(uint32_t*)(b + 20);
            uint32_t height = *(uint32_t*)(b + 24);
            uint8_t bpp = *(uint8_t*)(b + 28);
            uint8_t fb_type = *(uint8_t*)(b + 29);

            com_write_string(COM1_PORT, "[FB] Multiboot framebuffer tag found\n");
            com_write_string(COM1_PORT, "[FB] fb pitch=");
            { char tmp[32]; itoa((int)pitch, tmp, 10); com_write_string(COM1_PORT, tmp); }
            com_write_string(COM1_PORT, " bpp=");
            { char tmp[32]; itoa((int)bpp, tmp, 10); com_write_string(COM1_PORT, tmp); }
            com_write_string(COM1_PORT, " type=");
            { char tmp[32]; itoa((int)fb_type, tmp, 10); com_write_string(COM1_PORT, tmp); }
            com_write_string(COM1_PORT, "\n");

            // Multiboot2 RGB color info (valid when framebuffer_type==RGB)
            uint8_t red_pos = *(uint8_t*)(b + 32);
            uint8_t red_size = *(uint8_t*)(b + 33);
            uint8_t green_pos = *(uint8_t*)(b + 34);
            uint8_t green_size = *(uint8_t*)(b + 35);
            uint8_t blue_pos = *(uint8_t*)(b + 36);
            uint8_t blue_size = *(uint8_t*)(b + 37);

            // Validate usable RGB linear framebuffer
            if (fb_phys && width && height &&
                fb_type == MULTIBOOT_FRAMEBUFFER_TYPE_RGB &&
                pitch != 0 &&
                (bpp == 16 || bpp == 24 || bpp == 32) &&
                pitch >= (width * (uint32_t)(bpp / 8))) { 
                com_write_string(COM1_PORT, "[FB] RGB fields r=");
                { char tmp[32]; itoa((int)red_pos, tmp, 10); com_write_string(COM1_PORT, tmp); }
                com_write_string(COM1_PORT, "/");
                { char tmp[32]; itoa((int)red_size, tmp, 10); com_write_string(COM1_PORT, tmp); }
                com_write_string(COM1_PORT, " g=");
                { char tmp[32]; itoa((int)green_pos, tmp, 10); com_write_string(COM1_PORT, tmp); }
                com_write_string(COM1_PORT, "/");
                { char tmp[32]; itoa((int)green_size, tmp, 10); com_write_string(COM1_PORT, tmp); }
                com_write_string(COM1_PORT, " b=");
                { char tmp[32]; itoa((int)blue_pos, tmp, 10); com_write_string(COM1_PORT, tmp); }
                com_write_string(COM1_PORT, "/");
                { char tmp[32]; itoa((int)blue_size, tmp, 10); com_write_string(COM1_PORT, tmp); }
                com_write_string(COM1_PORT, "\n");

                uint64_t fb_size = (uint64_t)pitch * (uint64_t)height;
                uint64_t fb_size_aligned = (fb_size + 4095ULL) & ~4095ULL;

                com_write_string(COM1_PORT, "[FB] Mapping framebuffer via ioremap size=");
                { char tmp[32]; itoa((int)fb_size, tmp, 10); com_write_string(COM1_PORT, tmp); }
                com_write_string(COM1_PORT, " (aligned ");
                { char tmp[32]; itoa((int)fb_size_aligned, tmp, 10); com_write_string(COM1_PORT, tmp); }
                com_write_string(COM1_PORT, ") bytes\n");

                // Guarded mapping: catch framebuffer overflows (common in early gfx code)
                void *fb_virt = ioremap_guarded(fb_phys, fb_size_aligned);
                com_write_string(COM1_PORT, "[FB] ioremap returned fb_virt=");
                com_printf(COM1_PORT, "0x%08x%08x\n",
                           (uint32_t)(((uint64_t)(uintptr_t)fb_virt) >> 32),
                           (uint32_t)(((uint64_t)(uintptr_t)fb_virt) & 0xFFFFFFFFu));

                /* Sanity: framebuffer mapping must not land inside the kernel heap region. */
                if ((uint64_t)(uintptr_t)fb_virt >= 0xFFFF800000000000ULL && (uint64_t)(uintptr_t)fb_virt < 0xFFFF900000000000ULL) {
                    com_write_string(COM1_PORT, "[FB] Reject: fb_virt is inside KHEAP range; staying in text mode\n");
                    fb_virt = NULL;
                }

                /* Verify that the entire framebuffer range is mapped (debug safety). */
                if (fb_virt) {
                    uint8_t *vp = (uint8_t*)fb_virt;
                    for (uint64_t off = 0; off < fb_size; off += 4096ULL) {
                        uint64_t phys = paging_virt_to_phys((uint64_t)(uintptr_t)(vp + off));
                        if (phys == 0) {
                            com_write_string(COM1_PORT, "[FB] ERROR: framebuffer page not mapped at off=");
                            com_printf(COM1_PORT, "0x%x\n", (uint32_t)off);
                            fb_virt = NULL;
                            break;
                        }
                    }
                    if (!fb_virt) {
                        com_write_string(COM1_PORT, "[FB] Reject: framebuffer not fully mapped; staying in text mode\n");
                    }
                }

                if (fb_virt) {
                    framebuffer_t fb;
                    fb.addr = fb_virt;
                    fb.phys_addr = fb_phys;
                    fb.size_bytes = fb_size_aligned;
                    fb.width = width;
                    fb.height = height;
                    fb.pitch = pitch;
                    fb.bpp = bpp;
                    fb.fmt = FB_FMT_UNKNOWN;
                    fb.red_pos = red_pos; fb.red_mask_size = red_size;
                    fb.green_pos = green_pos; fb.green_mask_size = green_size;
                    fb.blue_pos = blue_pos; fb.blue_mask_size = blue_size;
                    VGA_SetFrameBuffer(&fb);
                    VGA_ClearFrameBuffer(0);
                    com_write_string(COM1_PORT, "[FB] Graphics framebuffer enabled\n");
                    com_write_string(COM1_PORT, "[FB] Framebuffer mapping verified OK\n");
                } else {
                    com_write_string(COM1_PORT, "[FB] WARNING: ioremap() failed; staying in text mode\n");
                }
            } else {
                com_write_string(COM1_PORT, "[FB] No usable RGB framebuffer; staying in text mode\n");
            }
        } else {
            com_write_string(COM1_PORT, "[FB] No framebuffer tag; staying in text mode\n");
        }
    }
    
    com_write_string(COM1_PORT, "[MEM] Step 4: Extending identity mapping to all RAM...\n");
    early_identity_map_all();
    
    /* ADD THESE DEBUG LINES */
    com_write_string(COM1_PORT, "[MEM] Step 4 complete! early_identity_map_all() returned successfully\n");
    com_write_string(COM1_PORT, "[MEM] Memory system initialized successfully!\n");
    com_write_string(COM1_PORT, "[MEM] Kernel heap is now available via kmalloc()\n");
    com_write_string(COM1_PORT, "[MEM] Returning from memory_system_init()...\n");
}