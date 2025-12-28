// mdinit.c - kernel initialization sequence extracted from kernel.c

#include "moduos/kernel/mdinit.h"
#include "moduos/arch/AMD64/gdt.h"

// We include the old kernel.c dependencies here so init behavior remains unchanged.
#include "moduos/kernel/kernel.h"
#include "moduos/kernel/COM/com.h"
#include "moduos/kernel/debug.h"
#include "moduos/kernel/memory/memory.h"

// from src/kernel/memory/memory.c
void memory_smoke_test(void);

#include "moduos/kernel/memory/paging.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/multiboot2.h"
#include "moduos/kernel/bootscreen.h"
#include "moduos/kernel/sqrm.h"
#include "moduos/kernel/blockdev.h"
#include "moduos/kernel/macros.h"
#include "moduos/kernel/panic.h"
#include "moduos/kernel/interrupts/fault.h"

#include "moduos/arch/AMD64/interrupts/idt.h"
#include "moduos/arch/AMD64/interrupts/irq.h"
#include "moduos/arch/AMD64/interrupts/pic.h"
#include "moduos/arch/AMD64/interrupts/timer.h"

#include "moduos/drivers/input/input.h"
#include "moduos/drivers/input/ps2/ps2.h"
#include "moduos/drivers/PCI/pci.h"
#include "moduos/drivers/Drive/SATA/AHCI.h"
#include "moduos/drivers/Drive/SATA/SATA.h"
#include "moduos/drivers/Drive/ATA/ata.h"
#include "moduos/drivers/Drive/vDrive.h"
#include "moduos/drivers/USB/usb.h"
#include "moduos/drivers/power/ACPI.h"
#include "moduos/drivers/graphics/VGA.h"
#include "moduos/drivers/graphics/fb_console.h"

#include "moduos/fs/fs.h"
#include "moduos/fs/fd.h"
#include "moduos/fs/devfs.h"

#include "moduos/kernel/process/process.h"
#include "moduos/kernel/syscall/syscall.h"
#include "moduos/kernel/shell/zenith4.h"

// ------------------ DEVICE INIT (split) ------------------

// Storage stack needed for boot drive detection
static void storage_early_init(void);
// Late devices that are not required for boot drive detection
static void devices_late_init(void);

// Boot drive detection
static int detect_boot_drive(void);

// Provided by kernel.c (global)
extern int boot_drive_slot;

// From kernel.c (kept local to init)
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

static inline void fpu_init(void) {
    __asm__ volatile (
        "fninit\n"
        "mov %%cr0, %%rax\n"
        "and $~0x4, %%rax\n"
        "mov %%rax, %%cr0\n"
        "mov %%cr4, %%rax\n"
        "or $0x600, %%rax\n"
        "mov %%rax, %%cr4\n"
        :
        :
        : "rax", "memory"
    );
}

static void Interrupts_Init(void) {
    COM_LOG_INFO(COM1_PORT, "Remapping PIC");
    pic_remap(0x20, 0x28);
    COM_LOG_OK(COM1_PORT, "PIC Remapped");

    COM_LOG_INFO(COM1_PORT, "Initializing CPU exception handlers");
    fault_init();
    COM_LOG_OK(COM1_PORT, "CPU exception handlers initialized");

    COM_LOG_INFO(COM1_PORT, "Initializing IRQs");
    irq_init();
    COM_LOG_OK(COM1_PORT, "Initialized IRQs");

    COM_LOG_INFO(COM1_PORT, "Initializing PIT (100Hz)");
    pit_init(100);
    COM_LOG_OK(COM1_PORT, "PIT initialized");

    COM_LOG_INFO(COM1_PORT, "Loading IDT");
    idt_load();
    COM_LOG_OK(COM1_PORT, "Loaded IDT");

    COM_LOG_INFO(COM1_PORT, "Enabling CPU interrupts");
    __asm__ volatile("sti");
    COM_LOG_OK(COM1_PORT, "CPU interrupts enabled!");
}

// Boot screen (text-mode)
static void loading(void);

// Full init sequence
static void init(uint64_t mb2_ptr_init);

// ------------------ DEVICE INIT (split) ------------------

static void storage_early_init(void) {
    // PCI Initialization
    COM_LOG_INFO(COM1_PORT, "Initializing PCI subsystem");
    pci_init();
    COM_LOG_OK(COM1_PORT, "PCI subsystem initialized");

    // AHCI Initialization
    COM_LOG_INFO(COM1_PORT, "Initializing AHCI");
    int ahci_status = ahci_init();
    if (ahci_status == 0) {
        COM_LOG_OK(COM1_PORT, "AHCI initialized successfully");
    } else {
        COM_LOG_WARN(COM1_PORT, "AHCI initialization failed or no drives found");
    }

    // SATA Initialization
    COM_LOG_INFO(COM1_PORT, "Initializing SATA subsystem");
    int sata_status = sata_init();
    if (sata_status == SATA_SUCCESS) {
        COM_LOG_OK(COM1_PORT, "SATA subsystem initialized");
    } else {
        COM_LOG_WARN(COM1_PORT, "SATA initialization failed");
    }

    vdrive_debug_registration();
    DEBUG_PAUSE(5);

    // ATA Initialization (fallback for older systems)
    COM_LOG_INFO(COM1_PORT, "Initializing ATA Controller / Drives");
    int ata_status = ata_init();
    if (ata_status == 0) {
        COM_LOG_OK(COM1_PORT, "ATA initialized successfully, drives detected");
    } else if (ata_status == -1) {
        COM_LOG_WARN(COM1_PORT, "ATA controller present, but no drives found");
    } else if (ata_status == -2) {
        COM_LOG_ERROR(COM1_PORT, "ATA controller not responding!");
    }

    // vDrive Initialization - UNIFIES ALL STORAGE
    COM_LOG_INFO(COM1_PORT, "Initializing vDrive unified drive interface");
    int vdrive_status = vdrive_init();

    if (vdrive_status == VDRIVE_SUCCESS || vdrive_status == VDRIVE_ERR_NO_DRIVES) {
        COM_LOG_OK(COM1_PORT, "vDrive subsystem initialized");

        // Print nice table of all drives
        vdrive_print_table();

        if (vdrive_get_count() == 0) {
            COM_LOG_WARN(COM1_PORT, "No storage drives available!");
            if (sata_status != SATA_SUCCESS && ata_status != 0) {
                trigger_panic_doata();  // No drives at all - panic!
            }
        }
    } else {
        COM_LOG_ERROR(COM1_PORT, "vDrive initialization failed");
    }
}

static void devices_late_init(void) {
    // DEVFS / $/dev devices
    //  - input:    $/dev/input/kbd0, $/dev/input/event0
    //  - graphics: $/dev/graphics/video0
    devfs_input_init();
    devfs_graphics_init();

    COM_LOG_INFO(COM1_PORT, "Initializing input subsystem");
    input_init();
    irq_install_handler(1, keyboard_irq_handler);

    // USB Initialization
    COM_LOG_INFO(COM1_PORT, "Initializing USB subsystem");
    usb_init();

    if (usb_has_controllers()) {
        /* Register HID class driver only if USB controllers exist.
         * On some emulators, running HID init with no controllers has triggered early faults.
         */
        // hid_init();
    } else {
        COM_LOG_INFO(COM1_PORT, "[HID] Skipping HID init (no USB controllers)");
    }

    COM_LOG_OK(COM1_PORT, "USB subsystem initialized");
}

// ------------------ VGA TEXT MODE BOOT SCREEN ------------------
static void loading(void) {
    VGA_Write("\n");
    VGA_Write("\n");
    VGA_Write("\\cp      $$\\      $$\\                 $$\\            $$$$$$\\   $$$$$$\\  \\rr\n");
    VGA_Write("\\cp      $$$\\    $$$ |                $$ |          $$  __$$\\ $$  __$$\\ \\rr\n");
    VGA_Write("\\cp      $$$$\\  $$$$ | $$$$$$\\   $$$$$$$ |$$\\   $$\\ $$ /  $$ |$$ /  \\__|\\rr\n");
    VGA_Write("\\cp      $$\\$$\\$$ $$ |$$  __$$\\ $$  __$$ |$$ |  $$ |$$ |  $$ |\\$$$$$$\\  \\rr\n");
    VGA_Write("\\cp      $$ \\$$$  $$ |$$ /  $$ |$$ /  $$ |$$ |  $$ |$$ |  $$ | \\____$$\\ \\rr\n");
    VGA_Write("\\cp      $$ |\\$  /$$ |$$ |  $$ |$$ |  $$ |$$ |  $$ |$$ |  $$ |$$\\   $$ |\\rr\n");
    VGA_Write("\\cp      $$ | \\_/ $$ |\\$$$$$$  |\\$$$$$$$ |\\$$$$$$  | $$$$$$  |\\$$$$$$  |\\rr\n");
    VGA_Write("\\cp      \\__|     \\__| \\______/  \\_______| \\______/  \\______/  \\______/ \\rr");
    VGA_Write("\\ccv0.3.9\\rr\n");
    VGA_Write("\\cp                                                                          \\rr\n");
    VGA_Write("\\cc                                   Booting...                         \\rr\n\n");
    VGA_Write("\n");
    VGA_Write("\n");
    VGA_Write("\n");
    VGA_Write("\n");
    VGA_Write("\n");
    VGA_Write("\n");
    VGA_Write("\n");
    VGA_Write("\n");
    VGA_Write("\n");
    VGA_Write("\n");
    VGA_Write("\n\\cw\\bb                      New Technologies Software 2025                            \\rr");
}

// ------------------ BOOT DRIVE DETECTION ------------------
static int detect_boot_drive(void) {
    com_write_string(COM1_PORT, "[INFO] Scanning drives for boot marker: /ModuOS/System64/mdsys.sqr\n");

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

        int slot = -1;
        if (drive->type == VDRIVE_TYPE_ATA_ATAPI || drive->type == VDRIVE_TYPE_SATA_OPTICAL) {
            com_write_string(COM1_PORT, "[INFO] Optical drive detected, trying ISO9660...\n");
            slot = fs_mount_drive(vdrive_id, 0, FS_TYPE_ISO9660);

            if (slot < 0) {
                com_write_string(COM1_PORT, "[INFO] ISO9660 mount failed (slot=");
                char tmp[16];
                itoa(slot, tmp, 10);
                com_write_string(COM1_PORT, tmp);
                com_write_string(COM1_PORT, ")\n");
                continue;
            }
            com_write_string(COM1_PORT, "[INFO] ISO9660 mounted successfully\n");
        } else {
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

            boot_drive_slot = slot;
            return vdrive_id;
        }

        com_write_string(COM1_PORT, "[INFO] vDrive ");
        id_str[0] = '0' + vdrive_id;
        id_str[1] = '\0';
        com_write_string(COM1_PORT, id_str);
        com_write_string(COM1_PORT, " Boot marker not found\n");

        com_write_string(COM1_PORT, "[INFO] Unmounting non-boot drive\n");
        fs_unmount_slot(slot);
    }

    COM_LOG_PANIC(COM1_PORT, "No boot drive detected!");
    panic(
        "No boot drive detected",
        "No drive contains /ModuOS/System64/mdsys.sqr",
        "Ensure ISO/HDD is attached and readable",
        "Boot",
        "BOOT_DRIVE",
        3
    );

    return -1;
}

// ------------------ INIT ------------------
static void init(uint64_t mb2_ptr_init) {
    // Initialize COM ports FIRST for early debug output
    com_early_init(COM1_PORT);
    com_early_init(COM2_PORT);

    COM_LOG_INFO(COM1_PORT, "COM1 and COM2 early initialized");

    VGA_Clear();
    loading();

    // Full initialization with testing (loopback test)
    COM_LOG_INFO(COM1_PORT, "Running COM1 loopback test...");
    if (com_init(COM1_PORT) == 0) {
        COM_LOG_OK(COM1_PORT, "COM1 loopback test PASSED");
    } else {
        COM_LOG_WARN(COM1_PORT, "COM1 loopback test FAILED (VirtualBox/some hardware doesn't support loopback, but port works fine for output)");
    }

    COM_LOG_INFO(COM2_PORT, "Running COM2 loopback test...");
    if (com_init(COM2_PORT) == 0) {
        COM_LOG_OK(COM2_PORT, "COM2 loopback test PASSED");
    } else {
        COM_LOG_WARN(COM2_PORT, "COM2 loopback test FAILED (VirtualBox/some hardware doesn't support loopback, but port works fine for output)");
    }

    // Initialize interrupt system
    Interrupts_Init();

    COM_LOG(COM1_PORT, "Initializing FPU");
    fpu_init();
    COM_LOG_OK(COM1_PORT, "Successfuly Initialized FPU");

    // Debug: show multiboot pointer
    com_write_string(COM1_PORT, "\n=== MEMORY INITIALIZATION ===\n");
    com_write_string(COM1_PORT, "[MEM] Multiboot2 pointer: ");
    com_print_hex64(mb2_ptr_init);
    com_write_string(COM1_PORT, "\n");

    // Initialize memory system (phys allocator + paging + heap)
    memory_system_init((void*)(uintptr_t)mb2_ptr_init);

    /* Initialize SMBIOS table pointers from Multiboot2, so bootscreen can pick correct branding. */
    md64api_init_smbios_from_mb2((void*)(uintptr_t)mb2_ptr_init);

    // If framebuffer is available already, show a splash immediately (no FS required).
    if (VGA_GetFrameBufferMode() == FB_MODE_GRAPHICS) {
        bootscreen_show_early();
        VGA_SetSplashLock(true);
    }

    com_write_string(COM1_PORT, "[KERNEL] memory_system_init() returned!\n");
    com_write_string(COM1_PORT, "[KERNEL] Starting memory smoke tests...\n");

    // Run memory smoke tests
    // Memory smoke tests (declared in memory.c)
    memory_smoke_test();
    com_write_string(COM1_PORT, "=== MEMORY INITIALIZATION COMPLETE ===\n\n");

    // Early storage stack so boot drive can be found ASAP.
    storage_early_init();

    // Register vDrives as block devices (first-party)
    blockdev_register_vdrives();

    // Initialize filesystem layer (VFS)
    COM_LOG_INFO(COM1_PORT, "Initializing filesystem layer");
    fs_init();
    fd_init();
    COM_LOG_OK(COM1_PORT, "Filesystem layer initialized");

    // Detect boot drive as soon as VFS is ready.
    com_write_string(COM1_PORT, "\n=== BOOT DRIVE DETECTION ===\n");
    boot_drive_index = detect_boot_drive();

    if (boot_drive_index >= 0) {
        com_write_string(COM1_PORT, "\n[OK] Boot drive: vDrive ");
        char id[4];
        id[0] = '0' + boot_drive_index;
        id[1] = '\0';
        com_write_string(COM1_PORT, id);
        com_write_string(COM1_PORT, "\n");
    } else {
        COM_LOG_ERROR(COM1_PORT, "Failed to detect boot drive!");
    }

    com_write_string(COM1_PORT, "=== BOOT DRIVE DETECTION COMPLETE ===\n\n");

    // Redraw splash now that the boot ISO is mounted (vendor/CPU-specific BMP).
    if (VGA_GetFrameBufferMode() == FB_MODE_GRAPHICS) {
        /* Allow bootscreen_show() to draw over the early burn-in splash. */
        VGA_SetSplashLock(false);

        if (bootscreen_show((void*)(uintptr_t)mb2_ptr_init) == 0) {
            /* After the BMP is drawn, keep the logo via overlay instead of freezing the framebuffer. */
            /* Disabled: overlay redraw has caused intermittent early-boot faults */
            bootscreen_overlay_set_enabled(0);
        }

        /* Do NOT re-enable splash lock here; let normal console output work. */
    }

    // Load kernel modules (.sqrm) as early as possible now that the boot filesystem is available.
    sqrm_load_all();

    // External filesystem drivers may have registered during SQRM loading.
    // Rescan so additional drives/partitions (e.g., ext2 HDD) become available automatically.
    fs_rescan_all();

    // Initialize ACPI
    if (acpi_init() == 0) {
        acpi_initialized = 1;
        COM_LOG_OK(COM1_PORT, "ACPI initialized");
    } else {
        COM_LOG_WARN(COM1_PORT, "ACPI initialization failed");
    }

    // Late device init (PS/2/input/USB, etc)
    devices_late_init();

    // Initialize process management system
    COM_LOG_INFO(COM1_PORT, "Initializing process management");

    /* Install kernel-owned GDT+TSS with user segments before any ring3 work. */
    amd64_gdt_init();

    process_init();
    scheduler_init();
    syscall_init();
    COM_LOG_OK(COM1_PORT, "Process management initialized");

}

void mdinit_run(uint64_t mb2_ptr) {
    init(mb2_ptr);
}
