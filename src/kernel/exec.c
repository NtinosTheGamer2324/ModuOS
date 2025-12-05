//exec.c
#include "moduos/kernel/exec.h"
#include "moduos/kernel/shell/zenith4.h"
#include "moduos/fs/fs.h"
#include "moduos/kernel/COM/com.h"
#include "moduos/kernel/memory/memory.h"
#include "moduos/kernel/loader/elf.h"
#include "moduos/kernel/process/process.h"
#include "moduos/drivers/graphics/VGA.h"
#include "moduos/kernel/memory/string.h"

extern shell_state_t shell_state;

/* Path manipulation helpers - these need to stay in shell or be made global */
extern void normalize_path(char* path);
extern void join_path(const char* base, const char* component, char* result);

#define MAX_ARGS 32

/* Parse command line into argv array (Windows-style with spaces) */
static int parse_args(const char *cmdline, char **argv, int max_args) {
    int argc = 0;
    const char *p = cmdline;
    static char arg_buf[1024];  // Static buffer for argument strings
    char *buf_ptr = arg_buf;
    
    while (*p && argc < max_args - 1) {
        // Skip leading spaces
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        
        // Mark start of this argument
        argv[argc++] = buf_ptr;
        
        // Handle quoted strings
        if (*p == '"') {
            p++; // Skip opening quote
            while (*p && *p != '"') {
                *buf_ptr++ = *p++;
            }
            if (*p == '"') p++; // Skip closing quote
        } else {
            // Copy until space or end
            while (*p && *p != ' ' && *p != '\t') {
                *buf_ptr++ = *p++;
            }
        }
        
        *buf_ptr++ = '\0'; // Null-terminate this argument
    }
    
    argv[argc] = NULL; // Null-terminate argv array
    return argc;
}

void exec(const char *args) {
    com_write_string(COM1_PORT, "\n[EXEC] ===== START EXEC COMMAND =====\n");
    
    // Skip leading spaces
    while (*args == ' ') args++;

    if (*args == '\0') {
        VGA_Write("Usage: exec <path> [args...]\n");
        VGA_Write("Example: exec /Apps/cat.sqr /file.txt\n");
        return;
    }

    if (shell_state.current_slot < 0) {
        VGA_Write("\\crError: No filesystem selected\\rr\n");
        return;
    }

    // Get mount from kernel mount table
    fs_mount_t* mount = fs_get_mount(shell_state.current_slot);
    if (!mount || !mount->valid) {
        VGA_Write("\\crError: Invalid filesystem mount\\rr\n");
        return;
    }

    // Parse arguments
    char *argv[MAX_ARGS];
    int argc = parse_args(args, argv, MAX_ARGS);
    
    if (argc == 0) {
        VGA_Write("\\crError: No program specified\\rr\n");
        return;
    }
    
    com_write_string(COM1_PORT, "[EXEC] Parsed ");
    char num_buf[12];
    itoa(argc, num_buf, 10);
    com_write_string(COM1_PORT, num_buf);
    com_write_string(COM1_PORT, " arguments:\n");
    for (int i = 0; i < argc; i++) {
        com_write_string(COM1_PORT, "[EXEC]   argv[");
        itoa(i, num_buf, 10);
        com_write_string(COM1_PORT, num_buf);
        com_write_string(COM1_PORT, "] = \"");
        com_write_string(COM1_PORT, argv[i]);
        com_write_string(COM1_PORT, "\"\n");
    }

    // Build normalized absolute path for the executable
    char exec_path[256];
    join_path(shell_state.cwd, argv[0], exec_path);

    com_write_string(COM1_PORT, "[EXEC] Path: ");
    com_write_string(COM1_PORT, exec_path);
    com_write_string(COM1_PORT, "\n");

    // Check existence
    com_write_string(COM1_PORT, "[EXEC] Checking if file exists...\n");
    if (!fs_file_exists(mount, exec_path)) {
        VGA_Write("\\crFile not found\\rr\n");
        return;
    }
    com_write_string(COM1_PORT, "[EXEC] File exists\n");

    // Stat the file
    com_write_string(COM1_PORT, "[EXEC] Getting file info...\n");
    fs_file_info_t info;
    if (fs_stat(mount, exec_path, &info) != 0) {
        VGA_Write("\\crFailed to get file info\\rr\n");
        com_write_string(COM1_PORT, "[EXEC] fs_stat failed\n");
        return;
    }
    com_write_string(COM1_PORT, "[EXEC] Got file info\n");

    if (info.is_directory) {
        VGA_Write("\\crError: Cannot exec a directory\\rr\n");
        return;
    }

    char size_str[16];
    itoa(info.size, size_str, 10);
    com_write_string(COM1_PORT, "[EXEC] File size: ");
    com_write_string(COM1_PORT, size_str);
    com_write_string(COM1_PORT, " bytes\n");

    // Allocate buffer
    com_write_string(COM1_PORT, "[EXEC] Calling kmalloc...\n");
    void *buffer = kmalloc(info.size);
    if (!buffer) {
        VGA_Write("\\crFailed to allocate memory\\rr\n");
        com_write_string(COM1_PORT, "[EXEC] kmalloc FAILED\n");
        return;
    }
    
    com_write_string(COM1_PORT, "[EXEC] kmalloc succeeded, buffer at 0x");
    uint64_t buf_addr = (uint64_t)buffer;
    for (int j = 15; j >= 0; j--) {
        uint8_t nibble = (buf_addr >> (j * 4)) & 0xF;
        char hex = nibble < 10 ? '0' + nibble : 'a' + (nibble - 10);
        com_write_byte(COM1_PORT, hex);
    }
    com_write_string(COM1_PORT, "\n");

    // Read file
    com_write_string(COM1_PORT, "[EXEC] Calling fs_read_file...\n");
    size_t bytes_read = 0;
    if (fs_read_file(mount, exec_path, buffer, info.size, &bytes_read) != 0) {
        VGA_Write("\\crFailed to read file\\rr\n");
        com_write_string(COM1_PORT, "[EXEC] fs_read_file FAILED\n");
        kfree(buffer);
        return;
    }
    
    com_write_string(COM1_PORT, "[EXEC] fs_read_file succeeded, read ");
    itoa(bytes_read, size_str, 10);
    com_write_string(COM1_PORT, size_str);
    com_write_string(COM1_PORT, " bytes\n");

    com_write_string(COM1_PORT, "[EXEC] About to call elf_load_with_args...\n");
    
    // Load ELF with arguments
    uint64_t entry_point;
    int elf_result = elf_load_with_args(buffer, bytes_read, &entry_point, argc, argv);
    
    com_write_string(COM1_PORT, "[EXEC] elf_load_with_args returned: ");
    itoa(elf_result, size_str, 10);
    com_write_string(COM1_PORT, size_str);
    com_write_string(COM1_PORT, "\n");
    
    if (elf_result != 0) {
        VGA_Write("\\crFailed to load ELF\\rr\n");
        com_write_string(COM1_PORT, "[EXEC] ELF load failed!\n");
        kfree(buffer);
        return;
    }
    
    // The ELF loader has already copied the data to new physical pages
    com_write_string(COM1_PORT, "[EXEC] Freeing temporary buffer\n");
    kfree(buffer); 

    com_write_string(COM1_PORT, "[EXEC] Entry point: 0x");
    for (int j = 15; j >= 0; j--) {
        uint8_t nibble = (entry_point >> (j * 4)) & 0xF;
        char hex = nibble < 10 ? '0' + nibble : 'a' + (nibble - 10);
        com_write_byte(COM1_PORT, hex);
    }
    com_write_string(COM1_PORT, "\n");

    // Extract filename
    const char *filename = exec_path;
    for (const char *p = exec_path; *p; p++) {
        if (*p == '/') filename = p + 1;
    }

    com_write_string(COM1_PORT, "[EXEC] Creating process...\n");
    
    // Create process with arguments if we have any
    process_t *proc;
    if (argc > 1) {
        // Skip argv[0] (program name) - pass remaining args
        proc = process_create_with_args(filename, (void(*)(void))entry_point, 1, argc, argv);
    } else {
        proc = process_create(filename, (void(*)(void))entry_point, 1);
    }

    if (!proc) {
        com_write_string(COM1_PORT, "[EXEC] Process creation FAILED\n");
        VGA_Write("\\crFailed to create process\\rr\n");
        com_write_string(COM1_PORT, "[EXEC] ===== END EXEC COMMAND =====\n\n");
        return;
    }
    
    // Save PID before yielding (proc may be freed after yield)
    int pid = proc->pid;
    
    com_write_string(COM1_PORT, "[EXEC] Process created successfully with PID ");
    itoa(pid, size_str, 10);
    com_write_string(COM1_PORT, size_str);
    com_write_string(COM1_PORT, "\n");

    // Show diagnostic info to COM only
    com_write_string(COM1_PORT, "[EXEC] RIP: 0x");
    for (int i = 15; i >= 0; i--) {
        uint8_t nibble = (proc->cpu_state.rip >> (i * 4)) & 0xF;
        char hex = nibble < 10 ? '0' + nibble : 'a' + (nibble - 10);
        com_write_byte(COM1_PORT, hex);
    }
    com_write_string(COM1_PORT, "\n");
    
    com_write_string(COM1_PORT, "[EXEC] RSP: 0x");
    for (int i = 15; i >= 0; i--) {
        uint8_t nibble = (proc->cpu_state.rsp >> (i * 4)) & 0xF;
        char hex = nibble < 10 ? '0' + nibble : 'a' + (nibble - 10);
        com_write_byte(COM1_PORT, hex);
    }
    com_write_string(COM1_PORT, "\n");
    
    // Check if memory at RIP is readable
    com_write_string(COM1_PORT, "[EXEC] First 16 bytes at RIP: ");
    uint8_t *code_ptr = (uint8_t *)proc->cpu_state.rip;
    for (int i = 0; i < 16; i++) {
        uint8_t byte = code_ptr[i];
        char h1 = (byte >> 4) < 10 ? '0' + (byte >> 4) : 'a' + ((byte >> 4) - 10);
        char h2 = (byte & 0xF) < 10 ? '0' + (byte & 0xF) : 'a' + ((byte & 0xF) - 10);
        com_write_byte(COM1_PORT, h1);
        com_write_byte(COM1_PORT, h2);
        com_write_byte(COM1_PORT, ' ');
    }
    com_write_string(COM1_PORT, "\n");
    
    // Log to serial
    com_write_string(COM1_PORT, "[EXEC] Executing process ");
    itoa(pid, size_str, 10);
    com_write_string(COM1_PORT, size_str);
    com_write_string(COM1_PORT, "\n");
    
    com_write_string(COM1_PORT, "[EXEC] Shell yielding to scheduler...\n");
    
    // Yield to run the process
    process_yield();
    
    // When we return here, the process has finished
    com_write_string(COM1_PORT, "[EXEC] Returned from yield - checking process status...\n");
    
    // Check if process still exists (use saved PID, not proc pointer!)
    proc = process_get_by_pid(pid);
    if (!proc) {
        com_write_string(COM1_PORT, "[EXEC] Process was reaped\n");
    } else {
        com_write_string(COM1_PORT, "[EXEC] Process state: ");
        itoa(proc->state, size_str, 10);
        com_write_string(COM1_PORT, size_str);
        if (proc->state == PROCESS_STATE_ZOMBIE) {
            com_write_string(COM1_PORT, " (exit code: ");
            itoa(proc->exit_code, size_str, 10);
            com_write_string(COM1_PORT, size_str);
            com_write_string(COM1_PORT, ")");
        }
        com_write_string(COM1_PORT, "\n");
    }
    
    com_write_string(COM1_PORT, "[EXEC] ===== END EXEC COMMAND =====\n\n");
}