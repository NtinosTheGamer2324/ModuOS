//shell.c
#include "moduos/kernel/shell/zenith4.h"
#include "moduos/kernel/shell/legacy/MSDOS.h"
#include "moduos/fs/fs.h"
#include "moduos/kernel/memory/memory.h"
#include <stddef.h>
#include <stdint.h>
#include "moduos/kernel/macros.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/drivers/graphics/VGA.h"
#include "moduos/drivers/input/input.h"
#include "moduos/drivers/Time/RTC.h"
#include "moduos/arch/AMD64/interrupts/irq.h"
#include "moduos/fs/fs.h"  // Unified filesystem interface
#include "moduos/kernel/shell/helpers.h"
#include "moduos/fs/path.h"
#include "moduos/fs/fd.h"
#include "moduos/kernel/shell/art.h"
#include "moduos/kernel/COM/com.h"
#include "moduos/kernel/events/events.h"
#include "moduos/kernel/debug.h"
#include "moduos/kernel/panic.h"
#include "moduos/kernel/applications/rl-clock.h"
#include "moduos/drivers/Drive/vDrive.h"  // Changed from ATA to vDrive
#include "moduos/kernel/process/process.h"
#include "moduos/kernel/exec.h"
#include "moduos/kernel/kernel.h"  // For boot_drive_index
#include "moduos/kernel/games/game_menu.h"
#include "moduos/drivers/power/ACPI.h"
#include "moduos/kernel/memory/memory.h"


/* Shell state */
shell_state_t shell_state = {
    .running = 1,
    .fat32_mounted = 0,
    .iso9660_mounted = 0,
    .last_command = "",
    .command_count = 0,
    .show_timestamps = 0,
    .path = "?/",
    .user = "mdman",
    .pcname = "dellpc",
    .history = {{0}},
    .history_count = 0,
    .history_index = 0,
    .browsing_history = 0,
    .current_slot = -1,
    .cwd = "/",
    .boot_slot = -1
};

/* Convert slot number (0-25) to letter (A-Z) */
static char slot_to_letter(int slot) {
    return 'A' + slot;
}

/* Convert letter (A-Z or a-z) to slot number (0-25), returns -1 if invalid */
static int letter_to_slot(char letter) {
    if (letter >= 'A' && letter <= 'Z') {
        return letter - 'A';
    } else if (letter >= 'a' && letter <= 'z') {
        return letter - 'a';
    }
    return -1;
}

/* Helper to parse integer arguments */
static int parse_int(const char* str, int* out) {
    if (!str || !out) return -1;
    
    int value = 0;
    int negative = 0;
    
    if (*str == '-') {
        negative = 1;
        str++;
    }
    
    if (*str == '\0') return -1;
    
    while (*str) {
        if (*str < '0' || *str > '9') return -1;
        value = value * 10 + (*str - '0');
        str++;
    }
    
    *out = negative ? -value : value;
    return 0;
}

/* Helper: Normalize path (remove trailing slashes). Supports both /... and $/... */
static int is_virtual_abs_path(const char *p) {
    return (p && p[0] == '$' && p[1] == '/');
}

void normalize_path(char* path) {
    if (!path) return;

    if (strlen(path) == 0) {
        strcpy(path, "/");
        return;
    }

    /* $/ paths are already absolute in the DEVVFS namespace */
    if (is_virtual_abs_path(path)) {
        size_t len = strlen(path);
        /* Remove trailing slash unless it's "$/" */
        if (len > 2 && path[len - 1] == '/') {
            path[len - 1] = '\0';
        }
        return;
    }

    /* Ensure normal paths start with / */
    if (path[0] != '/') {
        char temp[256];
        strcpy(temp, "/");
        strncat(temp, path, 254);
        strcpy(path, temp);
    }

    /* Remove trailing slash unless it's root */
    size_t len = strlen(path);
    if (len > 1 && path[len - 1] == '/') {
        path[len - 1] = '\0';
    }
}

/* Helper: Join two paths. Treats both /... and $/... as absolute. */
void join_path(const char* base, const char* component, char* result) {
    if (!result) return;
    if (!base) base = "/";
    if (!component) component = "";

    if (component[0] == '/' || is_virtual_abs_path(component)) {
        /* Absolute path */
        strncpy(result, component, 255);
        result[255] = 0;
    } else {
        /* Relative path */
        strncpy(result, base, 255);
        result[255] = 0;
        size_t len = strlen(result);
        if (len == 0 || result[len - 1] != '/') {
            strncat(result, "/", 255 - strlen(result));
        }
        strncat(result, component, 255 - strlen(result));
    }
    normalize_path(result);
}

/* Helper: Get parent directory */
static void get_parent_dir(const char* path, char* parent) {
    strcpy(parent, path);
    
    /* Find last slash */
    char* last_slash = NULL;
    for (char* p = parent; *p; p++) {
        if (*p == '/') last_slash = p;
    }
    
    if (last_slash == parent) {
        /* Parent is root */
        parent[1] = '\0';
    } else if (last_slash) {
        *last_slash = '\0';
    } else {
        strcpy(parent, "/");
    }
}

/* Helper: Update shell prompt based on current filesystem */
static void update_shell_path(void) {
    if (shell_state.current_slot < 0) {
        /* No mounted FS selected yet */
        shell_state.path[0] = '?';
        shell_state.path[1] = '/';
        shell_state.path[2] = '\0';
        return;
    }

    /*
     * ModuOS paths are POSIX-like. The old DOS-style "A:/" prompt is confusing
     * because the actual root is simply "/".
     */
    if (shell_state.cwd[0] == '\0') {
        strcpy(shell_state.path, "/");
    } else {
        strncpy(shell_state.path, shell_state.cwd, sizeof(shell_state.path) - 1);
        shell_state.path[sizeof(shell_state.path) - 1] = '\0';
    }
}


/* History management functions */
void add_to_history(const char* command) {
    if (!command || strlen(command) == 0) {
        return;
    }
    
    // Don't add duplicates of the last command
    if (shell_state.history_count > 0) {
        int last_idx = (shell_state.history_count - 1) % HISTORY_SIZE;
        if (strcmp(shell_state.history[last_idx], command) == 0) {
            return;
        }
    }
    
    // Add command to history
    int idx = shell_state.history_count % HISTORY_SIZE;
    strncpy(shell_state.history[idx], command, COMMAND_MAX_LEN - 1);
    shell_state.history[idx][COMMAND_MAX_LEN - 1] = '\0';
    shell_state.history_count++;
}

const char* get_history_prev(void) {
    if (shell_state.history_count == 0) {
        return NULL;
    }
    
    // First time browsing, start from the end
    if (!shell_state.browsing_history) {
        shell_state.browsing_history = 1;
        shell_state.history_index = shell_state.history_count - 1;
    } else if (shell_state.history_index > 0) {
        shell_state.history_index--;
    } else {
        // Already at oldest command
        return NULL;
    }
    
    int idx = shell_state.history_index % HISTORY_SIZE;
    return shell_state.history[idx];
}

const char* get_history_next(void) {
    if (!shell_state.browsing_history || shell_state.history_count == 0) {
        return NULL;
    }
    
    if (shell_state.history_index < shell_state.history_count - 1) {
        shell_state.history_index++;
        int idx = shell_state.history_index % HISTORY_SIZE;
        return shell_state.history[idx];
    } else {
        // Reached newest command - clear line
        shell_state.browsing_history = 0;
        return "";
    }
}

void reset_history_browsing(void) {
    shell_state.browsing_history = 0;
}

void up_arrow_pressed() {
    const char* cmd = get_history_prev();
    if (cmd) {
        replace_input_line(cmd);
    }
}

void down_arrow_pressed() {
    const char* cmd = get_history_next();
    if (cmd) {
        replace_input_line(cmd);
    }
}

void panicer_close_shell4(void) {
    shell_state.running = 0;
}

void parse_command2(char* input, char* command, char* args) {
    /* Skip leading whitespace */
    while (*input == ' ' || *input == '\t') input++;

    /* Extract command */
    while (*input && *input != ' ' && *input != '\t') {
        *command++ = *input++;
    }
    *command = '\0';

    /* Skip whitespace before args */
    while (*input == ' ' || *input == '\t') input++;
    
    /* Copy remaining args */
    while (*input) {
        *args++ = *input++;
    }
    *args = '\0';
}

static int auto_mount_boot_drive(void) {
    // Strategy: Just mount the first working FAT32 drive we can find
    // Don't require boot marker - let user work with any mounted filesystem
    
    VGA_Write("\\cySearching for bootable filesystem...\\rr\n");
    
    // Try vDrive 1 first (typically the hard disk)
    int slot = fs_mount_drive(1, 0, FS_TYPE_UNKNOWN);
    if (slot >= 0) {
        shell_state.current_slot = slot;
        shell_state.boot_slot = slot;
        strcpy(shell_state.cwd, "/");
        update_shell_path();
        
        fs_type_t type;
        fs_get_mount_info(slot, NULL, NULL, &type);
        const char* fs_name = fs_type_name(type);
        VGA_Writef("\\cgMounted filesystem in slot %d (%s)\\rr\n", slot, fs_name);
        
        // Check for boot marker (informational only)
        fs_mount_t* mount = fs_get_mount(slot);
        if (mount && fs_file_exists(mount, "/ModuOS/System64/mdsys.sqr")) {
            VGA_Write("\\cgBoot marker found: /ModuOS/System64/mdsys.sqr\\rr\n");
        } else {
            VGA_Write("\\cyNote: Boot marker not found (non-bootable disk)\\rr\n");
        }
        
        return 1;
    }
    
    // Try vDrive 0 (might be a bootable ISO)
    slot = fs_mount_drive(0, 0, FS_TYPE_UNKNOWN);
    if (slot >= 0) {
        shell_state.current_slot = slot;
        shell_state.boot_slot = slot;
        strcpy(shell_state.cwd, "/");
        update_shell_path();
        
        fs_type_t type;
        fs_get_mount_info(slot, NULL, NULL, &type);
        const char* fs_name = fs_type_name(type);
        VGA_Writef("\\cgMounted filesystem in slot %d (%s)\\rr\n", slot, fs_name);
        return 1;
    }
    
    // Try vDrive 2
    slot = fs_mount_drive(2, 0, FS_TYPE_UNKNOWN);
    if (slot >= 0) {
        shell_state.current_slot = slot;
        shell_state.boot_slot = slot;
        strcpy(shell_state.cwd, "/");
        update_shell_path();
        
        fs_type_t type;
        fs_get_mount_info(slot, NULL, NULL, &type);
        const char* fs_name = fs_type_name(type);
        VGA_Writef("\\cgMounted filesystem in slot %d (%s)\\rr\n", slot, fs_name);
        return 1;
    }
    
    return 0; // No filesystem found
}

#define BUFFER_SIZE 64

void read_pcname_file(void)
{
    static char pcname_buf[128];
    size_t bytes = 0;

    /* Default name fallback */
    strncpy(pcname_buf, "UNKNOWN-PC", sizeof(pcname_buf) - 1);
    pcname_buf[sizeof(pcname_buf) - 1] = '\0';

    /* No boot slot -> fallback */
    if (shell_state.boot_slot < 0) {
        goto set_name;
    }

    /* Get mount from kernel */
    fs_mount_t* mount = fs_get_mount(shell_state.boot_slot);
    if (!mount || !mount->valid) {
        goto set_name;
    }

    /* Check file exists */
    if (!fs_file_exists(mount, "/ModuOS/System64/pcname.txt")) {
        goto set_name;
    }

    /* Read file into buffer (leave room for NUL) */
    if (fs_read_file(mount, "/ModuOS/System64/pcname.txt",
                     pcname_buf, sizeof(pcname_buf) - 1, &bytes) != 0) {
        goto set_name;
    }

    /* Clamp bytes and ensure NUL termination */
    if (bytes >= sizeof(pcname_buf))
        bytes = sizeof(pcname_buf) - 1;
    pcname_buf[bytes] = '\0';

    /* Strip trailing CR/LF */
    for (size_t i = 0; i < bytes; i++) {
        if (pcname_buf[i] == '\r' || pcname_buf[i] == '\n') {
            pcname_buf[i] = '\0';
            break;
        }
    }

set_name:
    /* Copy into shell_state.pcname */
    strncpy(shell_state.pcname, pcname_buf, sizeof(shell_state.pcname) - 1);
    shell_state.pcname[sizeof(shell_state.pcname) - 1] = '\0';
}

void handle_ps_command(void) {
    VGA_Write("Process List:\n");
    VGA_Write("PID    NAME        STATE\n");
    VGA_Write("---    ----        -----\n");
    
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_t *proc = process_get_by_pid(i);
        if (proc) {
            char pid_str[12];
            itoa(proc->pid, pid_str, 10);
            VGA_Write(pid_str);
            VGA_Write("      ");
            VGA_Write(proc->name);
            VGA_Write("        ");
            
            switch (proc->state) {
                case PROCESS_STATE_READY:
                    VGA_Write("READY");
                    break;
                case PROCESS_STATE_RUNNING:
                    VGA_Write("RUNNING");
                    break;
                case PROCESS_STATE_BLOCKED:
                    VGA_Write("BLOCKED");
                    break;
                case PROCESS_STATE_SLEEPING:
                    VGA_Write("SLEEPING");
                    break;
                case PROCESS_STATE_ZOMBIE:
                    VGA_Write("ZOMBIE");
                    break;
                case PROCESS_STATE_TERMINATED:
                    VGA_Write("TERMINATED");
                    break;
            }
            VGA_Write("\n");
        }
    }
}

void handle_kill_command(const char *args) {
    int pid = atoi(args);
    process_kill(pid);
    VGA_Write("Killed process ");
    char pid_str[12];
    itoa(pid, pid_str, 10);
    VGA_Write(pid_str);
    VGA_Write("\n");
}

void zenith4_start() {
    // Get boot slot from kernel (already mounted!)
    int boot_slot = kernel_get_boot_slot();
    if (boot_slot < 0) {
        VGA_Write("\\crERROR: No boot drive mounted!\\rr\n");
        VGA_Write("\\cyThe kernel failed to mount a boot filesystem.\\rr\n");
        return;
    }

    // Store it in shell state
    shell_state.boot_slot = boot_slot;
    shell_state.current_slot = boot_slot;
    strcpy(shell_state.cwd, "/");
    update_shell_path();

    VGA_Clear();
    zsbanner();
    VGA_Write("\\cc(c) New Technologies Software 2025. All Rights Reserved\\rr\n");
    VGA_Write("\\cy type 'help' to see available commands\\rr\n");
    
    // Show which drive is mounted
    fs_type_t type;
    int vdrive_id;
    fs_get_mount_info(boot_slot, &vdrive_id, NULL, &type);
    const char* fs_name = fs_type_name(type);
    /* drive letters are deprecated */
    
    read_pcname_file();

    while (shell_state.running == 1)
    {
        if (shell_state.show_timestamps == 1) {
            rtc_datetime_t timeSTMP;
            rtc_get_datetime(&timeSTMP);

            if (strcmp(shell_state.user, "mdman") == 0) {
                VGA_Writef("\\cy%02u:%02u:%02u \\cr# \\cp%s\\rr@\\cc%s \\rr%s>", timeSTMP.hour, timeSTMP.minute, timeSTMP.second, shell_state.user, shell_state.pcname, shell_state.path);
            } else {
                VGA_Writef("\\cy%02u:%02u:%02u \\cg$ \\cp%s\\rr@\\cc%s \\rr%s>", timeSTMP.hour, timeSTMP.minute, timeSTMP.second, shell_state.user, shell_state.pcname, shell_state.path);
            }
        } else {
            if (strcmp(shell_state.user, "mdman") == 0) {
                VGA_Writef("\\cr# \\cp%s\\rr@\\cc%s \\rr%s>", shell_state.user, shell_state.pcname, shell_state.path);
            } else {
                VGA_Writef("\\cg$ \\cp%s\\rr@\\cc%s \\rr%s>", shell_state.user, shell_state.pcname, shell_state.path);
            }
        }

        // Reset history browsing when starting new input
        reset_history_browsing();

        // --- Get user input ---
        char* user_input = input();

        // --- Parse command and args ---
        char command[64] = {0};
        char args[192] = {0};
        parse_command2(user_input, command, args);

        // --- Build full command line for history ---
        char full_command[COMMAND_MAX_LEN] = {0};
        if (strlen(command) > 0) {
            strncpy(full_command, user_input, COMMAND_MAX_LEN - 1);
            full_command[COMMAND_MAX_LEN - 1] = '\0';
            
            // Add to history
            add_to_history(full_command);
            
            // Update last_command
            shell_state.command_count++;
            strncpy(shell_state.last_command, command, sizeof(shell_state.last_command) - 1);
            shell_state.last_command[sizeof(shell_state.last_command) - 1] = '\0';
        }

        // --- Command dispatch ---
        if (strcmp(command, "help") == 0) {
            VGA_Write("\\cydebug              Kernel debug level (debug off/med/on/status)\\rr\\n");

            VGA_Write("\\cyhelp                shows this message\\rr\n");
            VGA_Write("\\cyclear - cls         clears the screen\\rr\n");
            VGA_Write("\\cypoweroff - shutdown Shuts down the computer\\rr\n");
            VGA_Write("\\cyreboot - restart    Restart the computer\\rr\n");
            VGA_Write("\\cyzsfetch             Show System Stats\\rr\n");
            VGA_Write("\\cyhistory             Show command history\\rr\n");
            VGA_Write("\\cylsblk - dev         Shows all storage devices (ATA + SATA)\\rr\n");
            VGA_Write("\\cymount               Mount filesystem (mount <drive> [lba] [type])\\rr\n");
            VGA_Write("\\cyunmount             Unmount filesystem (unmount <slot>)\\rr\n");
            VGA_Write("\\cymounts              List all mounted filesystems\\rr\n");
            VGA_Write("\\cybootinfo            Show boot drive information\\rr\n");
            VGA_Write("\\cyls [path]           List directory (current fs or path)\\rr\n");
            VGA_Write("\\cycd <path>           Change directory\\rr\n");
            VGA_Write("\\cypwd                 Print current directory\\rr\n");
            VGA_Write("\\cycat <path>          Display file contents\\rr\n");
            VGA_Write("\\cygames               Shows Game Menu!\\rr\n");
            VGA_Write("\\cydate                Show current date and time\\rr\n");
            VGA_Write("\\cybanner              Show system banners\\rr\n");
            VGA_Write("\\cytimestamps          Toggle timestamps in the prompt\\rr\n");
            VGA_Write("\\cyclock               Launch the clock application\\rr\n");
            VGA_Write("\\cyexec <file_path>    Launch UserLand Application\\rr\n");
            VGA_Write("\\cyprocess_list        Lists all Running OR Ready processes\\rr\n");
            VGA_Write("\\cygfxinfo             Show current framebuffer + GRUB gfxmode setting\\rr\n");
            VGA_Write("\\cysetres WxHxBPP       Edit /boot/grub/grub.cfg gfxmode then reboot\\rr\n");
        } else if (strcmp(command, "clear") == 0 || strcmp(command, "cls") == 0) {
            VGA_Clear();
        } else if (strcmp(command, "poweroff") == 0 || strcmp(command, "shutdown") == 0) {
            poweroff2();
        } else if (strcmp(command, "reboot") == 0 || strcmp(command, "restart") == 0) {
            reboot2();
        } else if (strcmp(command, "mount") == 0) {
            /* Syntax: mount <drive_index> [partition_lba] [type] */
            char drive_str[16] = {0};
            char lba_str[32] = {0};
            char type_str[32] = {0};
            const char* ptr = args;
            int token = 0;
            char* dest = drive_str;
            int dest_idx = 0;
        
            while (*ptr) {
                if (*ptr == ' ' || *ptr == '\t') {
                    if (dest_idx > 0) {
                        *dest = '\0';
                        token++;
                        dest_idx = 0;
                        if (token == 1) dest = lba_str;
                        else if (token == 2) dest = type_str;
                        else break;
                    }
                    ptr++;
                } else {
                    if (dest_idx < 15) {
                        *dest++ = *ptr;
                        dest_idx++;
                    }
                    ptr++;
                }
            }
            *dest = '\0';
        
            if (strlen(drive_str) == 0) {
                VGA_Write("\\crUsage: mount <vdrive_id> [partition_lba] [type]\\rr\n");
                VGA_Write("\\cyExample: mount 0\\rr\n");
                VGA_Write("\\cyExample: mount 0 2048 fat32\\rr\n");
                VGA_Write("\\cyUse 'lsblk' to see available drives\\rr\n");
                goto mount_done;
            }
        
            int drive_index = -1;
            uint32_t partition_lba = 0;
            if (parse_int(drive_str, &drive_index) != 0) {
                VGA_Write("\\crError: Invalid vdrive index\\rr\n");
                goto mount_done;
            }
        
            if (strlen(lba_str) > 0) {
                int lba_int = 0;
                if (parse_int(lba_str, &lba_int) != 0 || lba_int < 0) {
                    VGA_Write("\\crError: Invalid LBA value\\rr\n");
                    goto mount_done;
                }
                partition_lba = (uint32_t)lba_int;
            }
            
            fs_type_t fs_type = FS_TYPE_UNKNOWN;
            if (strlen(type_str) > 0) {
                if (strcmp(type_str, "fat32") == 0 || strcmp(type_str, "FAT32") == 0) {
                    fs_type = FS_TYPE_FAT32;
                } else if (strcmp(type_str, "iso9660") == 0 || strcmp(type_str, "ISO9660") == 0) {
                    fs_type = FS_TYPE_ISO9660;
                } else {
                    VGA_Write("\\crError: Unknown filesystem type\\rr\n");
                    goto mount_done;
                }
            }
        
            /* Use kernel mount function */
            int slot = fs_mount_drive(drive_index, partition_lba, fs_type);
            
            if (slot >= 0) {
                fs_type_t type;
                fs_get_mount_info(slot, NULL, NULL, &type);
                const char* fs_name = fs_type_name(type);
                char label[32];
                if (fs_get_mount_label(slot, label, sizeof(label)) == 0) {
                    VGA_Writef("\\cgMounted %s in slot %d (%s) (vdrive=%d, lba=%u)\\rr\n",
                               fs_name, slot, label, drive_index, partition_lba);
                } else {
                    VGA_Writef("\\cgMounted %s in slot %d (vdrive=%d, lba=%u)\\rr\n",
                               fs_name, slot, drive_index, partition_lba);
                }
            } else if (slot == -2) {
                VGA_Write("\\crError: Drive already mounted\\rr\n");
            } else if (slot == -3) {
                VGA_Write("\\crError: Mount table full\\rr\n");
            } else {
                VGA_Write("\\crError: Failed to mount filesystem\\rr\n");
            }
        
        mount_done:;
        } else if (strcmp(command, "unmount") == 0 || strcmp(command, "umount") == 0) {
            if (strlen(args) == 0) {
                VGA_Write("\\crUsage: unmount <slot>\\rr\n");
                VGA_Write("\\cyExample: unmount 0\\rr\n");
                VGA_Write("\\cyNote: drive letters are deprecated\\rr\n");
            } else {
                int slot = -1;

                /* Prefer numeric slot (new interface) */
                if (args[0] >= '0' && args[0] <= '9') {
                    int tmp = 0;
                    if (parse_int(args, &tmp) != 0 || tmp < 0) {
                        VGA_Write("\\crError: Invalid slot number\\rr\n");
                        goto unmount_done;
                    }
                    slot = tmp;
                } else {
                    /* Legacy: drive letter */
                    VGA_Write("\\cy[DEPRECATED] Drive letters are deprecated. Use 'unmount <slot>'.\\rr\n");
                    slot = letter_to_slot(args[0]);
                    if (slot < 0) {
                        VGA_Write("\\crError: Invalid drive letter\\rr\n");
                        goto unmount_done;
                    }
                }

                fs_type_t type;
                if (fs_get_mount_info(slot, NULL, NULL, &type) != 0) {
                    VGA_Writef("\\crError: Slot %d is not mounted\\rr\n", slot);
                } else {
                    const char* fs_name = fs_type_name(type);

                    if (slot == shell_state.current_slot) {
                        shell_state.current_slot = -1;
                        strcpy(shell_state.cwd, "/");
                        update_shell_path();
                    }

                    fs_unmount_slot(slot);
                    VGA_Writef("\\cgUnmounted %s from slot %d\\rr\n", fs_name, slot);
                }

            unmount_done:;
            }
        } else if (strcmp(command, "panic") == 0) {
            /* Hidden test command: triggers a kernel panic to test the panic UI */
            trigger_panic_unknown();
        } else if (strcmp(command, "debug") == 0) {
            if (strlen(args) == 0 || strcmp(args, "status") == 0) {
                kernel_debug_level_t lvl = kernel_debug_get_level();
                const char *name = (lvl == KDBG_OFF) ? "OFF" : (lvl == KDBG_MED) ? "MED" : "ON";
                VGA_Writef("\\cyKernel debug logs: %s\\rr\n", name);
            } else if (strcmp(args, "on") == 0) {
                kernel_debug_set_level(KDBG_ON);
                VGA_Write("\\cgKernel debug logs set to ON (verbose)\\rr\n");
            } else if (strcmp(args, "med") == 0) {
                kernel_debug_set_level(KDBG_MED);
                VGA_Write("\\cgKernel debug logs set to MED (minimal)\\rr\n");
            } else if (strcmp(args, "off") == 0) {
                kernel_debug_set_level(KDBG_OFF);
                VGA_Write("\\cgKernel debug logs set to OFF\\rr\n");
            } else {
                VGA_Write("\\crUsage: debug off|med|on|status\\rr\n");
            }
        } else if (strcmp(command, "mounts") == 0) {
            fs_list_mounts();  // Kernel function
        } else if (strcmp(command, "use") == 0) {
            /*
             * Deprecated: drive-letter / slot based navigation (A:, B:, ...) is no longer part
             * of the user-facing interface. Keep the command as a stub for old users.
             */
            VGA_Write("\\cy[DEPRECATED] 'use' is deprecated. Drive letters (A:, B:, C:) are no longer supported.\\rr\\n");
            VGA_Write("\\cyUse POSIX paths like '/' and the $/mnt namespace (e.g. 'ls $/mnt', 'ls $/mnt/vDrive0').\\rr\\n");
            if (strlen(args) > 0) {
                VGA_Write("\\cyNote: ignoring argument; filesystem selection is automatic.\\rr\\n");
            }

            /* Do nothing else. */
        } else if (strcmp(command, "ls") == 0) {
            char target_path[256];
            if (strlen(args) == 0) {
                strcpy(target_path, shell_state.cwd);
            } else {
                join_path(shell_state.cwd, args, target_path);
            }

            /* Support ModuOS virtual-root paths like $/dev and $/mnt */
            if (target_path[0] == '$' && target_path[1] == '/') {
                process_t *proc = process_get_current();
                if (!proc) {
                    VGA_Write("\\crError: No process context\\rr\n");
                } else {
                    fs_path_resolved_t r;
                    if (fs_resolve_path(proc, target_path, &r) != 0) {
                        VGA_Writef("\\crError: Invalid path '%s'\\rr\n", target_path);
                    } else if (r.route == FS_ROUTE_DEVVFS) {
                        int dfd = fd_devvfs_opendir(r.devvfs_kind);
                        if (dfd < 0) {
                            VGA_Write("\\crError: Cannot open DEVVFS directory\\rr\n");
                        } else {
                            char name[128];
                            int is_dir = 0;
                            uint32_t sz = 0;
                            while (1) {
                                int rc = fd_readdir(dfd, name, sizeof(name), &is_dir, &sz);
                                if (rc <= 0) break;
                                VGA_Write(name);
                                if (is_dir) VGA_Write("/");
                                VGA_Write("  ");
                            }
                            VGA_Write("\n");
                            fd_closedir(dfd);
                        }
                    } else if (r.route == FS_ROUTE_MOUNT) {
                        fs_mount_t *mnt = fs_get_mount(r.mount_slot);
                        if (!mnt || !mnt->valid) {
                            VGA_Write("\\crError: Invalid mount\\rr\n");
                        } else {
                            fs_list_directory(mnt, r.rel_path);
                        }
                    } else {
                        VGA_Write("\\crError: Unsupported $/ path\\rr\n");
                    }
                }
            } else {
                if (shell_state.current_slot < 0) {
                    VGA_Write("\\crError: No filesystem selected\\rr\n");
                } else {
                    fs_mount_t* mount = fs_get_mount(shell_state.current_slot);
                    if (mount) {
                        fs_list_directory(mount, target_path);
                    } else {
                        VGA_Write("\\crError: Invalid mount\\rr\n");
                    }
                }
            }
        } else if (strcmp(command, "cd") == 0) {
            if (strlen(args) == 0) {
                /* cd without args goes to root */
                strcpy(shell_state.cwd, "/");
                update_shell_path();
            } else {
                char new_path[256];

                /* Handle special cases */
                if (strcmp(args, "..") == 0) {
                    get_parent_dir(shell_state.cwd, new_path);
                } else if (strcmp(args, ".") == 0) {
                    strcpy(new_path, shell_state.cwd);
                } else {
                    join_path(shell_state.cwd, args, new_path);
                }

                /* Allow switching into the virtual-root namespace ($/...) */
                if (new_path[0] == '$' && new_path[1] == '/') {
                    strcpy(shell_state.cwd, new_path);
                    update_shell_path();
                } else {
                    if (shell_state.current_slot < 0) {
                        VGA_Write("\\crError: No filesystem selected\\rr\n");
                    } else {
                        /* Validate the directory exists on the current mount */
                        fs_mount_t* mount = fs_get_mount(shell_state.current_slot);
                        if (mount) {
                            int exists = fs_directory_exists(mount, new_path);

                            if (exists) {
                                strcpy(shell_state.cwd, new_path);
                                update_shell_path();
                            } else {
                                VGA_Writef("\\crError: Directory '%s' does not exist\\rr\n", args);
                            }
                        } else {
                            VGA_Write("\\crError: Invalid mount\\rr\n");
                        }
                    }
                }
            }
        } else if (strcmp(command, "pwd") == 0) {
            if (shell_state.current_slot < 0) {
                VGA_Write("\\cr(no filesystem mounted)\\rr\n");
            } else {
                VGA_Writef("%s\n", shell_state.cwd);
            }
        }  else if (strcmp(command, "history") == 0) {
            if (shell_state.history_count == 0) {
                VGA_Write("\\cyNo commands in history\\rr\n");
            } else {
                VGA_Write("\\cyCommand History:\\rr\n");
                int start = (shell_state.history_count > HISTORY_SIZE) ? 
                            (shell_state.history_count - HISTORY_SIZE) : 0;
                for (int i = start; i < shell_state.history_count; i++) {
                    int idx = i % HISTORY_SIZE;
                    VGA_Writef("  %d: %s\n", i + 1, shell_state.history[idx]);
                }
            }
        } else if (strcmp(command, "dev") == 0 || strcmp(command, "lsblk") == 0) {
            // Use vDrive to list all storage devices (ATA + SATA)
            vdrive_print_table();
        } else if (strcmp(command, "gfxinfo") == 0) {
            if (VGA_GetFrameBufferMode() == FB_MODE_GRAPHICS) {
                VGA_Write("Framebuffer mode: GRAPHICS (multiboot framebuffer)\n");
            } else {
                VGA_Write("Framebuffer mode: TEXT\n");
            }

            const char *cfg_path = "/boot/grub/grub.cfg";
            fs_mount_t *mnt = fs_get_mount(shell_state.boot_slot);
            if (!mnt || !mnt->valid) {
                VGA_Write("(boot filesystem not mounted; cannot read grub.cfg)\n");
            } else {
                fs_file_info_t info;
                if (fs_stat(mnt, cfg_path, &info) != 0) {
                    VGA_Write("(cannot stat /boot/grub/grub.cfg)\n");
                } else if (info.size == 0 || info.size > 65536) {
                    VGA_Write("(grub.cfg size unsupported)\n");
                } else {
                    char *buf = (char*)kmalloc(info.size + 1);
                    if (!buf) {
                        VGA_Write("(out of memory reading grub.cfg)\n");
                    } else {
                        size_t rd = 0;
                        if (fs_read_file(mnt, cfg_path, buf, info.size, &rd) == 0) {
                            buf[rd] = 0;
                            char *p = strstr(buf, "set gfxmode=");
                            if (p) {
                                char line[128];
                                size_t i = 0;
                                while (p[i] && p[i] != '\n' && i + 1 < sizeof(line)) { line[i] = p[i]; i++; }
                                line[i] = 0;
                                VGA_Write("GRUB: ");
                                VGA_Write(line);
                                VGA_Write("\n");
                            } else {
                                VGA_Write("GRUB: (no 'set gfxmode=' line found)\n");
                            }
                        } else {
                            VGA_Write("(failed to read grub.cfg)\n");
                        }
                        kfree(buf);
                    }
                }
            }
        } else if (strcmp(command, "setres") == 0) {
            const char *p = args;
            while (p && (*p == ' ' || *p == '\t')) p++;
            if (!p || *p == 0) {
                VGA_Write("Usage: setres WxHxBPP (example: setres 1024x768x32)\n");
            } else {
                int w = 0, h = 0, bpp = 0;
                const char *q = p;
                while (*q >= '0' && *q <= '9') { w = w * 10 + (*q - '0'); q++; }
                if (*q != 'x') { VGA_Write("Invalid format. Expected WxHxBPP\n"); goto setres_done; }
                q++;
                while (*q >= '0' && *q <= '9') { h = h * 10 + (*q - '0'); q++; }
                if (*q != 'x') { VGA_Write("Invalid format. Expected WxHxBPP\n"); goto setres_done; }
                q++;
                while (*q >= '0' && *q <= '9') { bpp = bpp * 10 + (*q - '0'); q++; }

                if (w <= 0 || h <= 0 || (bpp != 16 && bpp != 24 && bpp != 32)) {
                    VGA_Write("Invalid values (bpp must be 16/24/32)\n");
                    goto setres_done;
                }

                const char *cfg_path = "/boot/grub/grub.cfg";
                fs_mount_t *mnt = fs_get_mount(shell_state.boot_slot);
                if (!mnt || !mnt->valid) {
                    VGA_Write("Error: boot filesystem not mounted; cannot edit grub.cfg\n");
                    goto setres_done;
                }

                fs_file_info_t info;
                if (fs_stat(mnt, cfg_path, &info) != 0 || info.size == 0 || info.size > 65536) {
                    VGA_Write("Error: cannot read grub.cfg\n");
                    goto setres_done;
                }

                char *buf = (char*)kmalloc(info.size + 1);
                if (!buf) { VGA_Write("Error: out of memory\n"); goto setres_done; }

                size_t rd = 0;
                if (fs_read_file(mnt, cfg_path, buf, info.size, &rd) != 0) {
                    VGA_Write("Error: failed to read grub.cfg\n");
                    kfree(buf);
                    goto setres_done;
                }
                buf[rd] = 0;

                char new_line[64];
                snprintf(new_line, sizeof(new_line), "set gfxmode=%dx%dx%d", w, h, bpp);

                char *pos = strstr(buf, "set gfxmode=");
                if (!pos) {
                    VGA_Write("Error: grub.cfg has no 'set gfxmode=' line\n");
                    kfree(buf);
                    goto setres_done;
                }

                char *eol = pos;
                while (*eol && *eol != '\n') eol++;

                size_t prefix_len = (size_t)(pos - buf);
                size_t suffix_len = strlen(eol);
                size_t out_sz = prefix_len + strlen(new_line) + suffix_len;

                char *out = (char*)kmalloc(out_sz + 1);
                if (!out) {
                    VGA_Write("Error: out of memory\n");
                    kfree(buf);
                    goto setres_done;
                }

                memcpy(out, buf, prefix_len);
                memcpy(out + prefix_len, new_line, strlen(new_line));
                memcpy(out + prefix_len + strlen(new_line), eol, suffix_len);
                out[out_sz] = 0;

                int wr = fs_write_file(mnt, cfg_path, out, out_sz);
                if (wr == 0) {
                    VGA_Write("Updated GRUB gfxmode successfully.\n");
                    VGA_Write("Reboot required for resolution change to take effect.\n");
                } else {
                    VGA_Writef("Failed to write grub.cfg (err=%d).\n", wr);
                }

                kfree(out);
                kfree(buf);
            }
        setres_done:;
        } else if (strcmp(command, "date") == 0) {
            rtc_datetime_t clock;
            rtc_get_datetime(&clock);
            VGA_Writef("\\cyDate: %02u/%02u/%02u || Time: %02u:%02u:%02u \\rr\n", clock.day, clock.month, clock.year, clock.hour, clock.minute, clock.second);
        } else if (strcmp(command, "banner") == 0) {
            zsbanner();
            mdbanner();
        } else if (strcmp(command, "games") == 0) {
            shell_state.running = 0;
            VGA_Clear();
            event_clear(); 
            Menu();
            shell_state.running = 1;
        } else if (strcmp(command, "timestamps") == 0) {
            if (shell_state.show_timestamps == 0) {
                shell_state.show_timestamps = 1;
                VGA_Write("\\cgTimestamps enabled\\rr\n");
            } else if (shell_state.show_timestamps == 1) {
                shell_state.show_timestamps = 0;
                VGA_Write("\\cgTimestamps disabled\\rr\n");
            }
        } else if (strcmp(command, "clock") == 0) {
            Clock();
        } else if (strcmp(command, "dos") == 0) {
            shell_state.running = 0;
            msdos_start();
            shell_state.running = 1;
        } else if (strcmp(command, "bootinfo") == 0) {
            if (shell_state.boot_slot < 0) {
                VGA_Write("\\cyNo boot drive detected\\rr\n");
                VGA_Write("\\cyThe system looked for: /ModuOS/System64/mdsys.sqr\\rr\n");
            } else {
                int drive_idx;
                uint32_t lba;
                fs_type_t type;
                fs_get_mount_info(shell_state.boot_slot, &drive_idx, &lba, &type);
                const char* fs_name = fs_type_name(type);
                VGA_Writef("\\cgBoot Drive: slot %d (%s)\\rr\n", shell_state.boot_slot, fs_name);
                VGA_Writef("\\cyVirtual Drive ID: %d\\rr\n", drive_idx);
                VGA_Writef("\\cyPartition LBA: %u\\rr\n", lba);
                
                // Get vDrive info for the boot drive
                vdrive_t* vd = vdrive_get(drive_idx);
                if (vd && vd->present) {
                    VGA_Writef("\\cyDrive Type: %s\\rr\n", vdrive_get_type_string(vd->type));
                    VGA_Writef("\\cyBackend: %s\\rr\n", vdrive_get_backend_string(vd->backend));
                    VGA_Writef("\\cyModel: %s\\rr\n", vd->model);
                    if (vd->backend == VDRIVE_BACKEND_SATA && strlen(vd->serial) > 0) {
                        VGA_Writef("\\cySerial: %s\\rr\n", vd->serial);
                    }
                }
                
                VGA_Write("\\cyBoot Marker: /ModuOS/System64/mdsys.sqr\\rr\n");
                
                if (shell_state.current_slot == shell_state.boot_slot) {
                    VGA_Write("\\cg(Currently active)\\rr\n");
                }
            }
        } else if (strcmp(command, "process_list") == 0) {
            handle_ps_command();
        } else if (strcmp(command, "exec") == 0) {
            exec(args);
        } else if (strcmp(command, "acpi") == 0) {
            acpi_print_info();
        } else if (strcmp(command, "exit") == 0) {
            VGA_Write("exiting shell");
            DEBUG_PAUSE(2);
            shell_state.running = 0;
        } else if (strlen(command) == 0) {
            // Do nothing on empty command
        } else {
            // Unknown command - try to find it in /Apps/<command>.sqr
            if (shell_state.current_slot < 0) {
                VGA_Writef("\\crZenith: error: Command Not Found '%s'\\rr\n", command);
                VGA_Write("\\cy(No filesystem mounted)\\rr\n");
            } else {
                fs_mount_t* mnt = fs_get_mount(shell_state.current_slot);
                
                if (!mnt) {
                    VGA_Writef("\\crZenith: error: Command Not Found '%s'\\rr\n", command);
                    VGA_Write("\\cy(Invalid mount)\\rr\n");
                } else {
                    // Build path: /Apps/<command>.sqr
                    char app_path[256];
                    strcpy(app_path, "/Apps/");
                    strcat(app_path, command);
                    strcat(app_path, ".sqr");
                    
                    if (fs_file_exists(mnt, app_path)) {
                        // Build full command line with app path + args
                        char exec_cmdline[COMMAND_MAX_LEN];
                        strcpy(exec_cmdline, app_path);
                        if (strlen(args) > 0) {
                            strcat(exec_cmdline, " ");
                            strcat(exec_cmdline, args);
                        }
                        // Execute with full command line
                        exec(exec_cmdline);
                    } else {
                        // Not found
                        VGA_Writef(
                            "\\cr%s : The term '%s' is not recognized as the name of a klet, or operable program.\n"
                            "Check the spelling of the name and try again.\n"
                            "+ %s %s \n \\rr\\rr",
                            command, command, command, args
                        );
                    }
                }
            }
        }
    }
}