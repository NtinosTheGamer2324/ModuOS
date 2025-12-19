//shell.c
#include "moduos/kernel/shell/zenith4.h"
#include "moduos/kernel/shell/legacy/MSDOS.h"
#include <stddef.h>
#include <stdint.h>
#include "moduos/kernel/macros.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/drivers/graphics/VGA.h"
#include "moduos/drivers/input/input.h"
#include "moduos/drivers/input/ps2/ps2.h"
#include "moduos/drivers/Time/RTC.h"
#include "moduos/arch/AMD64/interrupts/irq.h"
#include "moduos/fs/fs.h"  // Unified filesystem interface
#include "moduos/kernel/shell/helpers.h"
#include "moduos/kernel/shell/art.h"
#include "moduos/kernel/COM/com.h"
#include "moduos/kernel/events/events.h"
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

/* Helper: Normalize path (remove trailing slashes, handle . and ..) */
void normalize_path(char* path) {
    if (!path || strlen(path) == 0) {
        strcpy(path, "/");
        return;
    }
    
    /* Ensure path starts with / */
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

/* Helper: Join two paths */
void join_path(const char* base, const char* component, char* result) {
    if (component[0] == '/') {
        /* Absolute path */
        strcpy(result, component);
    } else {
        /* Relative path */
        strcpy(result, base);
        if (strcmp(base, "/") != 0) {
            strcat(result, "/");
        }
        strcat(result, component);
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
        shell_state.path[0] = '?';
        shell_state.path[1] = '/';
        shell_state.path[2] = '\0';
    } else {
        char drive_letter = slot_to_letter(shell_state.current_slot);

        if (strcmp(shell_state.cwd, "/") == 0) {
            /* Just "X:/" */
            shell_state.path[0] = drive_letter;
            shell_state.path[1] = ':';
            shell_state.path[2] = '/';
            shell_state.path[3] = '\0';
        } else {
            /* "X:/path" */
            shell_state.path[0] = drive_letter;
            shell_state.path[1] = ':';
            strcpy(shell_state.path + 2, shell_state.cwd);
        }
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
        char drive_letter = slot_to_letter(slot);
        VGA_Writef("\\cgMounted filesystem as %c: (%s)\\rr\n", drive_letter, fs_name);
        
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
        char drive_letter = slot_to_letter(slot);
        VGA_Writef("\\cgMounted filesystem as %c: (%s)\\rr\n", drive_letter, fs_name);
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
        char drive_letter = slot_to_letter(slot);
        VGA_Writef("\\cgMounted filesystem as %c: (%s)\\rr\n", drive_letter, fs_name);
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
    char drive_letter = 'A' + boot_slot;
    
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
            VGA_Write("\\cyAvailable Commands: \\rr\n");
            VGA_Write("\\cyhelp                shows this message\\rr\n");
            VGA_Write("\\cyclear - cls         clears the screen\\rr\n");
            VGA_Write("\\cypoweroff - shutdown Shuts down the computer\\rr\n");
            VGA_Write("\\cyreboot - restart    Restart the computer\\rr\n");
            VGA_Write("\\cyzsfetch             Show System Stats\\rr\n");
            VGA_Write("\\cyhistory             Show command history\\rr\n");
            VGA_Write("\\cylsblk - dev         Shows all storage devices (ATA + SATA)\\rr\n");
            VGA_Write("\\cymount               Mount filesystem (mount <drive> [lba] [type])\\rr\n");
            VGA_Write("\\cyunmount             Unmount filesystem (unmount <letter>)\\rr\n");
            VGA_Write("\\cymounts              List all mounted filesystems\\rr\n");
            VGA_Write("\\cyuse                 Switch to filesystem (use <letter>)\\rr\n");
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
                char drive_letter = slot_to_letter(slot);
                VGA_Writef("\\cgMounted %s on %c: (vdrive=%d, lba=%u)\\rr\n",
                           fs_name, drive_letter, drive_index, partition_lba);
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
                VGA_Write("\\crUsage: unmount <letter>\\rr\n");
                VGA_Write("\\cyExample: unmount C\\rr\n");
            } else {
                int slot = letter_to_slot(args[0]);
                if (slot < 0) {
                    VGA_Write("\\crError: Invalid drive letter\\rr\n");
                } else {
                    fs_type_t type;
                    if (fs_get_mount_info(slot, NULL, NULL, &type) != 0) {
                        VGA_Writef("\\crError: Drive %c: is not mounted\\rr\n", slot_to_letter(slot));
                    } else {
                        const char* fs_name = fs_type_name(type);
                        
                        if (slot == shell_state.current_slot) {
                            shell_state.current_slot = -1;
                            strcpy(shell_state.cwd, "/");
                            update_shell_path();
                        }
                        
                        fs_unmount_slot(slot);
                        VGA_Writef("\\cgUnmounted %s from %c:\\rr\n", fs_name, slot_to_letter(slot));
                    }
                }
            }
        } else if (strcmp(command, "mounts") == 0) {
            fs_list_mounts();  // Kernel function
        } else if (strcmp(command, "use") == 0) {
            if (strlen(args) == 0) {
                VGA_Write("\\crUsage: use <letter>\\rr\n");
                if (shell_state.current_slot >= 0) {
                    VGA_Writef("\\cyCurrent drive: %c:\\rr\n", slot_to_letter(shell_state.current_slot));
                } else {
                    VGA_Write("\\cyNo filesystem selected\\rr\n");
                }
            } else {
                int slot = letter_to_slot(args[0]);
                if (slot < 0) {
                    VGA_Write("\\crError: Invalid drive letter\\rr\n");
                } else {
                    fs_type_t type;
                    if (fs_get_mount_info(slot, NULL, NULL, &type) != 0) {
                        VGA_Writef("\\crError: Drive %c: is not mounted\\rr\n", slot_to_letter(slot));
                    } else {
                        shell_state.current_slot = slot;
                        strcpy(shell_state.cwd, "/");
                        update_shell_path();
                        const char* fs_name = fs_type_name(type);
                        VGA_Writef("\\cgSwitched to %c: (%s)\\rr\n", slot_to_letter(slot), fs_name);
                    }
                }
            }
        } else if (strcmp(command, "ls") == 0) {
            if (shell_state.current_slot < 0) {
                VGA_Write("\\crError: No filesystem selected\\rr\n");
            } else {
                char target_path[256];
                if (strlen(args) == 0) {
                    strcpy(target_path, shell_state.cwd);
                } else {
                    join_path(shell_state.cwd, args, target_path);
                }
                
                fs_mount_t* mount = fs_get_mount(shell_state.current_slot);
                if (mount) {
                    fs_list_directory(mount, target_path);
                } else {
                    VGA_Write("\\crError: Invalid mount\\rr\n");
                }
            }
        } else if (strcmp(command, "cd") == 0) {
            if (shell_state.current_slot < 0) {
                VGA_Write("\\crError: No filesystem selected\\rr\n");
            } else if (strlen(args) == 0) {
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
                
                /* Validate the directory exists */
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
                char drive_letter = slot_to_letter(shell_state.boot_slot);
                int drive_idx;
                uint32_t lba;
                fs_type_t type;
                fs_get_mount_info(shell_state.boot_slot, &drive_idx, &lba, &type);
                const char* fs_name = fs_type_name(type);
                VGA_Writef("\\cgBoot Drive: %c: (%s)\\rr\n", drive_letter, fs_name);
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