// exec_new.c - POSIX exec() implementation

#include "moduos/kernel/process/process_new.h"
#include "moduos/kernel/memory/kheap.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/memory/paging.h"
#include "moduos/kernel/loader/elf.h"
#include "moduos/kernel/COM/com.h"
#include "moduos/fs/fs.h"
#include "moduos/fs/hvfs.h"

// External functions (itoa is in string.h)

#define USER_STACK_SIZE 0x100000  // 1MB user stack
#define USER_STACK_TOP  0x0000800000000000ULL  // User stack grows down from here

#include "moduos/kernel/memory/phys.h"

// External functions
extern void amd64_enter_user_trampoline(void);

// Wrapper functions
static inline uint64_t create_user_page_table(void) {
    return paging_create_process_pml4();
}

static inline uint64_t alloc_physical_page(void) {
    return phys_alloc_frame();
}

static inline void map_user_page(uint64_t cr3, uint64_t vaddr, uint64_t paddr, int writable) {
    uint64_t flags = 0x07;  // Present, R/W, User
    if (!writable) flags &= ~0x02;  // Clear write bit
    uint64_t *pml4_virt = (uint64_t *)(cr3 + 0xFFFF800000000000ULL);  // Convert phys to virt
    paging_map_range_to_pml4(pml4_virt, vaddr, paddr, 0x1000, flags);
}

// Execute a new program in the current process
int do_exec(const char *path, char **argv, char **envp) {
    if (!current || !path) {
        com_write_string(COM1_PORT, "[EXEC] Invalid parameters\n");
        return -1;
    }
    
    com_write_string(COM1_PORT, "[EXEC] Executing ");
    com_write_string(COM1_PORT, path);
    com_write_string(COM1_PORT, " in PID ");
    char buf[16];
    itoa(current->pid, buf, 10);
    com_write_string(COM1_PORT, buf);
    com_write_string(COM1_PORT, "\n");
    
    // Read the executable file using HVFS
    void *file_data = NULL;
    size_t file_size = 0;
    
    if (hvfs_read(0, path, &file_data, &file_size) != 0 || !file_data) {
        com_write_string(COM1_PORT, "[EXEC] Failed to read file\n");
        return -1;
    }
    
    // Count argc
    int argc = 0;
    if (argv) {
        while (argv[argc]) argc++;
    }
    
    // Load ELF using existing kernel ELF loader
    uint64_t entry_point = 0;
    uint64_t image_base = 0;
    uint64_t image_end = 0;
    
    int result = elf_load_with_args(file_data, file_size, &entry_point, argc, argv, &image_base, &image_end);
    
    if (result != 0) {
        com_write_string(COM1_PORT, "[EXEC] ELF load failed\n");
        kfree(file_data);
        return -1;
    }
    
    // Free old argv/envp
    if (current->argv) {
        for (int i = 0; i < current->argc; i++) {
            if (current->argv[i]) kfree(current->argv[i]);
        }
        kfree(current->argv);
    }
    
    if (current->envp) {
        for (int i = 0; current->envp[i]; i++) {
            kfree(current->envp[i]);
        }
        kfree(current->envp);
    }
    
    // Copy argv
    current->argc = 0;
    if (argv) {
        while (argv[current->argc]) current->argc++;
        current->argv = kmalloc(sizeof(char *) * (current->argc + 1));
        for (int i = 0; i < current->argc; i++) {
            size_t len = strlen(argv[i]);
            current->argv[i] = kmalloc(len + 1);
            strncpy(current->argv[i], argv[i], len);
            current->argv[i][len] = 0;
        }
        current->argv[current->argc] = NULL;
    }
    
    // Copy envp
    if (envp) {
        int env_count = 0;
        while (envp[env_count]) env_count++;
        current->envp = kmalloc(sizeof(char *) * (env_count + 1));
        for (int i = 0; i < env_count; i++) {
            size_t len = strlen(envp[i]);
            current->envp[i] = kmalloc(len + 1);
            strncpy(current->envp[i], envp[i], len);
            current->envp[i][len] = 0;
        }
        current->envp[env_count] = NULL;
    }
    
    // Update process fields
    current->entry_point = entry_point;
    current->user_sp = USER_STACK_TOP;
    current->user_image_base = image_base;
    current->user_image_end = image_end;
    
    // Set up CPU context for user mode entry
    memset(&current->context, 0, sizeof(cpu_context_t));
    current->context.rip = (uint64_t)amd64_enter_user_trampoline;
    current->context.rsp = (uint64_t)current->kernel_stack + 16384 - 16;
    current->context.rflags = 0x202;  // IF=1
    
    // Store user entry point in r14, user stack in r15 (for trampoline)
    current->context.r14 = current->entry_point;
    current->context.r15 = current->user_sp;
    current->context.r12 = current->argc;
    current->context.r13 = (uint64_t)current->argv;
    
    kfree(file_data);
    
    com_write_string(COM1_PORT, "[EXEC] Loaded, entry point: 0x");
    char hex_buf[20];
    utoa((uint32_t)(entry_point >> 32), hex_buf, 16);
    com_write_string(COM1_PORT, hex_buf);
    utoa((uint32_t)(entry_point & 0xFFFFFFFF), hex_buf, 16);
    com_write_string(COM1_PORT, hex_buf);
    com_write_string(COM1_PORT, "\n");
    
    return 0;
}
