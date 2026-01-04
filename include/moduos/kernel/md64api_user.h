#ifndef MD64API_USER_H
#define MD64API_USER_H

#include <stdint.h>

/*
 * User-safe system info structure.
 * No pointers: all strings are copied into fixed-size buffers.
 */

typedef struct md64api_sysinfo_data_u {
    uint64_t sys_available_ram;
    uint64_t sys_total_ram;

    /* Version strings (copied from kernel; user-safe) */
    char SystemVersion[32];
    char KernelVersion[64];

    char KernelVendor[64];
    char os_name[32];
    char os_arch[16];

    char pcname[128];
    char username[32];
    char domain[32];
    char kconsole[64];

    char cpu[16];
    char cpu_manufacturer[16];
    char cpu_model[64];
    int cpu_cores;
    int cpu_threads;
    int cpu_hyperthreading_enabled;
    int cpu_base_mhz;
    int cpu_max_mhz;
    int cpu_cache_l1_kb;
    int cpu_cache_l2_kb;
    int cpu_cache_l3_kb;
    char cpu_flags[128];

    int is_virtual_machine;
    char virtualization_vendor[32];

    char gpu_name[128];
    int gpu_vram_mb;

    uint64_t storage_total_mb;
    uint64_t storage_free_mb;
    char primary_disk_model[128];

    char bios_vendor[64];
    char bios_version[64];
    char motherboard_model[64];

    int secure_boot_enabled;
    int tpm_version;
} md64api_sysinfo_data_u;

#endif
