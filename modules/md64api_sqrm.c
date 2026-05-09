/*
 * md64api_sqrm.c - System information SQRM module (SQRM_TYPE_GENERIC)
 *
 * All system information collection lives here, not in the kernel.
 * Exposes data via $/dev/md64api/ devfs nodes.
 * Exports SQRM service "md64api" for inter-module use.
 */

#include <stdint.h>
#include <stddef.h>

#include "sqrm_sdk.h"
#include "moduos/kernel/multiboot2.h"
#include "moduos/fs/devfs.h"
#include "moduos/kernel/md64api_user.h"

/*
 * AMD64-optimised memory primitives using rep movsq / rep stosq.
 * Handles unaligned head/tail with byte ops, bulk with 8-byte stores.
 */

static void *memset(void *dst, int c, size_t n) {
    unsigned char *p = (unsigned char *)dst;
    unsigned char  v = (unsigned char)c;
    while (n--) *p++ = v;
    return dst;
}

static void *memcpy(void *dst, const void *src, size_t n) {
    unsigned char       *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
    return dst;
}

static void *memmove(void *dst, const void *src, size_t n) {
    unsigned char       *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d == s || n == 0) return dst;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n; s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

static int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    for (size_t i = 0; i < n; i++) {
        if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    }
    return 0;
}

static void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = (const unsigned char *)s;
    unsigned char v = (unsigned char)c;
    for (size_t i = 0; i < n; i++) {
        if (p[i] == v) return (void *)(p + i);
    }
    return (void *)0;
}


SQRM_DEFINE_MODULE(SQRM_TYPE_GENERIC, "md64api");

static void collect_sysinfo(void);

static const sqrm_kernel_api_t *g_api;
static const void              *g_mb2;
static md64api_sysinfo_data_u   g_sysinfo;

/* safe_copy: bounded copy that always NUL-terminates */
static void safe_copy(char *dst, size_t dsz, const char *src) {
    if (!dst || dsz == 0) return;
    if (!src) src = "";
    size_t i = 0;
    for (; i + 1 < dsz && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}

static ssize_t str_read(const char *src, void *buf, size_t count) {
    if (!src || !src[0]) src = "(unknown)";
    size_t len = strlen(src);
    if (len > count) len = count;
    memcpy(buf, src, len);
    return (ssize_t)len;
}

static ssize_t u32_read(uint32_t val, void *buf, size_t count) {
    char tmp[12];
    utoa((unsigned long)val, tmp, 10);
    size_t len = strlen(tmp);
    if (len > count) len = count;
    memcpy(buf, tmp, len);
    return (ssize_t)len;
}

static struct multiboot_tag *find_tag(uint32_t type) {
    return multiboot2_find_tag((void *)g_mb2, type);
}

/* ---- DevFS stubs ---- */

static void *devfs_open_stub(void *ctx, int flags) {
    g_api->com_write_string(0x3F8, "[md64api] OPEN Triggered\n");
    (void)flags;
    return ctx; /* return ctx as the handle */
}

static int devfs_close_stub(void *ctx) {
    (void)ctx;
    return 0;
}

static ssize_t devfs_write_stub(void *ctx, const void *buf, size_t count) {
    (void)ctx; (void)buf; (void)count;
    return -1; /* read-only nodes */
}

/* ---- Read handlers ---- */

static ssize_t rd_cmdline(void *ctx, void *buf, size_t n) {
    (void)ctx;
    struct multiboot_tag_string *t = (struct multiboot_tag_string *)find_tag(MULTIBOOT_TAG_TYPE_CMDLINE);
    return str_read(t ? t->string : "", buf, n);
}

static ssize_t rd_bootloader(void *ctx, void *buf, size_t n) {
    (void)ctx;
    struct multiboot_tag_string *t = (struct multiboot_tag_string *)find_tag(MULTIBOOT_TAG_TYPE_BOOT_LOADER_NAME);
    return str_read(t ? t->string : "", buf, n);
}

static ssize_t rd_ram_lower(void *ctx, void *buf, size_t n) {
    (void)ctx;
    struct multiboot_tag_basic_meminfo *t = (struct multiboot_tag_basic_meminfo *)find_tag(MULTIBOOT_TAG_TYPE_BASIC_MEMINFO);
    return u32_read(t ? t->mem_lower : 0, buf, n);
}

static ssize_t rd_ram_upper(void *ctx, void *buf, size_t n) {
    (void)ctx;
    struct multiboot_tag_basic_meminfo *t = (struct multiboot_tag_basic_meminfo *)find_tag(MULTIBOOT_TAG_TYPE_BASIC_MEMINFO);
    return u32_read(t ? t->mem_upper : 0, buf, n);
}

static ssize_t rd_sysinfo(void *ctx, void *buf, size_t count) {
    (void)ctx;
    g_api->com_write_string(0x3F8, "[md64api] sysinfo read triggered\n");
    collect_sysinfo();
    g_api->com_write_string(0x3F8, "[md64api] collect_sysinfo done\n");
    size_t n = count < sizeof(g_sysinfo) ? count : sizeof(g_sysinfo);
    g_api->com_write_string(0x3F8, "[md64api] count done\n");
    memcpy(buf, &g_sysinfo, n);
    g_api->com_write_string(0x3F8, "[md64api] memcpy(buf, &g_sysinfo, n); done\n ABOUT TO RETURN\n");
    return (ssize_t)n;
}

/* ---- sysinfo collection ---- */

#define ADDF(fl,flen,fsz,cond,nm) do { \
    if((cond)&&(flen)+sizeof(nm)<(fsz)){ \
        if(flen)(fl)[(flen)++]=' '; \
        const char *_n=(nm); \
        for(size_t _i=0;_n[_i];_i++)(fl)[(flen)++]=_n[_i]; \
    } \
} while(0)

static void collect_sysinfo(void) {
    md64api_sysinfo_data_u *o = &g_sysinfo;

    g_api->com_write_string(0x3F8, "[md64api] Starting sysinfo collection\n");
    g_api->com_write_string(0x3F8, "[md64api] Collecting Memory Info\n");
    /* Memory */
    if (g_api->phys_total_frames && g_api->phys_count_free_frames) {
        uint64_t tot = g_api->phys_total_frames();
        uint64_t fr  = g_api->phys_count_free_frames();
        o->sys_total_ram     = (tot * 4096ULL) / (1024ULL * 1024ULL);
        o->sys_available_ram = (fr  * 4096ULL) / (1024ULL * 1024ULL);
    } else {
        struct multiboot_tag_basic_meminfo *mt =
            (struct multiboot_tag_basic_meminfo *)find_tag(MULTIBOOT_TAG_TYPE_BASIC_MEMINFO);
        if (mt) o->sys_total_ram = o->sys_available_ram =
            (uint64_t)(mt->mem_upper + mt->mem_lower) / 1024ULL;
    }
    
    g_api->com_write_string(0x3F8, "[md64api] Memory info collected\n");
    g_api->com_write_string(0x3F8, "[md64api] Safe Copy Static Strings\n");
    /* Static strings */
    safe_copy(o->SystemVersion, sizeof(o->SystemVersion), "0.6.1");
    safe_copy(o->KernelVersion, sizeof(o->KernelVersion), "0.6.1");
    safe_copy(o->KernelVendor,  sizeof(o->KernelVendor),  "NTSoftware");
    safe_copy(o->os_name,       sizeof(o->os_name),       "ModuOS");
    safe_copy(o->os_arch,       sizeof(o->os_arch),       "AMD64");
    safe_copy(o->username,      sizeof(o->username),      "{dont_use_me}");
    safe_copy(o->kconsole,      sizeof(o->kconsole),      "TTYMAN.CTL");


    g_api->com_write_string(0x3F8, "[md64api] Start SMBIOS Checks\n");
    /* SMBIOS */
    if (g_api->get_smbios_field) {
        const char *mfr  = g_api->get_smbios_field(0);
        const char *prod = g_api->get_smbios_field(1);
        const char *bv   = g_api->get_smbios_field(2);
        const char *bver = g_api->get_smbios_field(3);
        if (mfr)  safe_copy(o->motherboard_model, sizeof(o->motherboard_model), mfr);
        if (prod) safe_copy(o->pcname,            sizeof(o->pcname),            prod);
        if (bv)   safe_copy(o->bios_vendor,       sizeof(o->bios_vendor),       bv);
        if (bver) safe_copy(o->bios_version,      sizeof(o->bios_version),      bver);
    }
    g_api->com_write_string(0x3F8, "[md64api] DONE SMBIOS Checks\n");
    g_api->com_write_string(0x3F8, "[md64api] Start GPU Checks\n");
    /* GPU */
    if (g_api->get_gpu_driver_name) {
        const char *gpu = g_api->get_gpu_driver_name();
        if (gpu) {
            safe_copy(o->gpu_name,   sizeof(o->gpu_name),   gpu);
            safe_copy(o->gpu_driver, sizeof(o->gpu_driver), gpu);
        }
    }
    o->gpu_vram_mb = 64;
    g_api->com_write_string(0x3F8, "[md64api] DONE GPU Checks\n");

g_api->com_write_string(0x3F8, "[md64api] Start Blockdev Checks\n");
    /* Primary disk model */
    if (g_api->block_get_info) {
        g_api->com_write_string(0x3F8, "[md64api] block_get_info is valid\n");
        for (blockdev_handle_t h = 1; h <= 4; h++) {
            g_api->com_write_string(0x3F8, "[md64api] Checking handle ");
            char hbuf[4] = {'0' + (char)h, '\n', 0};
            g_api->com_write_string(0x3F8, hbuf);

            g_api->com_write_string(0x3F8, "[md64api] About to memset info\n");
            blockdev_info_t info;
            memset(&info, 0, sizeof(info));
            g_api->com_write_string(0x3F8, "[md64api] memset done, calling block_get_info\n");

            int rc = g_api->block_get_info(h, &info);
            g_api->com_write_string(0x3F8, "[md64api] block_get_info returned\n");

            if (rc != 0) {
                g_api->com_write_string(0x3F8, "[md64api] block_get_info failed, skipping\n");
                continue;
            }

            g_api->com_write_string(0x3F8, "[md64api] block_get_info success, checking flags\n");
            if (info.flags & BLOCKDEV_F_REMOVABLE) {
                g_api->com_write_string(0x3F8, "[md64api] drive is removable, skipping\n");
                continue;
            }

            g_api->com_write_string(0x3F8, "[md64api] checking model string\n");
            if (info.model[0]) {
                g_api->com_write_string(0x3F8, "[md64api] model found: ");
                g_api->com_write_string(0x3F8, info.model);
                g_api->com_write_string(0x3F8, "\n");
                safe_copy(o->primary_disk_model, sizeof(o->primary_disk_model), info.model);
                g_api->com_write_string(0x3F8, "[md64api] safe_copy done, breaking\n");
                break;
            }

            g_api->com_write_string(0x3F8, "[md64api] model empty, continuing\n");
        }
        g_api->com_write_string(0x3F8, "[md64api] loop done\n");
    } else {
        g_api->com_write_string(0x3F8, "[md64api] block_get_info is NULL, skipping\n");
    }
    g_api->com_write_string(0x3F8, "[md64api] DONE Blockdev Checks\n");
    g_api->com_write_string(0x3F8, "[md64api] Start Uptime Checks\n");
    /* Uptime */
    if (g_api->get_system_ticks && g_api->ticks_to_ms) {
        uint64_t ms = g_api->ticks_to_ms(g_api->get_system_ticks());
        o->uptime_seconds = ms / 1000ULL;
    }

    g_api->com_write_string(0x3F8, "[md64api] Start CPUID Checks\n");
    /* CPUID: vendor (leaf 0) */
    {
        uint32_t ebx = 0, ecx = 0, edx = 0;
        __asm__ volatile("cpuid" : "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0u));
        char v[13] = {0};
        ((uint32_t *)v)[0] = ebx;
        ((uint32_t *)v)[1] = edx;
        ((uint32_t *)v)[2] = ecx;
        safe_copy(o->cpu,              sizeof(o->cpu),              v);
        safe_copy(o->cpu_manufacturer, sizeof(o->cpu_manufacturer), v);
    }

    /* CPUID: leaf 1 */
    uint32_t l1_ecx = 0, l1_edx = 0;
    {
        uint32_t eax = 0, ebx = 0;
        __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(l1_ecx), "=d"(l1_edx) : "a"(1u));
        o->cpu_hyperthreading_enabled = (l1_edx & (1u << 28)) ? 1 : 0;
        o->cpu_cores   = (ebx >> 16) & 0xFF;
        if (!o->cpu_cores) o->cpu_cores = 1;
        o->cpu_threads  = o->cpu_cores;
        o->cpu_base_mhz = 2400;
        o->cpu_max_mhz  = 3600;
        o->cpu_cache_l1_kb = 32;
        o->cpu_cache_l2_kb = 256;
        o->cpu_cache_l3_kb = 8192;
    }

    /* CPUID: brand (0x80000002-4) */
    {
        uint32_t mx = 0;
        __asm__ volatile("cpuid" : "=a"(mx) : "a"(0x80000000u) : "ebx", "ecx", "edx");
        if (mx >= 0x80000004u) {
            uint32_t b[12] = {0};
            __asm__ volatile("cpuid" : "=a"(b[0]),  "=b"(b[1]),  "=c"(b[2]),  "=d"(b[3])  : "a"(0x80000002u));
            __asm__ volatile("cpuid" : "=a"(b[4]),  "=b"(b[5]),  "=c"(b[6]),  "=d"(b[7])  : "a"(0x80000003u));
            __asm__ volatile("cpuid" : "=a"(b[8]),  "=b"(b[9]),  "=c"(b[10]), "=d"(b[11]) : "a"(0x80000004u));
            const char *bs = (const char *)b;
            while (*bs == ' ') bs++;
            safe_copy(o->cpu_model, sizeof(o->cpu_model), bs);
        } else {
            safe_copy(o->cpu_model, sizeof(o->cpu_model), o->cpu);
        }
    }

    /* CPUID: flags */
    {
        char fl[128] = {0};
        size_t flen = 0;
        ADDF(fl, flen, sizeof(fl), l1_edx & (1u << 25), "SSE");
        ADDF(fl, flen, sizeof(fl), l1_edx & (1u << 26), "SSE2");
        ADDF(fl, flen, sizeof(fl), l1_ecx & (1u << 0),  "SSE3");
        ADDF(fl, flen, sizeof(fl), l1_ecx & (1u << 9),  "SSSE3");
        ADDF(fl, flen, sizeof(fl), l1_ecx & (1u << 19), "SSE4.1");
        ADDF(fl, flen, sizeof(fl), l1_ecx & (1u << 20), "SSE4.2");
        ADDF(fl, flen, sizeof(fl), l1_ecx & (1u << 28), "AVX");
        ADDF(fl, flen, sizeof(fl), l1_ecx & (1u << 25), "AES");
        ADDF(fl, flen, sizeof(fl), l1_ecx & (1u << 1),  "PCLMUL");
        ADDF(fl, flen, sizeof(fl), l1_ecx & (1u << 30), "RDRAND");
        ADDF(fl, flen, sizeof(fl), l1_edx & (1u << 4),  "TSC");
        ADDF(fl, flen, sizeof(fl), l1_edx & (1u << 23), "MMX");
        fl[flen] = 0;
        safe_copy(o->cpu_flags, sizeof(o->cpu_flags), fl);
    }

    /* CPUID: VM detection */
    if (l1_ecx & (1u << 31)) {
        o->is_virtual_machine = 1;
        uint32_t hv[4] = {0};
        __asm__ volatile("cpuid" : "=a"(hv[0]), "=b"(hv[1]), "=c"(hv[2]), "=d"(hv[3]) : "a"(0x40000000u));
        char hs[13] = {0};
        ((uint32_t *)hs)[0] = hv[1];
        ((uint32_t *)hs)[1] = hv[2];
        ((uint32_t *)hs)[2] = hv[3];
        if      (memcmp(hs, "KVMKVMKVM",    9)  == 0) safe_copy(o->virtualization_vendor, sizeof(o->virtualization_vendor), "KVM/QEMU");
        else if (memcmp(hs, "VMwareVMware", 12)  == 0) safe_copy(o->virtualization_vendor, sizeof(o->virtualization_vendor), "VMware");
        else if (memcmp(hs, "VBoxVBoxVBox", 12)  == 0) safe_copy(o->virtualization_vendor, sizeof(o->virtualization_vendor), "VirtualBox");
        else if (memcmp(hs, "Microsoft Hv", 12)  == 0) safe_copy(o->virtualization_vendor, sizeof(o->virtualization_vendor), "Hyper-V");
        else safe_copy(o->virtualization_vendor, sizeof(o->virtualization_vendor), hs);
    }
    g_api->com_write_string(0x3F8, "[md64api] DONE CPUID Checks AND Litterly DONE\n");
}

/* ---- Service ---- */

typedef struct { int (*get_sysinfo)(void *out, size_t out_size); } md64api_service_t;

static int svc_get_sysinfo(void *out, size_t out_size) {
    if (!out) return -1;
    collect_sysinfo();
    size_t n = out_size < sizeof(g_sysinfo) ? out_size : sizeof(g_sysinfo);
    memcpy(out, &g_sysinfo, n);
    return 0;
}

static const md64api_service_t g_svc = { .get_sysinfo = svc_get_sysinfo };

/* ---- DevFS ops structs ---- */

static const devfs_owner_t g_owner = {
    .kind = DEVFS_OWNER_SQRM,
    .id   = "md64api",
};

static devfs_device_ops_t g_ops_cmdline = {
    .name        = "cmdline",
    .open        = devfs_open_stub,
    .read        = rd_cmdline,
    .write       = devfs_write_stub,
    .close       = devfs_close_stub,
    .can_replace = NULL,
};

static devfs_device_ops_t g_ops_bootloader = {
    .name        = "bootloader",
    .open        = devfs_open_stub,
    .read        = rd_bootloader,
    .write       = devfs_write_stub,
    .close       = devfs_close_stub,
    .can_replace = NULL,
};

static devfs_device_ops_t g_ops_ram_lower = {
    .name        = "lower",
    .open        = devfs_open_stub,
    .read        = rd_ram_lower,
    .write       = devfs_write_stub,
    .close       = devfs_close_stub,
    .can_replace = NULL,
};

static devfs_device_ops_t g_ops_ram_upper = {
    .name        = "upper",
    .open        = devfs_open_stub,
    .read        = rd_ram_upper,
    .write       = devfs_write_stub,
    .close       = devfs_close_stub,
    .can_replace = NULL,
};

static devfs_device_ops_t g_ops_sysinfo = {
    .name        = "sysinfo",
    .open        = devfs_open_stub,
    .read        = rd_sysinfo,
    .write       = devfs_write_stub,
    .close       = devfs_close_stub,
    .can_replace = NULL,
};

/* ---- Module init ---- */

int sqrm_init(const sqrm_kernel_api_t *api) {
    if (!api || !api->multiboot2_header || !api->devfs_register_path) return -1;

    g_api = api;
    g_mb2 = api->multiboot2_header;

    api->devfs_register_path("md64api/cmdline",    &g_ops_cmdline,    NULL);
    api->devfs_register_path("md64api/bootloader", &g_ops_bootloader, NULL);
    api->devfs_register_path("md64api/ram/lower",  &g_ops_ram_lower,  NULL);
    api->devfs_register_path("md64api/ram/upper",  &g_ops_ram_upper,  NULL);
    api->devfs_register_path("md64api/sysinfo",    &g_ops_sysinfo,    NULL);

    if (api->sqrm_service_register)
        api->sqrm_service_register("md64api", &g_svc, sizeof(g_svc));

    api->com_write_string(0x3F8, "[md64api] $/dev/md64api/ registered\n");
    return 0;
}