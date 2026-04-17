#include "moduos/kernel/md64api.h"
#include "moduos/kernel/multiboot2.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/memory/memory.h"
#include "moduos/kernel/memory/phys.h"
#include "moduos/kernel/memory/paging.h"
#include "moduos/drivers/Time/RTC.h"
#include "moduos/kernel/COM/com.h"
#include "moduos/drivers/PCI/pci.h"
#include "moduos/kernel/kernel.h"
#include "moduos/kernel/gfx.h"
#include <stdint.h>
#include <stddef.h>

struct smbios_entry {
    char anchor[4];
    uint8_t checksum;
    uint8_t length;
    uint8_t major_version;
    uint8_t minor_version;
    uint16_t max_structure_size;
    uint8_t entry_point_revision;
    char formatted_area[5];
    char intermediate_anchor[5];
    uint8_t intermediate_checksum;
    uint16_t structure_table_length;
    uint32_t structure_table_address;
    uint16_t num_structures;
    uint8_t bcd_revision;
} __attribute__((packed));

struct smbios3_entry {
    char anchor[5];
    uint8_t checksum;
    uint8_t length;
    uint8_t major;
    uint8_t minor;
    uint8_t docrev;
    uint8_t entry_point_rev;
    uint8_t reserved;
    uint32_t structure_table_max_size;
    uint64_t structure_table_address;
} __attribute__((packed));

struct smbios_header {
    uint8_t type;
    uint8_t length;
    uint16_t handle;
} __attribute__((packed));

struct smbios_bios_info {
    struct smbios_header header;
    uint8_t vendor;
    uint8_t bios_version;
    uint16_t bios_start_segment;
    uint8_t bios_release_date;
    uint8_t bios_rom_size;
    uint64_t bios_characteristics;
} __attribute__((packed));

struct smbios_system_info {
    struct smbios_header header;
    uint8_t manufacturer;
    uint8_t product_name;
    uint8_t version;
    uint8_t serial_number;
    uint8_t uuid[16];
    uint8_t wake_up_type;
} __attribute__((packed));

struct __attribute__((packed)) multiboot_tag_smbios {
    uint32_t type;
    uint32_t size;
    uint8_t major;
    uint8_t minor;
    uint8_t reserved[6];
    uint8_t tables[0];
};

static uintptr_t smbios_table_addr   = 0;
static uint32_t  smbios_table_length = 0;
static int       smbios_initialized  = 0;

static const char *smbios_system_manufacturer = "";
static const char *smbios_system_product      = "";
static const char *smbios_bios_vendor         = "";
static const char *smbios_bios_version        = "";

static void com_print_dec64(uint64_t v) {
    char tmp[32];
    int pos = 0;
    if (v == 0) { com_write_string(COM1_PORT, "0"); return; }
    while (v > 0 && pos < 31) { tmp[pos++] = '0' + (v % 10); v /= 10; }
    for (int i = pos - 1; i >= 0; i--) {
        char c[2] = { tmp[i], 0 };
        com_write_string(COM1_PORT, c);
    }
}

static int checksum_ok(const uint8_t *p, size_t len) {
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++) sum = (uint8_t)(sum + p[i]);
    return sum == 0;
}

static const char *get_smbios_string(struct smbios_header *header, uint8_t index) {
    if (index == 0) return "";
    char *str = (char *)header + header->length;
    for (uint8_t i = 1; i < index; i++) {
        while (*str) str++;
        str++;
    }
    return str;
}

static void scan_smbios_tables(void) {
    if (!smbios_table_addr || !smbios_table_length) return;

    const uint8_t *base = (const uint8_t *)smbios_table_addr;
    const uint8_t *end  = base + smbios_table_length;

    for (const uint8_t *p = base; p < end; ) {
        struct smbios_header *header = (struct smbios_header *)p;
        if (header->type == 127) break;
        if (p + sizeof(*header) > end) break;

        if (header->type == 0 && header->length >= sizeof(struct smbios_bios_info)) {
            struct smbios_bios_info *bios = (struct smbios_bios_info *)p;
            smbios_bios_vendor  = get_smbios_string(header, bios->vendor);
            smbios_bios_version = get_smbios_string(header, bios->bios_version);
        }
        if (header->type == 1) {
            struct smbios_system_info *sys = (struct smbios_system_info *)p;
            smbios_system_manufacturer = get_smbios_string(header, sys->manufacturer);
            smbios_system_product      = get_smbios_string(header, sys->product_name);
        }

        p += header->length;
        /* Walk past the string section (terminated by double NUL). */
        while (p + 1 < end && !(p[0] == 0 && p[1] == 0)) p++;
        p += 2;
    }
}

/*
 * Translate a physical address to a kernel virtual address.
 * phys_to_virt_kernel covers addresses in the physmap direct-map region.
 * For addresses outside the physmap (e.g. low BIOS memory on some configs),
 * fall back to ioremap so we always get a valid virtual pointer.
 */
static uintptr_t phys_to_virt_safe(uint64_t phys) {
    void *v = phys_to_virt_kernel(phys);
    if (v) return (uintptr_t)v;
    /* phys_to_virt_kernel returned NULL — region not covered by physmap.
     * ioremap the containing page and offset into it. */
    uint64_t page_base = phys & ~(uint64_t)0xFFF;
    uint64_t offset    = phys &  (uint64_t)0xFFF;
    void *mapped = ioremap(page_base, 0x1000);
    if (!mapped) return 0;
    return (uintptr_t)mapped + offset;
}

static int find_smbios_entry(void) {
    if (smbios_initialized) return smbios_table_addr != 0;
    smbios_initialized = 1;

    /*
     * Scan the BIOS shadow area for SMBIOS entry points.
     * All accesses go through phys_to_virt_safe — never use raw physical
     * addresses as virtual pointers in long mode.
     */
    for (uint32_t phys = 0xF0000; phys < 0x100000; phys += 16) {
        uintptr_t virt = phys_to_virt_safe((uint64_t)phys);
        if (!virt) continue;

        /* SMBIOS3 (_SM3_) */
        struct smbios3_entry *e3 = (struct smbios3_entry *)virt;
        if (memcmp(e3->anchor, "_SM3_", 5) == 0 &&
            e3->length >= sizeof(struct smbios3_entry) && e3->length < 0x80 &&
            checksum_ok((const uint8_t *)e3, e3->length) &&
            e3->structure_table_address &&
            e3->structure_table_max_size > 0 &&
            e3->structure_table_max_size < 0x100000) {

            uintptr_t tbl = phys_to_virt_safe(e3->structure_table_address);
            if (!tbl) continue;
            smbios_table_addr   = tbl;
            smbios_table_length = e3->structure_table_max_size;
            com_write_string(COM1_PORT, "[SMBIOS] Legacy scan found SMBIOS3 entry\n");
            return 1;
        }

        /* SMBIOS2 (_SM_) */
        struct smbios_entry *e2 = (struct smbios_entry *)virt;
        if (memcmp(e2->anchor, "_SM_", 4) == 0 &&
            e2->length >= 16 && e2->length < 0x80 &&
            checksum_ok((const uint8_t *)e2, e2->length) &&
            e2->structure_table_address &&
            e2->structure_table_length > 0 &&
            e2->structure_table_length < 0x10000) {

            uintptr_t tbl = phys_to_virt_safe((uint64_t)e2->structure_table_address);
            if (!tbl) continue;
            smbios_table_addr   = tbl;
            smbios_table_length = e2->structure_table_length;
            com_write_string(COM1_PORT, "[SMBIOS] Legacy scan found SMBIOS2 entry\n");
            return 1;
        }
    }

    com_write_string(COM1_PORT, "[SMBIOS] Legacy scan found no entry\n");
    return 0;
}

void md64api_init_smbios_from_mb2(void *mb2) {
    if (!mb2) return;

    struct multiboot_tag *t = multiboot2_find_tag(mb2, MULTIBOOT_TAG_TYPE_SMBIOS);
    if (!t) {
        com_write_string(COM1_PORT, "[SMBIOS] MB2 tag not found; using legacy scan\n");
        find_smbios_entry();
        scan_smbios_tables();
        return;
    }

    com_write_string(COM1_PORT, "[SMBIOS] MB2 SMBIOS tag found\n");

    struct multiboot_tag_smbios *st = (struct multiboot_tag_smbios *)t;
    const uint8_t *base = st->tables;
    size_t avail = (size_t)st->size;
    if (avail < sizeof(*st) + 0x20) {
        com_write_string(COM1_PORT, "[SMBIOS] MB2 SMBIOS tag too small\n");
        goto fallback;
    }
    avail -= sizeof(*st);

    if (avail >= sizeof(struct smbios_entry) && memcmp(base, "_SM_", 4) == 0) {
        const struct smbios_entry *ep = (const struct smbios_entry *)base;
        size_t ep_len = ep->length;
        if (ep_len < 0x10 || ep_len > avail) {
            com_write_string(COM1_PORT, "[SMBIOS] MB2: invalid entry point length\n");
            goto fallback;
        }
        /*
         * In the MB2 tag the bootloader copies the SMBIOS tables inline
         * immediately after the entry point. `base` is already a valid
         * virtual pointer — no physical translation needed.
         */
        smbios_table_addr   = (uintptr_t)(base + ep_len);
        smbios_table_length = ep->structure_table_length;
        smbios_initialized  = 1;

        com_write_string(COM1_PORT, "[SMBIOS] MB2: ver=");
        com_print_dec64((uint64_t)st->major);
        com_write_string(COM1_PORT, ".");
        com_print_dec64((uint64_t)st->minor);
        com_write_string(COM1_PORT, " len=");
        com_print_dec64((uint64_t)smbios_table_length);
        com_write_string(COM1_PORT, "\n");

        scan_smbios_tables();
        return;
    }

    /* SMBIOS3 in MB2 tag */
    if (avail >= sizeof(struct smbios3_entry) && memcmp(base, "_SM3_", 5) == 0) {
        const struct smbios3_entry *ep3 = (const struct smbios3_entry *)base;
        size_t ep_len = ep3->length;
        if (ep_len < sizeof(struct smbios3_entry) || ep_len > avail) {
            com_write_string(COM1_PORT, "[SMBIOS] MB2: invalid SMBIOS3 entry point length\n");
            goto fallback;
        }
        smbios_table_addr   = (uintptr_t)(base + ep_len);
        smbios_table_length = ep3->structure_table_max_size;
        smbios_initialized  = 1;

        com_write_string(COM1_PORT, "[SMBIOS] MB2 SMBIOS3: ver=");
        com_print_dec64((uint64_t)st->major);
        com_write_string(COM1_PORT, ".");
        com_print_dec64((uint64_t)st->minor);
        com_write_string(COM1_PORT, " len=");
        com_print_dec64((uint64_t)smbios_table_length);
        com_write_string(COM1_PORT, "\n");

        scan_smbios_tables();
        return;
    }

    com_write_string(COM1_PORT, "[SMBIOS] MB2 tag present but unsupported format; using legacy scan\n");

fallback:
    find_smbios_entry();
    scan_smbios_tables();
}

const char *md64api_get_smbios_system_manufacturer(void) {
    if (!smbios_initialized) { find_smbios_entry(); scan_smbios_tables(); }
    return smbios_system_manufacturer ? smbios_system_manufacturer : "";
}

const char *md64api_get_smbios_system_product(void) {
    if (!smbios_initialized) { find_smbios_entry(); scan_smbios_tables(); }
    return smbios_system_product ? smbios_system_product : "";
}

const char *md64api_get_smbios_bios_vendor(void) {
    if (!smbios_initialized) { find_smbios_entry(); scan_smbios_tables(); }
    return smbios_bios_vendor ? smbios_bios_vendor : "";
}

const char *md64api_get_smbios_bios_version(void) {
    if (!smbios_initialized) { find_smbios_entry(); scan_smbios_tables(); }
    return smbios_bios_version ? smbios_bios_version : "";
}

/*
 * Indexed accessor wired into sqrm_kernel_api_t.get_smbios_field.
 * 0=manufacturer  1=product  2=bios_vendor  3=bios_version
 */
const char *md64api_sqrm_get_smbios_field(int field) {
    if (!smbios_initialized) { find_smbios_entry(); scan_smbios_tables(); }
    switch (field) {
        case 0: return smbios_system_manufacturer ? smbios_system_manufacturer : "";
        case 1: return smbios_system_product      ? smbios_system_product      : "";
        case 2: return smbios_bios_vendor         ? smbios_bios_vendor         : "";
        case 3: return smbios_bios_version        ? smbios_bios_version        : "";
        default: return "";
    }
}

static inline void cpuid(uint32_t leaf, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx) {
    uint32_t a = leaf, b = 0, c = 0, d = 0;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(a));
    if (eax) *eax = a; if (ebx) *ebx = b; if (ecx) *ecx = c; if (edx) *edx = d;
}

md64api_sysinfo_data get_system_info(void) {
    md64api_sysinfo_data info;
    memset(&info, 0, sizeof(info));

    uint64_t total_frames = 0, free_frames = 0, used_frames = 0;
    phys_get_stats(&total_frames, &free_frames, &used_frames);
    info.sys_total_ram     = (total_frames * 4096) / (1024 * 1024);
    info.sys_available_ram = (free_frames  * 4096) / (1024 * 1024);

    info.SystemVersion = "0.6.0";
    info.KernelVersion = "0.5.6";
    info.KernelVendor  = "NTSoftware";
    info.os_name       = "ModuOS";
    info.os_arch       = "AMD64";
    info.pcname        = "UNKNOWN-PC";
    info.username      = "root";
    info.domain        = "";
    info.kconsole      = "VGA";

    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;

    cpuid(0, &eax, &ebx, &ecx, &edx);
    static char cpu_vendor[13];
    ((uint32_t *)cpu_vendor)[0] = ebx;
    ((uint32_t *)cpu_vendor)[1] = edx;
    ((uint32_t *)cpu_vendor)[2] = ecx;
    cpu_vendor[12] = 0;
    info.cpu = cpu_vendor;

    if      (memcmp(cpu_vendor, "GenuineIntel", 12) == 0) info.cpu_manufacturer = "Intel";
    else if (memcmp(cpu_vendor, "AuthenticAMD", 12) == 0) info.cpu_manufacturer = "AMD";
    else                                                    info.cpu_manufacturer = cpu_vendor;

    info.cpu_model                = "";
    info.cpu_cores                = 1;
    info.cpu_threads              = 1;
    info.cpu_hyperthreading_enabled = 0;
    info.cpu_base_mhz             = 0;
    info.cpu_max_mhz              = 0;
    info.cpu_cache_l1_kb          = 0;
    info.cpu_cache_l2_kb          = 0;
    info.cpu_cache_l3_kb          = 0;
    info.cpu_flags                = "";

    info.is_virtual_machine    = 0;
    info.virtualization_vendor = "";

    cpuid(1, &eax, &ebx, &ecx, &edx);
    if (ecx & (1u << 31)) {
        info.is_virtual_machine = 1;
        uint32_t max_leaf = 0;
        cpuid(0x40000000u, &max_leaf, NULL, NULL, NULL);
        if (max_leaf >= 0x40000000u) {
            static char hvendor[13];
            uint32_t h0, h1, h2, h3;
            cpuid(0x40000000u, &h0, &h1, &h2, &h3);
            memcpy(&hvendor[0], &h1, 4);
            memcpy(&hvendor[4], &h2, 4);
            memcpy(&hvendor[8], &h3, 4);
            hvendor[12] = 0;
            info.virtualization_vendor = hvendor;
        }
    }

    pci_device_t *gpu = NULL;
    int gpu_count = pci_get_device_count();
    for (int i = 0; i < gpu_count; i++) {
        pci_device_t *dev = pci_get_device(i);
        if (dev && dev->class_code == PCI_CLASS_DISPLAY) { gpu = dev; break; }
    }
    if (gpu) {
        static char gpu_buf[64];
        snprintf(gpu_buf, sizeof(gpu_buf), "Vendor 0x%04X Device 0x%04X",
                 gpu->vendor_id, gpu->device_id);
        info.gpu_name = gpu_buf;
    } else {
        info.gpu_name = "";
    }

    const char *gpu_drv = gfx_get_sqrm_gpu_driver_name();
    if (gpu_drv && gpu_drv[0]) {
        static char gpu_driver_buf[64];
        snprintf(gpu_driver_buf, sizeof(gpu_driver_buf), "%s.sqrm", gpu_drv);
        info.gpu_driver = gpu_driver_buf;
    } else {
        info.gpu_driver = "";
    }
    info.gpu_vram_mb = 0;

    info.storage_total_mb   = 0;
    info.storage_free_mb    = 0;
    info.primary_disk_model = "";

    info.bios_vendor       = md64api_get_smbios_system_manufacturer();
    info.bios_version      = md64api_get_smbios_bios_version();
    info.motherboard_model = md64api_get_smbios_system_product();

    info.secure_boot_enabled = 0;
    info.tpm_version         = 0;

    return info;
}

md64api_date_time get_date_time(void) {
    rtc_datetime_t time;
    rtc_get_datetime(&time);

    md64api_date_time date_time;
    date_time.second = time.second;
    date_time.minute = time.minute;
    date_time.hour   = time.hour;
    date_time.day    = time.day;
    date_time.month  = time.month;
    date_time.year   = time.year;

    return date_time;
}