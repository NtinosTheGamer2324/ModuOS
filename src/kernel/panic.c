#include "moduos/kernel/panic.h"
#include "moduos/drivers/graphics/VGA.h"
#include "moduos/kernel/shell/zenith4.h"
#include "moduos/drivers/power/ACPI.h"
#include "moduos/drivers/Time/RTC.h"

// Shared boilerplate
static void panic_header(const char* title)
{
    panicer_close_shell4();
    VGA_Clear();
    VGA_Write("\\cr   XXX  XXX           XXXXX     XXX    XX   XX  XXXX   XXXX  \\rr\n");
    VGA_Write("\\cr   XXX  XXX           XXXXX   XX   XX  XXX  XX  XXXX  XXXXX  \\rr\n");
    VGA_Write("\\cr   XXX  XXX           XX   X  XXXXXXX  XXXX XX   XX   XX     \\rr\n");
    VGA_Write("\\cr                      XXXXX   XX   XX  XX  XXX   XX   XX     \\rr\n");
    VGA_Write("\\cr  XXXXXXXXXX          XXX     XX   XX  XX   XX  XXXX  XXXXX  \\rr\n");
    VGA_Write("\\crXX          XX        XX      XX   XX  XX   XX  XXXX   XXXX  \\rr\n");
    VGA_Write(title);
    VGA_Write("\n\n");
}

/**
 * Generic panic function
 * @param title        The panic title
 * @param message      Detailed message explaining the problem
 * @param tips         Optional troubleshooting tips (can be NULL)
 * @param err_cat      Error category string (e.g., "DEV")
 * @param err_code     Specific error code string (e.g., "ATA_DEV_NONE")
 * @param reboot_delay Seconds to wait before reboot
 */
void panic(const char* title, const char* message, const char* tips, const char* err_cat, const char* err_code, int reboot_delay)
{
    // Countdown animation
    for (int i = reboot_delay; i >= 0; i--) {
        VGA_Clear();
        panic_header(title);

        VGA_Write(message);
        VGA_Write("\n\n");

        if (tips != NULL) {
            VGA_Write("\\cyTroubleshooting Tips:\\rr\n");
            VGA_Write(tips);
            VGA_Write("\n");
        }

        VGA_Writef("ERR_CODE_CAT: %s | ERR_CODE: %s\n\n", err_cat, err_code);

        VGA_Write("\n If this issue repeats , please contact customer support at support.new-tech.com\n");

        VGA_Write("The system will reboot shortly.\n");
        VGA_Writef("Rebooting in %d seconds...\n", i);
        rtc_wait_seconds(1);
    }
    acpi_reboot();

    // fallback infinite halt in case reboot fails
    for (;;) { __asm__("hlt"); }
}

void trigger_no_shell_panic() {
    panic(
        "Zenith4 has stopped responding",
        "The system cannot continue without the shell running.\n"
        "This may be due to memory corruption.",
        " - Check if your RAM is properly connected and not loose.\n"
        " - Try a different RAM stick.",
        "SYS_PROCESS",
        "ZENITH4_NOT_RUNNING",
        6
    );
}

void trigger_panic_dodev() {
    panic(
        "No hard disks were detected during boot.",
        "The system cannot continue without at least one storage device.\n"
        "This may be due to missing drivers, hardware failure, or misconfiguration.",
        " - Check if your storage devices are properly connected.\n"
        " - Try a different hardware configuration if available.",
        "HW_DEVICE",
        "NO_MEDIUM_FOUND",
        6
    );
}

void trigger_panic_doata() {
    panic(
        "The ATA Controller did not respond during boot.",
        "The system cannot continue without a functional ATA controller.\n"
        "This may be due to missing drivers, hardware failure, or misconfiguration.",
        " - Ensure your storage controller is enabled in BIOS/UEFI.\n"
        " - Verify that drives are properly connected.\n"
        " - Try a different hardware or emulator configuration if available.",
        "HW_DEVICE",
        "ATA_CONTROLLER_UNRESPONSIVE",
        6
    );
}

void trigger_panic_dops2() {
    panic(
        "The PS/2 keyboard did not respond during boot.",
        "The system cannot continue without a keyboard device.\n"
        "This may be due to missing drivers, hardware failure, or misconfiguration.",
        " - Check if your PS/2 device is properly connected.\n"
        " - Try a different hardware configuration if available.",
        "HW_TIMEOUT",
        "PS2_DEVICE_TIMEOUT",
        6
    );
}

void trigger_panic_dofs() {
    panic(
        "No FAT32 or ISO9660 filesystem was detected during boot.",
        "The system cannot continue without a valid filesystem.\n"
        "This may be due to:\n"
        " - Missing or corrupted partition/boot sector.\n"
        " - Unsupported filesystem type.\n"
        " - Drive not properly formatted.",
        " - Verify that your disk is formatted with FAT32.\n"
        " - Ensure the drive is properly connected and detected.\n"
        " - If using an image, confirm it contains a valid ISO9660 volume.",
        "FS_LAYER",
        "FS_INIT_NO_VALID_FS",
        6
    );
}

void trigger_panic_unknown() {
    panic(
        "An unexpected system crash has occurred.",
        "The system encountered a fatal error and cannot continue.\n"
        "Please restart your computer or contact a developer.",
        "If this error persists, report it with the steps to reproduce.",
        "UNKNOWN",
        "UNKNOWN_ERROR",
        6
    );
}
