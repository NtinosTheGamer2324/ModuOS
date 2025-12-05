//kernel.c
#include "moduos/kernel/COM/com.h"
#include "moduos/drivers/graphics/VGA.h"
#include "moduos/kernel/macros.h"
#include "moduos/kernel/shell/zenith4.h"
#include "moduos/drivers/ps2/ps2.h"
#include "moduos/kernel/panic.h"
#include "moduos/kernel/interrupts/idt.h"
#include "moduos/kernel/interrupts/irq.h"
#include "moduos/kernel/interrupts/pic.h"
#include "moduos/kernel/interrupts/timer.h"
#include "moduos/kernel/interrupts/fault.h"
#include "moduos/drivers/Drive/ATA/ata.h"
#include "moduos/fs/fs.h"
#include "moduos/drivers/power/ACPI.h"
#include "moduos/kernel/kernel.h"
#include "moduos/kernel/memory/memory.h"
#include "moduos/kernel/memory/phys.h"
#include "moduos/kernel/process/process.h"
#include "moduos/kernel/syscall/syscall.h"
#include "moduos/fs/fd.h" 
#include <stdint.h>

int acpi_initialized;
int boot_drive_index = -1;  // Global to store boot drive
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

// ------------------ BOOT DRIVE DETECTION ------------------
// ------------------ BOOT DRIVE DETECTION ------------------
static int detect_boot_drive(void) {
    com_write_string(COM1_PORT, "[INFO] Scanning drives for boot marker: /ModuOS/System64/mdsys.sqr\n");
    
    int max_drives = 4;
    
    for (int drive = 0; drive < max_drives; drive++) {
        com_write_string(COM1_PORT, "[INFO] Checking drive ");
        char drive_str[4];
        drive_str[0] = '0' + drive;
        drive_str[1] = '.';
        drive_str[2] = '.';
        drive_str[3] = '\0';
        com_write_string(COM1_PORT, drive_str);
        com_write_string(COM1_PORT, ".\n");
        
        // Mount the drive with auto-detection (returns slot ID or negative on error)
        int slot = fs_mount_drive(drive, 0, FS_TYPE_UNKNOWN);
        
        if (slot < 0) {
            com_write_string(COM1_PORT, "[INFO] Drive ");
            com_write_string(COM1_PORT, drive_str);
            com_write_string(COM1_PORT, " No valid filesystem\n");
            continue;
        }
        
        // Get the mount handle from the slot
        fs_mount_t* mount = fs_get_mount(slot);
        if (!mount || !mount->valid) {
            com_write_string(COM1_PORT, "[INFO] Drive ");
            com_write_string(COM1_PORT, drive_str);
            com_write_string(COM1_PORT, " Mount invalid\n");
            fs_unmount_slot(slot);
            continue;
        }
        
        const char* fs_name = fs_type_name(mount->type);
        com_write_string(COM1_PORT, "[INFO] Drive ");
        com_write_string(COM1_PORT, drive_str);
        com_write_string(COM1_PORT, " Found ");
        com_write_string(COM1_PORT, fs_name);
        com_write_string(COM1_PORT, " filesystem\n");
        
        // ONLY check if file exists - DO NOT try to load it!
        if (fs_file_exists(mount, "/ModuOS/System64/mdsys.sqr")) {
            com_write_string(COM1_PORT, "[OK] Boot drive found on drive ");
            com_write_string(COM1_PORT, drive_str);
            com_write_string(COM1_PORT, " (");
            com_write_string(COM1_PORT, fs_name);
            com_write_string(COM1_PORT, ")\n");
            
            // Just unmount and return - don't load anything!
            fs_unmount_slot(slot);
            return drive;
        }
        
        com_write_string(COM1_PORT, "[INFO] Drive ");
        com_write_string(COM1_PORT, drive_str);
        com_write_string(COM1_PORT, " Boot marker not found\n");
        fs_unmount_slot(slot);
    }
    
    COM_LOG_WARN(COM1_PORT, "No boot drive detected!");
    return -1;
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
        
        /* Skip kfree for now - we'll test it later
        com_write_string(COM1_PORT, "[TEST] Freeing memory...\n");
        kfree(p1);
        COM_LOG_OK(COM1_PORT, "Memory freed");
        */
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
        /* Skip freeing for now
        for (int i = 0; i < 5; i++) {
            kfree(ptrs[i]);
        }
        */
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

// ------------------ INTERRUPTS -------------------
static void Interrupts_Init(void)
{
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


// ------------------ DEVICE INIT ------------------
static void device_Init(void)
{
    COM_LOG_INFO(COM1_PORT, "Initializing PS/2");
    if (ps2_init() != 0) {
        COM_LOG_WARN(COM1_PORT, "PS/2 did not respond! (This happens on some VMs)");
    } else {
        COM_LOG_OK(COM1_PORT, "PS/2 initialized");
    }

    irq_install_handler(1, keyboard_irq_handler);

    COM_LOG_INFO(COM1_PORT, "Initializing ATA Controller / Drives");
    int ata_status = ata_init();
    if (ata_status == 0) {
        COM_LOG_OK(COM1_PORT, "ATA initialized successfully, drives detected");
    } else if (ata_status == -1) {
        COM_LOG_WARN(COM1_PORT, "ATA controller present, but no drives found");
    } else if (ata_status == -2) {
        COM_LOG_ERROR(COM1_PORT, "ATA controller not responding!");
        trigger_panic_doata();
    }
}

// ------------------ BOOT SCREEN ------------------
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

// ------------------ INIT ------------------
static void init(uint64_t mb2_ptr_init)
{
    VGA_Clear();
    loading();
    
    // Initialize COM ports for debug output
    if (com_init(COM1_PORT) == 0) {
        COM_LOG_OK(COM1_PORT, "COM1 successfully initialized");
    }
    if (com_init(COM2_PORT) == 0) {
        COM_LOG_OK(COM2_PORT, "COM2 successfully initialized");
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
    
    // Run memory smoke tests to verify everything works
    memory_smoke_test();
    com_write_string(COM1_PORT, "=== MEMORY INITIALIZATION COMPLETE ===\n\n");

    // Initialize devices
    device_Init();

    // Initialize filesystem layer
    COM_LOG_INFO(COM1_PORT, "Initializing filesystem layer");
    fs_init();
    fd_init();
    COM_LOG_OK(COM1_PORT, "Filesystem layer initialized");

    // Initialize ACPI
    if (acpi_init() == 0) {
        acpi_initialized = 1;
        COM_LOG_OK(COM1_PORT, "ACPI initialized");
    } else {
        COM_LOG_WARN(COM1_PORT, "ACPI initialization failed");
    }

    // Initialize process management system
    COM_LOG_INFO(COM1_PORT, "Initializing process management");
    process_init();
    scheduler_init();
    syscall_init();
    COM_LOG_OK(COM1_PORT, "Process management initialized");

    // Detect boot drive after all devices are initialized
    boot_drive_index = detect_boot_drive();
    
    // Clear screen and show boot logo again
    VGA_Clear();
    loading();
}

void shell_process_entry(void) {
    com_write_string(COM1_PORT, "[SHELL-PROC] Shell process started!\n");
    
    // Call your existing shell start function
    zenith4_start();
    
    // If shell ever exits (shouldn't happen), log it
    com_write_string(COM1_PORT, "[SHELL-PROC] Shell exited!\n");
    
    // Exit cleanly
    process_exit(0);
}

// ------------------ KERNEL MAIN ------------------
void kernel_main(uint64_t mb2_ptr)
{
    init(mb2_ptr);
    
    com_write_string(COM1_PORT, "\n=== BOOT COMPLETE ===\n");
    com_write_string(COM1_PORT, "[KERNEL] Creating shell as a process...\n");
    
    // CRITICAL FIX: Create shell as a real process instead of running it directly
    process_t *shell_proc = process_create("shell", shell_process_entry, 10);
    
    if (!shell_proc) {
        COM_LOG_ERROR(COM1_PORT, "Failed to create shell process!");
        trigger_no_shell_panic();
    }
    
    COM_LOG_OK(COM1_PORT, "Shell process created successfully");
    
    // Now just let the scheduler handle everything
    // The idle process will run, and when timer ticks, shell will get scheduled
    com_write_string(COM1_PORT, "[KERNEL] Entering idle loop, scheduler will run shell...\n");
    
    // Enable interrupts and enter idle loop
    __asm__ volatile("sti");
    
    while (1) {
        // Let scheduler do its job
        schedule();
        __asm__ volatile("hlt");
    }
}
