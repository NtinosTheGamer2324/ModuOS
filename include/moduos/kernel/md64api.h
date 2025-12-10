#ifndef MD64API_H
#define MD64API_H

#include <stdint.h>  /* For uint64_t */

/* String type */
typedef const char* string;

/* 
 * MD64 API – System Information Structure (Fixed & Improved)
 */
typedef struct md64api_sysinfo_data
{
    /* --- Memory Info --- */
    uint64_t sys_available_ram;     /* Available RAM in MB */
    uint64_t sys_total_ram;         /* Total system RAM in MB */

    /* --- OS / Kernel Info --- */
    int SystemVersion;              /* OS major version */
    int KernelVersion;              /* Kernel version number */
    string KernelVendor;            /* NTSoftware / New Technologies Software */
    string os_name;                 /* ModuOS */
    string os_arch;                 /* AMD64 only (ARM version not implemented) */

    /* --- System Identity --- */
    string pcname;                  /* Host / computer name */
    string username;                /* Current user */
    string domain;                  /* Domain / workgroup */
    string kconsole;                /* VGA console / VBE text console */

    /* --- CPU Info --- */
    string cpu;                     /* Vendor string: GenuineIntel, AuthenticAMD, etc */
    string cpu_manufacturer;        /* Intel / AMD / etc */
    string cpu_model;               /* Core i5-13600K, Ryzen 7 5800X, etc */
    int cpu_cores;                  /* Physical cores */
    int cpu_threads;                /* Logical processors */
    int cpu_hyperthreading_enabled; /* 1 = enabled, 0 = disabled */
    int cpu_base_mhz;               /* Base clock in MHz */
    int cpu_max_mhz;                /* Max turbo clock in MHz */
    int cpu_cache_l1_kb;            /* L1 cache size */
    int cpu_cache_l2_kb;            /* L2 cache size */
    int cpu_cache_l3_kb;            /* L3 cache size */
    string cpu_flags;               /* SSE, SSE2, AVX, AVX2, AES, etc */

    /* --- Virtualization Info --- */
    int is_virtual_machine;         /* 1 = VM detected, 0 = physical */
    string virtualization_vendor;   /* VMware, VirtualBox, KVM, Hyper-V, etc */

    /* --- GPU Info --- */
    string gpu_name;                /* GPU model */
    int gpu_vram_mb;                /* VRAM in MB */

    /* --- Storage Info --- */
    uint64_t storage_total_mb;      /* Total storage space in MB */
    uint64_t storage_free_mb;       /* Free storage space in MB */
    string primary_disk_model;      /* Disk model name */

    /* --- Firmware / BIOS --- */
    string bios_vendor;             /* AMI, Phoenix, UEFI vendor */
    string bios_version;            /* BIOS/UEFI version */
    string motherboard_model;       /* Motherboard identifier */

    /* --- Security Features --- */
    int secure_boot_enabled;        /* 1 = Secure Boot enabled */
    int tpm_version;                /* 0 = none, 1.2 = 1, 2.0 = 2 */

} md64api_sysinfo_data;

md64api_sysinfo_data get_system_info(void);

#endif /* MD64API_H */