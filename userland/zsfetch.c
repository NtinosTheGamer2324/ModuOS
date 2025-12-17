#include "libc.h"

int md_main(long argc, char** argv) {
    (void)argc;
    (void)argv;
    
    md64api_sysinfo_data *info = get_system_info();
    
    if (!info) {
        puts("Error: Cannot get system info");
        return 1;
    }
    
    /* Basic System Info */
    printf("%s@%s\n", info->username, info->pcname);
    puts("---------------------------");
    printf("OS: %s %s\n", info->os_name, info->os_arch);
    printf("Kernel: %s\n", info->KernelVendor);
    puts("");
    
    /* CPU - Most Important Info Only */
    printf("CPU: %s\n", info->cpu_model[0] ? info->cpu_model : info->cpu);
    if (info->cpu_flags && info->cpu_flags[0]) {
        printf("Features: %s\n", info->cpu_flags);
    }
    puts("");
    
    /* Memory */
    if (info->sys_total_ram > 0) {
        uint64_t used = info->sys_total_ram - info->sys_available_ram;
        printf("Memory: %llu MB / %llu MB\n", used, info->sys_total_ram);
    }
    puts("");
    
    /* GPU if available */
    if (info->gpu_name && info->gpu_name[0]) {
        printf("GPU: %s\n", info->gpu_name);
        puts("");
    }
    
    /* BIOS Info */
    if (info->bios_vendor && info->bios_vendor[0]) {
        printf("BIOS: %s", info->bios_vendor);
        if (info->bios_version && info->bios_version[0]) {
            printf(" %s", info->bios_version);
        }
        puts("");
        puts("");
    }
    
    /* Security - Compact */
    if (info->is_virtual_machine) {
        printf("VM: Yes (%s)\n", 
               info->virtualization_vendor[0] ? info->virtualization_vendor : "Unknown");
    }
    
    if (info->tpm_version > 0) {
        printf("TPM: %s\n", info->tpm_version == 2 ? "2.0" : "1.2");
    }
    
    if (info->secure_boot_enabled) {
        puts("Secure Boot: Enabled");
    }
    
    return 0;
}