#include "moduos/kernel/md64api.h"
#include "moduos/drivers/Time/RTC.h"
#include "moduos/kernel/kernel.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/memory/memory.h"
#include "moduos/kernel/memory/phys.h"
#include "moduos/drivers/PCI/pci.h"
#include "moduos/fs/fd.h"
#include <stdint.h>
#include <stddef.h>

/* Global PC name buffer */
static char pcname[128] = "UNKNOWN-PC";

/* --- SMBIOS Helper Functions --- */

/* SMBIOS Entry Point structure (32-bit) at 0xF0000-0xFFFFF */
struct smbios_entry {
    char anchor[4];           /* "_SM_" */
    uint8_t checksum;
    uint8_t length;
    uint8_t major_version;
    uint8_t minor_version;
    uint16_t max_structure_size;
    uint8_t entry_point_revision;
    char formatted_area[5];
    char intermediate_anchor[5]; /* "_DMI_" */
    uint8_t intermediate_checksum;
    uint16_t structure_table_length;
    uint32_t structure_table_address;
    uint16_t num_structures;
    uint8_t bcd_revision;
} __attribute__((packed));

/* SMBIOS Structure Header */
struct smbios_header {
    uint8_t type;
    uint8_t length;
    uint16_t handle;
} __attribute__((packed));

/* SMBIOS structure addresses (cached after first scan) */
static uint32_t smbios_table_addr = 0;
static uint16_t smbios_table_length = 0;
static int smbios_initialized = 0;

/* Find SMBIOS entry point in memory */
static int find_smbios_entry(void) {
    if (smbios_initialized) return smbios_table_addr != 0;
    
    smbios_initialized = 1; /* Mark as attempted */
    
    /* CRITICAL FIX: Limit scan to safe BIOS area only */
    /* Search for SMBIOS in BIOS area 0xF0000-0xFFFFF */
    for (uint32_t addr = 0xF0000; addr < 0x100000; addr += 16) {
        /* SAFETY CHECK: Skip if address not identity mapped */
        if (addr < 0xF0000 || addr >= 0x100000) continue;
        
        struct smbios_entry *entry = (struct smbios_entry *)(uintptr_t)addr;
        
        /* Check for "_SM_" anchor */
        if (entry->anchor[0] == '_' && entry->anchor[1] == 'S' && 
            entry->anchor[2] == 'M' && entry->anchor[3] == '_') {
            
            /* Validate length is reasonable */
            if (entry->length < 16 || entry->length > 64) continue;
            
            /* Validate checksum */
            uint8_t sum = 0;
            uint8_t *ptr = (uint8_t *)entry;
            for (int i = 0; i < entry->length; i++) {
                sum += ptr[i];
            }
            
            if (sum == 0) {
                /* Additional safety: verify table address is reasonable */
                if (entry->structure_table_address >= 0x100000 && 
                    entry->structure_table_address < 0x100000000ULL &&
                    entry->structure_table_length > 0 &&
                    entry->structure_table_length < 0x10000) {
                    
                    smbios_table_addr = entry->structure_table_address;
                    smbios_table_length = entry->structure_table_length;
                    return 1;
                }
            }
        }
    }
    
    return 0;
}

/* Get string from SMBIOS structure (strings are after the formatted portion) */
static const char *get_smbios_string(struct smbios_header *header, uint8_t index) {
    if (index == 0) return "";
    
    char *str_area = (char *)header + header->length;
    
    /* Skip to the requested string */
    for (uint8_t i = 1; i < index; i++) {
        while (*str_area) str_area++;
        str_area++;
    }
    
    return str_area;
}

/* Type 0: BIOS Information */
struct smbios_bios_info {
    struct smbios_header header;
    uint8_t vendor;
    uint8_t version;
    uint16_t starting_segment;
    uint8_t release_date;
    uint8_t rom_size;
    uint64_t characteristics;
    /* ... more fields ... */
} __attribute__((packed));

/* Type 2: Baseboard Information */
struct smbios_baseboard_info {
    struct smbios_header header;
    uint8_t manufacturer;
    uint8_t product;
    uint8_t version;
    uint8_t serial_number;
    /* ... more fields ... */
} __attribute__((packed));

/* Find SMBIOS structure by type */
static void *find_smbios_structure(uint8_t type) {
    if (!find_smbios_entry()) return NULL;
    
    uint8_t *ptr = (uint8_t *)(uintptr_t)smbios_table_addr;
    uint8_t *end = ptr + smbios_table_length;
    
    while (ptr < end) {
        struct smbios_header *header = (struct smbios_header *)ptr;
        
        if (header->type == type) {
            return header;
        }
        
        /* Skip to next structure */
        ptr += header->length;
        
        /* Skip string area (terminated by double null) */
        while (ptr < end && !(*ptr == 0 && *(ptr+1) == 0)) {
            ptr++;
        }
        ptr += 2; /* Skip double null */
        
        /* Check for end of table */
        if (header->type == 127) break;
    }
    
    return NULL;
}

static const char *get_smbios_bios_vendor(void) {
    static char vendor_buf[64] = "";
    if (vendor_buf[0]) return vendor_buf;
    
    struct smbios_bios_info *bios = find_smbios_structure(0);
    if (!bios) return "";
    
    const char *vendor = get_smbios_string(&bios->header, bios->vendor);
    strncpy(vendor_buf, vendor, sizeof(vendor_buf) - 1);
    vendor_buf[sizeof(vendor_buf) - 1] = '\0';
    return vendor_buf;
}

static const char *get_smbios_bios_version(void) {
    static char version_buf[64] = "";
    if (version_buf[0]) return version_buf;
    
    struct smbios_bios_info *bios = find_smbios_structure(0);
    if (!bios) return "";
    
    const char *version = get_smbios_string(&bios->header, bios->version);
    strncpy(version_buf, version, sizeof(version_buf) - 1);
    version_buf[sizeof(version_buf) - 1] = '\0';
    return version_buf;
}

static const char *get_smbios_baseboard_product(void) {
    static char product_buf[64] = "";
    if (product_buf[0]) return product_buf;
    
    struct smbios_baseboard_info *board = find_smbios_structure(2);
    if (!board) return "";
    
    const char *product = get_smbios_string(&board->header, board->product);
    strncpy(product_buf, product, sizeof(product_buf) - 1);
    product_buf[sizeof(product_buf) - 1] = '\0';
    return product_buf;
}

/* --- UEFI Secure Boot Detection --- */
static int get_secure_boot_status(void) {
    /* SIMPLIFIED: Just return 0 for now to avoid dangerous memory scanning
     * Proper implementation would need EFI runtime services */
    return 0;
}

/* --- TPM Detection --- */
static int get_tpm_version(void) {
    /* SIMPLIFIED: Disable TPM detection to avoid dangerous ACPI table scanning
     * This was causing hangs due to accessing unmapped memory regions
     * Proper implementation needs safe ACPI table parsing with bounds checking */
    return 0;
}

/* Global PC name buffer */

/* --- CPUID helpers --- */
static inline void cpuid(uint32_t code, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    asm volatile("cpuid"
                 : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                 : "a"(code), "c"(0));
    if (a) *a = eax;
    if (b) *b = ebx;
    if (c) *c = ecx;
    if (d) *d = edx;
}

/* Read vendor (12 chars + NUL). buf must be >=13 bytes. */
static void get_cpu_vendor(char *buf)
{
    uint32_t eax, ebx, ecx, edx;
    cpuid(0, &eax, &ebx, &ecx, &edx);
    memcpy(&buf[0], &ebx, 4);
    memcpy(&buf[4], &edx, 4);
    memcpy(&buf[8], &ecx, 4);
    buf[12] = '\0';
}

/* Read brand string if extended leaves present (48 bytes + NUL). buf must be >=49. */
static void get_cpu_brand(char *buf, size_t bufsize)
{
    if (bufsize < 49) {
        if (bufsize) buf[0] = '\0';
        return;
    }

    uint32_t max_ext = 0;
    cpuid(0x80000000u, &max_ext, NULL, NULL, NULL);
    if (max_ext >= 0x80000004u) {
        uint32_t parts[12];
        cpuid(0x80000002u, &parts[0],  &parts[1],  &parts[2],  &parts[3]);
        cpuid(0x80000003u, &parts[4],  &parts[5],  &parts[6],  &parts[7]);
        cpuid(0x80000004u, &parts[8],  &parts[9],  &parts[10], &parts[11]);

        for (int i = 0; i < 12; ++i) {
            uint32_t v = parts[i];
            size_t off = (size_t)i * 4;
            buf[off + 0] = (char)(v & 0xFF);
            buf[off + 1] = (char)((v >> 8) & 0xFF);
            buf[off + 2] = (char)((v >> 16) & 0xFF);
            buf[off + 3] = (char)((v >> 24) & 0xFF);
        }
        buf[48] = '\0';

        /* trim leading spaces */
        size_t start = 0;
        while (start < 48 && buf[start] == ' ') start++;
        if (start > 0) {
            size_t i = 0;
            while (start + i <= 48) {
                buf[i] = buf[start + i];
                i++;
            }
        }
        /* right-trim */
        size_t end = strlen(buf);
        while (end > 0 && buf[end - 1] == ' ') end--;
        buf[end] = '\0';
    } else {
        buf[0] = '\0';
    }
}

/* Build CPU feature string */
static const char *get_cpu_flags_string(void)
{
    static char flags[128];
    flags[0] = '\0';

    uint32_t a,b,c,d;
    cpuid(1, &a, &b, &c, &d);

    #define APPEND_FLAG(cond, tok) do { \
        if (cond) { \
            size_t len = strlen(flags); \
            if (len < sizeof(flags)-1) { \
                snprintf(flags+len, sizeof(flags)-len, "%s ", tok); \
            } \
        } \
    } while(0)

    APPEND_FLAG(d & (1u<<25), "SSE");
    APPEND_FLAG(d & (1u<<26), "SSE2");
    APPEND_FLAG(c & (1u<<0),  "SSE3");
    APPEND_FLAG(c & (1u<<9),  "SSSE3");
    APPEND_FLAG(c & (1u<<19), "SSE4.1");
    APPEND_FLAG(c & (1u<<20), "SSE4.2");
    APPEND_FLAG(c & (1u<<12), "FMA");
    APPEND_FLAG(c & (1u<<28), "AVX");

    cpuid(7, &a, &b, &c, &d);
    APPEND_FLAG(b & (1u<<5), "AVX2");

    /* strip trailing space */
    size_t L = strlen(flags);
    if (L > 0 && flags[L-1] == ' ') flags[L-1] = '\0';

    #undef APPEND_FLAG
    return flags;
}

/* --- PCI helpers --- */
static pci_device_t *find_pci_class(uint8_t class_code)
{
    pci_scan_bus();
    int count = pci_get_device_count();
    for (int i = 0; i < count; ++i) {
        pci_device_t *dev = pci_get_device(i);
        if (!dev) continue;
        if (dev->class_code == class_code) return dev;
    }
    return NULL;
}

static const char *format_pci_vendor_device(uint16_t vendor, uint16_t device)
{
    static char buf[128];
    buf[0] = '\0';

    const char *vstr = pci_vendor_name(vendor);
    if (vstr && vstr[0] != '\0') {
        snprintf(buf, sizeof(buf), "%s (dev 0x%04X)", vstr, device);
    } else {
        snprintf(buf, sizeof(buf), "Vendor 0x%04X Device 0x%04X", vendor, device);
    }
    return buf;
}

/* Read PC name from file */
static void read_pcname_file(void)
{
    char pcname_buf[128];
    size_t bytes = 0;

    strncpy(pcname_buf, "UNKNOWN-PC", sizeof(pcname_buf) - 1);
    pcname_buf[sizeof(pcname_buf) - 1] = '\0';

    fs_mount_t* mount = kernel_get_boot_mount();
    if (mount && mount->valid) {
        if (fs_file_exists(mount, "/ModuOS/System64/pcname.txt")) {
            if (fs_read_file(mount, "/ModuOS/System64/pcname.txt",
                             pcname_buf, sizeof(pcname_buf) - 1, &bytes) == 0) {
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
            }
        }
    }

    strncpy(pcname, pcname_buf, sizeof(pcname) - 1);
    pcname[sizeof(pcname) - 1] = '\0';
}

/* --- Public API: get_system_info --- */
md64api_sysinfo_data get_system_info(void)
{
    static char cpu_vendor[13];
    static char cpu_brand[49];
    static char gpu_name_buf[128];
    static char disk_model_buf[128];

    /* zero static buffers */
    cpu_vendor[0] = 0;
    cpu_brand[0] = 0;
    gpu_name_buf[0] = 0;
    disk_model_buf[0] = 0;

    /* CPUID: vendor + brand + flags */
    get_cpu_vendor(cpu_vendor);
    get_cpu_brand(cpu_brand, sizeof(cpu_brand));
    const char *flags = get_cpu_flags_string();

    /* try to read pcname from disk/mount */
    read_pcname_file();

    md64api_sysinfo_data info;
    memset(&info, 0, sizeof(info));

    /* --- Memory Info --- */
    uint64_t total_frames = 0, free_frames = 0, used_frames = 0;
    phys_get_stats(&total_frames, &free_frames, &used_frames);
    
    /* Convert frames to MB (PAGE_SIZE = 4096 bytes) */
    info.sys_total_ram = (total_frames * PAGE_SIZE) / (1024 * 1024);
    info.sys_available_ram = (free_frames * PAGE_SIZE) / (1024 * 1024);

    /* --- OS / Kernel Info --- */
    info.SystemVersion = 0;
    info.KernelVersion = 0;
    info.KernelVendor = "New Technologies Software";
    info.os_name = "ModuOS";
    info.os_arch = "AMD64";

    /* --- System Identity --- */
    info.pcname = pcname;
    info.username = "mdman";
    info.domain = "SYSTEM";
    info.kconsole = "VGA Konsole";

    /* --- CPU Info --- */
    info.cpu = cpu_vendor[0] ? cpu_vendor : "";
    info.cpu_manufacturer = NULL;
    info.cpu_model = cpu_brand[0] ? cpu_brand : "";
    info.cpu_cores = 0;
    info.cpu_threads = 0;
    info.cpu_hyperthreading_enabled = 0;
    info.cpu_base_mhz = 0;
    info.cpu_max_mhz = 0;
    info.cpu_cache_l1_kb = 0;
    info.cpu_cache_l2_kb = 0;
    info.cpu_cache_l3_kb = 0;
    info.cpu_flags = flags[0] ? flags : "";

    /* Set cpu_manufacturer from vendor string */
    if (cpu_vendor[0]) {
        if (strncmp(cpu_vendor, "GenuineIntel", 12) == 0) 
            info.cpu_manufacturer = "Intel";
        else if (strncmp(cpu_vendor, "AuthenticAMD", 12) == 0) 
            info.cpu_manufacturer = "AMD";
        else 
            info.cpu_manufacturer = "";
    } else {
        info.cpu_manufacturer = "";
    }

    /* --- Virtualization Info --- */
    info.is_virtual_machine = 0;
    info.virtualization_vendor = "";

    /* Hypervisor detection: CPUID.1:ECX[31] */
    {
        uint32_t a,b,c,d;
        cpuid(1, &a,&b,&c,&d);
        if (c & (1u << 31)) {
            info.is_virtual_machine = 1;
            uint32_t max_leaf = 0;
            cpuid(0x40000000u, &max_leaf, NULL, NULL, NULL);
            if (max_leaf >= 0x40000000u) {
                static char hvendor[13];
                uint32_t h0,h1,h2,h3;
                cpuid(0x40000000u, &h0, &h1, &h2, &h3);
                memcpy(&hvendor[0], &h1, 4);
                memcpy(&hvendor[4], &h2, 4);
                memcpy(&hvendor[8], &h3, 4);
                hvendor[12] = 0;
                info.virtualization_vendor = hvendor;
            } else {
                info.virtualization_vendor = "";
            }
        }
    }

    /* --- GPU Info (from PCI) --- */
    {
        pci_device_t *gpu = find_pci_class(PCI_CLASS_DISPLAY);
        if (gpu) {
            const char *name = format_pci_vendor_device(gpu->vendor_id, gpu->device_id);
            snprintf(gpu_name_buf, sizeof(gpu_name_buf), "%s", name);
            info.gpu_name = gpu_name_buf;
            info.gpu_vram_mb = 0;
        } else {
            info.gpu_name = "";
            info.gpu_vram_mb = 0;
        }
    }

    /* --- Storage Info (from PCI storage class) --- */
    {
        pci_device_t *stor = find_pci_class(PCI_CLASS_STORAGE);
        if (stor) {
            const char *name = format_pci_vendor_device(stor->vendor_id, stor->device_id);
            snprintf(disk_model_buf, sizeof(disk_model_buf), "%s", name);
            info.primary_disk_model = disk_model_buf;
            info.storage_total_mb = 0;
            info.storage_free_mb = 0;
        } else {
            info.primary_disk_model = "";
            info.storage_total_mb = 0;
            info.storage_free_mb = 0;
        }
    }

        /* --- Firmware / BIOS --- */
    /* BIOS/UEFI information from SMBIOS tables */
    info.bios_vendor = get_smbios_bios_vendor();
    info.bios_version = get_smbios_bios_version();
    info.motherboard_model = get_smbios_baseboard_product();

    /* --- Security Features --- */
    info.secure_boot_enabled = get_secure_boot_status();
    info.tpm_version = get_tpm_version();
    
    return info;
}

md64api_date_time get_date_time(void) {
    rtc_datetime_t time;

    rtc_get_datetime(&time);

    md64api_date_time date_time;

    date_time.second = time.second;
    date_time.minute = time.minute;
    date_time.hour = time.hour;
    date_time.day = time.day;
    date_time.month = time.month;
    date_time.year = time.year;

    return date_time;
}