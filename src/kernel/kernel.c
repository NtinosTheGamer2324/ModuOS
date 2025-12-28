//kernel.c
#include "moduos/kernel/COM/com.h"
#include "moduos/drivers/graphics/VGA.h"
#include "moduos/kernel/macros.h"
#include "moduos/kernel/shell/zenith4.h"
#include "moduos/kernel/exec.h"
#include "moduos/drivers/input/input.h"
#include "moduos/drivers/input/ps2/ps2.h"
#include "moduos/fs/devfs.h"
#include "moduos/kernel/panic.h"
#include "moduos/kernel/interrupts/idt.h"
#include "moduos/kernel/interrupts/irq.h"
#include "moduos/kernel/interrupts/pic.h"
#include "moduos/kernel/interrupts/timer.h"
#include "moduos/kernel/interrupts/fault.h"
#include "moduos/drivers/Drive/ATA/ata.h"
#include "moduos/drivers/Drive/SATA/SATA.h"
#include "moduos/drivers/Drive/SATA/AHCI.h"
#include "moduos/fs/fs.h"
#include "moduos/drivers/power/ACPI.h"
#include "moduos/kernel/kernel.h"
#include "moduos/kernel/memory/memory.h"
#include "moduos/kernel/memory/phys.h"
#include "moduos/kernel/process/process.h"
#include "moduos/kernel/syscall/syscall.h"
#include "moduos/drivers/PCI/pci.h"
#include "moduos/fs/fd.h" 
#include "moduos/drivers/Drive/vDrive.h"
#include "moduos/drivers/USB/usb.h"
#include "moduos/kernel/multiboot2.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/memory/paging.h"
#include "moduos/kernel/bootscreen.h"
#include "moduos/kernel/sqrm.h"
#include "moduos/kernel/audio.h"
#include "moduos/kernel/mdinit.h"
#include <stdint.h>

int acpi_initialized;
int boot_drive_index = -1;  // Global to store boot drive
int boot_drive_slot = -1;
extern uint64_t multiboot_info_ptr;

// ------------------ HELPER FUNCTIONS ------------------
static void com_print_hex64(uint64_t v) {
    const char hex[] = "0123456789abcdef";
    char buf[19];
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 16; ++i) {
        buf[2 + i] = hex[(v >> ((15 - i) * 4)) & 0xF];
    }
    buf[18] = 0;
    com_write_string(COM1_PORT, buf);
}

static inline void fpu_init(void) {
    __asm__ volatile (
        "fninit\n"            // init x87 FPU
        "mov %%cr0, %%rax\n"
        "and $~0x4, %%rax\n"  // clear EM (allow FPU instructions)
        "mov %%rax, %%cr0\n"
        "mov %%cr4, %%rax\n"
        "or $0x600, %%rax\n"  // enable OSFXSR + OSXMMEXCPT
        "mov %%rax, %%cr4\n"
        :
        :
        : "rax", "memory"
    );
}

static void com_print_dec64(uint64_t v) {
    char tmp[32];
    int pos = 0;
    if (v == 0) { 
        com_write_string(COM1_PORT, "0"); 
        return; 
    }
    while (v > 0 && pos < 31) {
        tmp[pos++] = '0' + (v % 10);
        v /= 10;
    }
    for (int i = pos - 1; i >= 0; i--) {
        char c[2] = { tmp[i], 0 };
        com_write_string(COM1_PORT, c);
    }
}

// ------------------ INIT MOVED ------------------
// Boot/init code moved to src/kernel/mdinit.c
static int detect_boot_drive(void) {
    com_write_string(COM1_PORT, "[INFO] Scanning drives for boot marker: /ModuOS/System64/mdsys.sqr\n");
    
    // Get total vDrive count
    int drive_count = vdrive_get_count();
    
    if (drive_count == 0) {
        COM_LOG_WARN(COM1_PORT, "No drives available for boot detection!");
        return -1;
    }
    
    com_write_string(COM1_PORT, "[INFO] Scanning ");
    char count_str[4];
    count_str[0] = '0' + drive_count;
    count_str[1] = ' ';
    count_str[2] = '\0';
    com_write_string(COM1_PORT, count_str);
    com_write_string(COM1_PORT, "vDrives...\n");
    
    // Scan all vDrives
    for (int vdrive_id = 0; vdrive_id < VDRIVE_MAX_DRIVES; vdrive_id++) {
        vdrive_t *drive = vdrive_get(vdrive_id);
        
        if (!drive || !vdrive_is_ready(vdrive_id)) {
            continue;
        }
        
        com_write_string(COM1_PORT, "[INFO] Checking vDrive ");
        char id_str[4];
        id_str[0] = '0' + vdrive_id;
        id_str[1] = ' ';
        id_str[2] = '(';
        id_str[3] = '\0';
        com_write_string(COM1_PORT, id_str);
        com_write_string(COM1_PORT, vdrive_get_backend_string(drive->backend));
        com_write_string(COM1_PORT, " - ");
        com_write_string(COM1_PORT, drive->model);
        com_write_string(COM1_PORT, ")...\n");
        
        // Try to mount based on drive type
        int slot = -1;
        if (drive->type == VDRIVE_TYPE_ATA_ATAPI || 
            drive->type == VDRIVE_TYPE_SATA_OPTICAL) {
            
            com_write_string(COM1_PORT, "[INFO] Optical drive detected, trying ISO9660...\n");
            slot = fs_mount_drive(vdrive_id, 0, FS_TYPE_ISO9660);
            
            if (slot < 0) {
                com_write_string(COM1_PORT, "[INFO] ISO9660 mount failed (slot=");
                char tmp[16];
                itoa(slot, tmp, 10);
                com_write_string(COM1_PORT, tmp);
                com_write_string(COM1_PORT, ")\n");
                continue;  // Skip this optical drive
            }
            com_write_string(COM1_PORT, "[INFO] ISO9660 mounted successfully\n");
        } else {
            // For hard drives, use auto-detection
            com_write_string(COM1_PORT, "[INFO] Hard drive detected, auto-detecting filesystem...\n");
            slot = fs_mount_drive(vdrive_id, 0, FS_TYPE_UNKNOWN);
            
            if (slot < 0) {
                com_write_string(COM1_PORT, "[INFO] vDrive ");
                id_str[0] = '0' + vdrive_id;
                id_str[1] = '\0';
                com_write_string(COM1_PORT, id_str);
                com_write_string(COM1_PORT, " mount failed (slot=");
                char tmp[16];
                itoa(slot, tmp, 10);
                com_write_string(COM1_PORT, tmp);
                com_write_string(COM1_PORT, ")\n");
                continue;
            }
        }
        
        if (slot < 0) {
            com_write_string(COM1_PORT, "[INFO] vDrive ");
            id_str[0] = '0' + vdrive_id;
            id_str[1] = '\0';
            com_write_string(COM1_PORT, id_str);
            com_write_string(COM1_PORT, " No valid filesystem\n");
            continue;
        }
        
        fs_mount_t* mount = fs_get_mount(slot);
        if (!mount || !mount->valid) {
            com_write_string(COM1_PORT, "[INFO] vDrive ");
            id_str[0] = '0' + vdrive_id;
            id_str[1] = '\0';
            com_write_string(COM1_PORT, id_str);
            com_write_string(COM1_PORT, " Mount invalid\n");
            fs_unmount_slot(slot);
            continue;
        }
        
        const char* fs_name = fs_type_name(mount->type);
        com_write_string(COM1_PORT, "[INFO] vDrive ");
        id_str[0] = '0' + vdrive_id;
        id_str[1] = '\0';
        com_write_string(COM1_PORT, id_str);
        com_write_string(COM1_PORT, " Found ");
        com_write_string(COM1_PORT, fs_name);
        com_write_string(COM1_PORT, " filesystem\n");
        
        // Check for boot marker
        com_write_string(COM1_PORT, "[INFO] Checking for boot marker...\n");
        if (fs_file_exists(mount, "/ModuOS/System64/mdsys.sqr")) {
            com_write_string(COM1_PORT, "[OK] Boot drive found on vDrive ");
            id_str[0] = '0' + vdrive_id;
            id_str[1] = '\0';
            com_write_string(COM1_PORT, id_str);
            com_write_string(COM1_PORT, "\n");
            
            com_write_string(COM1_PORT, "     Model: ");
            com_write_string(COM1_PORT, drive->model);
            com_write_string(COM1_PORT, "\n");
            
            com_write_string(COM1_PORT, "     Type:  ");
            com_write_string(COM1_PORT, vdrive_get_type_string(drive->type));
            com_write_string(COM1_PORT, "\n");
            
            com_write_string(COM1_PORT, "     FS:    ");
            com_write_string(COM1_PORT, fs_name);
            com_write_string(COM1_PORT, "\n");
            
            com_write_string(COM1_PORT, "     Slot:  ");
            id_str[0] = '0' + slot;
            id_str[1] = '\0';
            com_write_string(COM1_PORT, id_str);
            com_write_string(COM1_PORT, "\n");
            
            // CRITICAL: Store both the vDrive ID and the mount slot
            boot_drive_slot = slot;
            
            // Keep mount active - don't unmount boot drive!
            
            return vdrive_id;
        }
        
        com_write_string(COM1_PORT, "[INFO] vDrive ");
        id_str[0] = '0' + vdrive_id;
        id_str[1] = '\0';
        com_write_string(COM1_PORT, id_str);
        com_write_string(COM1_PORT, " Boot marker not found\n");
        
        // IMPORTANT: Unmount drives that don't have the boot marker
        // This prevents non-boot drives from cluttering the mount table
        com_write_string(COM1_PORT, "[INFO] Unmounting non-boot drive\n");
        fs_unmount_slot(slot);
    }
    
    COM_LOG_PANIC(COM1_PORT, "No boot drive detected!");
    panic(
        "No boot drive detected",
        "The system cannot continue without the boot drive.\n"
        "This may be due to booting from a SATAPI Device ",
        " - SATAPI Is not fully fixed, please use SATA or ATAPI to boot.\n"
        " - If that is not the case, please contact support.",
        "BOOT",
        "BOOT_DRIVE_NOT_FOUND",
        6
    );

    return -1;
}

int kernel_get_boot_drive(void) {
    return boot_drive_index;
}

int kernel_get_boot_slot(void) {
    return boot_drive_slot;
}

fs_mount_t* kernel_get_boot_mount(void) {
    if (boot_drive_slot < 0) {
        return NULL;
    }
    return fs_get_mount(boot_drive_slot);
}

// ------------------ MEMORY TESTS ------------------
void memory_smoke_test(void) {
    COM_LOG_INFO(COM1_PORT, "Running memory smoke tests...");
    
    // Test 1: Simple allocation
    com_write_string(COM1_PORT, "[TEST] Allocating 8 KiB...\n");
    void *p1 = kmalloc(8192);
    if (p1) {
        COM_LOG_OK(COM1_PORT, "8 KiB allocation successful");
        
        // Write to memory to verify it's usable
        char *test = (char *)p1;
        for (int i = 0; i < 8192; i++) {
            test[i] = (char)(i & 0xFF);
        }
        
        // Verify writes
        int verify_ok = 1;
        for (int i = 0; i < 8192; i++) {
            if (test[i] != (char)(i & 0xFF)) {
                verify_ok = 0;
                break;
            }
        }
        
        if (verify_ok) {
            COM_LOG_OK(COM1_PORT, "Memory write/read test passed");
        } else {
            COM_LOG_ERROR(COM1_PORT, "Memory write/read test FAILED");
        }
        
        com_write_string(COM1_PORT, "[TEST] Freeing memory...\n");
        kfree(p1);
        COM_LOG_OK(COM1_PORT, "Memory freed");
    } else {
        COM_LOG_ERROR(COM1_PORT, "kmalloc failed!");
        return;
    }
    
    // Test 2: Multiple allocations
    com_write_string(COM1_PORT, "[TEST] Multiple allocations...\n");
    void *ptrs[5];
    int alloc_ok = 1;
    
    for (int i = 0; i < 5; i++) {
        ptrs[i] = kmalloc(1024);
        if (!ptrs[i]) {
            alloc_ok = 0;
            break;
        }
    }
    
    if (alloc_ok) {
        COM_LOG_OK(COM1_PORT, "Multiple allocations successful");
        for (int i = 0; i < 5; i++) {
            kfree(ptrs[i]);
        }
    } else {
        COM_LOG_ERROR(COM1_PORT, "Multiple allocations FAILED");
    }
    
    // Test 3: kzalloc (zeroed allocation)
    com_write_string(COM1_PORT, "[TEST] Testing kzalloc...\n");
    void *zero_mem = kzalloc(2048);
    if (zero_mem) {
        uint8_t *bytes = (uint8_t *)zero_mem;
        int all_zero = 1;
        for (int i = 0; i < 2048; i++) {
            if (bytes[i] != 0) {
                all_zero = 0;
                break;
            }
        }
        
        if (all_zero) {
            COM_LOG_OK(COM1_PORT, "kzalloc zeroes memory correctly");
        } else {
            COM_LOG_ERROR(COM1_PORT, "kzalloc FAILED to zero memory");
        }
        /* Skip free
        kfree(zero_mem);
        */
    } else {
        COM_LOG_ERROR(COM1_PORT, "kzalloc allocation failed");
    }
    
    // Print memory statistics
    com_write_string(COM1_PORT, "[INFO] Total physical frames: ");
    com_print_dec64(phys_total_frames());
    com_write_string(COM1_PORT, "\n");
    
    COM_LOG_OK(COM1_PORT, "Memory smoke tests complete!");
    com_write_string(COM1_PORT, "[INFO] Note: kfree() tests skipped (will be tested later)\n");
}

void shell_process_entry(void) {
    com_write_string(COM1_PORT, "[USER-PROC] Initial user process started (login)\n");

    com_write_string(COM1_PORT, "[USER-PROC] login exited; falling back to shell\n");
    zenith4_start();

    com_write_string(COM1_PORT, "[USER-PROC] Shell exited\n");
    process_exit(0);
}

static void kernel_post_init_run_shell(void) {
    // Re-enable text output (we may have locked it while splash screen was visible).
    VGA_SetSplashLock(false);
    bootscreen_overlay_set_enabled(0);


    com_write_string(COM1_PORT, "\n=== BOOT COMPLETE ===\n");

    com_write_string(COM1_PORT, "[KERNEL] Creating shell as a process...\n");

    process_t *shell_proc = process_create("shell", shell_process_entry, 10);
    if (!shell_proc) {
        COM_LOG_ERROR(COM1_PORT, "Failed to create shell process!");
        trigger_no_shell_panic();
    }

    COM_LOG_OK(COM1_PORT, "Shell process created successfully");
    com_write_string(COM1_PORT, "[KERNEL] Entering idle loop, scheduler will run shell...\n");

    __asm__ volatile("sti");
    while (1) {
        schedule();
        __asm__ volatile("hlt");
    }
}

static void kernel_try_enable_framebuffer(uint64_t mb2_ptr) {
    struct multiboot_tag *t = multiboot2_find_tag((void*)(uintptr_t)mb2_ptr, MULTIBOOT_TAG_TYPE_FRAMEBUFFER);
    if (!t) {
        com_write_string(COM1_PORT, "[FB] No MULTIBOOT_TAG_TYPE_FRAMEBUFFER tag found\n");
        return;
    }

    // Parse tag with explicit offsets (Multiboot2 spec)
    uint8_t *b = (uint8_t*)t;
    uint64_t fb_phys = *(uint64_t*)(b + 8);
    uint32_t pitch   = *(uint32_t*)(b + 16);
    uint32_t width   = *(uint32_t*)(b + 20);
    uint32_t height  = *(uint32_t*)(b + 24);
    uint8_t  bpp     = *(uint8_t*)(b + 28);
    uint8_t  fb_type = *(uint8_t*)(b + 29);

    // Multiboot2 RGB field info (valid when framebuffer_type==RGB)
    uint8_t red_pos    = *(uint8_t*)(b + 32);
    uint8_t red_size   = *(uint8_t*)(b + 33);
    uint8_t green_pos  = *(uint8_t*)(b + 34);
    uint8_t green_size = *(uint8_t*)(b + 35);
    uint8_t blue_pos   = *(uint8_t*)(b + 36);
    uint8_t blue_size  = *(uint8_t*)(b + 37);

    com_write_string(COM1_PORT, "[FB] Framebuffer tag:\n");
    com_write_string(COM1_PORT, "[FB]   fb_phys="); com_print_hex64(fb_phys);
    com_printf(COM1_PORT, " pitch=%u width=%u height=%u bpp=%u type=%u\n",
               pitch, width, height, (unsigned)bpp, (unsigned)fb_type);
    com_printf(COM1_PORT, "[FB]   RGB fields: r=%u/%u g=%u/%u b=%u/%u\n",
               (unsigned)red_pos, (unsigned)red_size,
               (unsigned)green_pos, (unsigned)green_size,
               (unsigned)blue_pos, (unsigned)blue_size);

    if (!fb_phys || !width || !height) {
        com_write_string(COM1_PORT, "[FB] Reject: missing fb_phys/width/height\n");
        return;
    }
    if (fb_type != MULTIBOOT_FRAMEBUFFER_TYPE_RGB) {
        com_write_string(COM1_PORT, "[FB] Reject: framebuffer_type != RGB\n");
        return;
    }
    if (pitch == 0) {
        com_write_string(COM1_PORT, "[FB] Reject: pitch == 0\n");
        return;
    }
    if (!(bpp == 16 || bpp == 24 || bpp == 32)) {
        com_write_string(COM1_PORT, "[FB] Reject: unsupported bpp\n");
        return;
    }
    if (pitch < (width * (uint32_t)(bpp / 8))) {
        com_write_string(COM1_PORT, "[FB] Reject: pitch < width*(bpp/8)\n");
        return;
    }

    uint64_t fb_size = (uint64_t)pitch * (uint64_t)height;
    /* Map a page-aligned size so scrolling/memmove never hits an unmapped tail page. */
    uint64_t fb_size_aligned = (fb_size + 4095ULL) & ~4095ULL;

    com_write_string(COM1_PORT, "[FB] Mapping framebuffer via ioremap size=");
    com_print_dec64(fb_size);
    com_write_string(COM1_PORT, " bytes (aligned ");
    com_print_dec64(fb_size_aligned);
    com_write_string(COM1_PORT, ")\n");

    void *fb_virt = ioremap(fb_phys, fb_size_aligned);
    com_write_string(COM1_PORT, "[FB] ioremap returned fb_virt=");
    com_print_hex64((uint64_t)(uintptr_t)fb_virt);
    com_write_string(COM1_PORT, "\n");

    if (!fb_virt) {
        com_write_string(COM1_PORT, "[FB] Reject: ioremap failed\n");
        return;
    }

    /* Sanity: framebuffer mapping must not land inside the kernel heap region. */
    if ((uint64_t)(uintptr_t)fb_virt >= 0xFFFF800000000000ULL && (uint64_t)(uintptr_t)fb_virt < 0xFFFF900000000000ULL) {
        com_write_string(COM1_PORT, "[FB] Reject: fb_virt is inside KHEAP range; disabling framebuffer\n");
        return;
    }

    uint64_t vtophys = paging_virt_to_phys((uint64_t)(uintptr_t)fb_virt);
    com_write_string(COM1_PORT, "[FB] ioremap virt_to_phys=");
    com_print_hex64(vtophys);
    com_write_string(COM1_PORT, "\n");

    framebuffer_t fb;
    memset(&fb, 0, sizeof(fb));
    fb.addr = fb_virt;
    fb.width = width;
    fb.height = height;
    fb.pitch = pitch;
    fb.bpp = bpp;
    fb.fmt = (bpp == 32) ? FB_FMT_XRGB8888 : (bpp == 16) ? FB_FMT_RGB565 : FB_FMT_UNKNOWN;

    // Important: provide channel packing info so VGA_ClearFrameBuffer can pack correctly
    fb.red_pos = red_pos; fb.red_mask_size = red_size;
    fb.green_pos = green_pos; fb.green_mask_size = green_size;
    fb.blue_pos = blue_pos; fb.blue_mask_size = blue_size;

    VGA_SetFrameBuffer(&fb);
    com_write_string(COM1_PORT, "[FB] VGA_SetFrameBuffer done\n");
}

static void gfx_sleep_ms(uint64_t ms) {
    /* PIT runs at 100Hz (pit_init(100)), so 1 tick = 10ms */
    const uint64_t start = get_system_ticks();
    const uint64_t ticks = (ms + 9) / 10;
    while ((get_system_ticks() - start) < ticks) {
        __asm__ volatile("hlt");
    }
}

static void kernel_post_init_graphics_test(uint64_t mb2_ptr) {
    com_write_string(COM1_PORT, "\n=== GRAPHICS TEST MODE ===\n");

    if (VGA_GetFrameBufferMode() != FB_MODE_GRAPHICS) {
        kernel_try_enable_framebuffer(mb2_ptr);
    }

    if (VGA_GetFrameBufferMode() != FB_MODE_GRAPHICS) {
        com_write_string(COM1_PORT, "[GFX] No framebuffer; continuing with text-mode shell\n");
        kernel_post_init_run_shell();
        return;
    }

    __asm__ volatile("sti");

    /* Initialize graphics console and drop into the normal shell */
    VGA_ResetTextColor();
    VGA_Clear();
    VGA_Write("\\cg[GRAPHICS] Framebuffer console enabled (bitmap font).\\rr\n");

    kernel_post_init_run_shell();
}

static int cmdline_has_token(const char *cmdline, const char *token) {
    if (!cmdline || !token || !token[0]) return 0;
    size_t tlen = strlen(token);
    for (size_t i = 0; cmdline[i]; i++) {
        // token must start at beginning or after whitespace
        if (i > 0 && cmdline[i-1] != ' ' && cmdline[i-1] != '\t') continue;
        // match token
        size_t j = 0;
        while (j < tlen && cmdline[i + j] == token[j]) j++;
        if (j != tlen) continue;
        // token must end at end or whitespace
        char end = cmdline[i + j];
        if (end == 0 || end == ' ' || end == '\t') return 1;
    }
    return 0;
}

// ------------------ KERNEL MAIN (DISPATCH) ------------------
void kernel_main(uint64_t mb2_ptr)
{
    // Boot arg selection:
    //  - "gfx-test" => force graphics test path
    //  - otherwise  => normal text-mode boot
    const char *cmdline = NULL;
    struct multiboot_tag *t = multiboot2_find_tag((void*)(uintptr_t)mb2_ptr, MULTIBOOT_TAG_TYPE_CMDLINE);
    if (t) {
        struct multiboot_tag_string *s = (struct multiboot_tag_string*)t;
        cmdline = s->string;
    }

    com_write_string(COM1_PORT, "[BOOT] cmdline: ");
    if (cmdline) {
        // Print cmdline (bounded)
        for (int i = 0; i < 160 && cmdline[i]; i++) {
            char c[2] = { cmdline[i], 0 };
            com_write_string(COM1_PORT, c);
        }
    } else {
        com_write_string(COM1_PORT, "<none>");
    }
    com_write_string(COM1_PORT, "\n");

    int want_gfx_test = (cmdline && cmdline_has_token(cmdline, "gfx-test")) ? 1 : 0;
    if (want_gfx_test) com_write_string(COM1_PORT, "[BOOT] gfx-test requested\n");
    else com_write_string(COM1_PORT, "[BOOT] normal boot\n");

    // Run full init ONCE (running it twice corrupts the multiboot info area)
    mdinit_run(mb2_ptr);

    // For gfx-test, try to (re)enable framebuffer after full init in case something reset VGA state.
    if (want_gfx_test) {
        com_write_string(COM1_PORT, "[BOOT] Enabling framebuffer for gfx-test...\n");
        kernel_try_enable_framebuffer(mb2_ptr);
        com_write_string(COM1_PORT, "[BOOT] VGA framebuffer mode right after enable: ");
        com_write_string(COM1_PORT, (VGA_GetFrameBufferMode() == FB_MODE_GRAPHICS) ? "GRAPHICS\n" : "TEXT\n");
    }

    com_write_string(COM1_PORT, "[BOOT] VGA framebuffer mode after init: ");
    com_write_string(COM1_PORT, (VGA_GetFrameBufferMode() == FB_MODE_GRAPHICS) ? "GRAPHICS\n" : "TEXT\n");

    /*
     * IMPORTANT: kernel_main must never return to the bootstrap.
     * If we return, the 64-bit entry stub would resume execution after the call site
     * and may run into undefined bytes, causing random exceptions.
    
    if (want_gfx_test) {
        kernel_post_init_graphics_test(mb2_ptr);
    } else {
        kernel_post_init_run_shell();
    }
    */
    VGA_Clear();
    VGA_Writef("This is free software, distributed without any warranty.\n");
    VGA_Writef("Copyright (c) New Technologies Software — ModuOS Login\n");
    exec("/Apps/login.sqr");
    kernel_post_init_run_shell();
    /* Should never be reached */
    for (;;) {
        __asm__ volatile("hlt");
    }
}
