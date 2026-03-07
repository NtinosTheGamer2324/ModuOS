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
#include "../sdk/sqrmlibc/include/sqrmlibc.h"

SQRM_DEFINE_MODULE(SQRM_TYPE_GENERIC, "md64api");

static void collect_sysinfo(void);

static const sqrm_kernel_api_t *g_api;
static const void              *g_mb2;
static md64api_sysinfo_data_u   g_sysinfo;

/* safe_copy: bounded copy that always NUL-terminates */
static void safe_copy(char *dst, size_t dsz, const char *src) {
    if (!dst || dsz == 0) return;
    strlcpy(dst, src ? src : "", dsz);
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

static ssize_t rd_cmdline(void *ctx,void *buf,size_t n) {
    (void)ctx;
    struct multiboot_tag_string *t=(struct multiboot_tag_string*)find_tag(MULTIBOOT_TAG_TYPE_CMDLINE);
    return str_read(t?t->string:"",buf,n);
}
static ssize_t rd_bootloader(void *ctx,void *buf,size_t n) {
    (void)ctx;
    struct multiboot_tag_string *t=(struct multiboot_tag_string*)find_tag(MULTIBOOT_TAG_TYPE_BOOT_LOADER_NAME);
    return str_read(t?t->string:"",buf,n);
}
static ssize_t rd_ram_lower(void *ctx,void *buf,size_t n) {
    (void)ctx;
    struct multiboot_tag_basic_meminfo *t=(struct multiboot_tag_basic_meminfo*)find_tag(MULTIBOOT_TAG_TYPE_BASIC_MEMINFO);
    return u32_read(t?t->mem_lower:0,buf,n);
}
static ssize_t rd_ram_upper(void *ctx,void *buf,size_t n) {
    (void)ctx;
    struct multiboot_tag_basic_meminfo *t=(struct multiboot_tag_basic_meminfo*)find_tag(MULTIBOOT_TAG_TYPE_BASIC_MEMINFO);
    return u32_read(t?t->mem_upper:0,buf,n);
}
static ssize_t rd_sysinfo(void *ctx,void *buf,size_t count) {
    (void)ctx;
    collect_sysinfo();
    size_t n=count<sizeof(g_sysinfo)?count:sizeof(g_sysinfo);
    memcpy(buf, &g_sysinfo, n);
    return (ssize_t)n;
}

#define ADDF(fl,flen,fsz,cond,nm) do { \
    if((cond)&&(flen)+sizeof(nm)<(fsz)){ \
        if(flen)(fl)[(flen)++]=' '; \
        const char *_n=(nm); \
        for(size_t _i=0;_n[_i];_i++)(fl)[(flen)++]=_n[_i]; \
    } \
} while(0)

static void collect_sysinfo(void) {
    md64api_sysinfo_data_u *o = &g_sysinfo;
    /* BSS is already zeroed; individual fields are overwritten below. */

    /* Memory */
    if (g_api->phys_total_frames && g_api->phys_count_free_frames) {
        uint64_t tot = g_api->phys_total_frames();
        uint64_t fr  = g_api->phys_count_free_frames();
        o->sys_total_ram     = (tot * 4096ULL) / (1024ULL*1024ULL);
        o->sys_available_ram = (fr  * 4096ULL) / (1024ULL*1024ULL);
    } else {
        struct multiboot_tag_basic_meminfo *mt =
            (struct multiboot_tag_basic_meminfo*)find_tag(MULTIBOOT_TAG_TYPE_BASIC_MEMINFO);
        if (mt) o->sys_total_ram = o->sys_available_ram =
            (uint64_t)(mt->mem_upper + mt->mem_lower) / 1024ULL;
    }

    /* Static strings */
    safe_copy(o->SystemVersion, sizeof(o->SystemVersion), "0.6.0");
    safe_copy(o->KernelVersion, sizeof(o->KernelVersion), "0.5.6");
    safe_copy(o->KernelVendor,  sizeof(o->KernelVendor),  "NTSoftware");
    safe_copy(o->os_name,       sizeof(o->os_name),       "ModuOS");
    safe_copy(o->os_arch,       sizeof(o->os_arch),       "AMD64");
    safe_copy(o->username,      sizeof(o->username),      "root");
    safe_copy(o->kconsole,      sizeof(o->kconsole),      "TTYMAN.CTL");

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

    /* GPU */
    if (g_api->get_gpu_driver_name) {
        const char *gpu = g_api->get_gpu_driver_name();
        if (gpu) {
            safe_copy(o->gpu_name,   sizeof(o->gpu_name),   gpu);
            safe_copy(o->gpu_driver, sizeof(o->gpu_driver), gpu);
        }
    }
    o->gpu_vram_mb = 64;

    /* Primary disk model — use block_get_info on cached handles only.
     * Avoid block_get_handle_for_vdrive during early init as it may
     * trigger I/O that is unsafe before the scheduler is fully running. */
    if (g_api->block_get_info) {
        for (blockdev_handle_t h = 1; h <= 4; h++) {
            blockdev_info_t info;
            memset(&info, 0, sizeof(info));
            if (g_api->block_get_info(h, &info) != 0) continue;
            /* Skip optical drives */
            if (info.flags & BLOCKDEV_F_REMOVABLE) continue;
            if (info.model[0]) {
                safe_copy(o->primary_disk_model, sizeof(o->primary_disk_model), info.model);
                break;
            }
        }
    }

    /* Uptime */
    if (g_api->get_system_ticks && g_api->ticks_to_ms) {
        uint64_t ms = g_api->ticks_to_ms(g_api->get_system_ticks());
        o->uptime_seconds = ms / 1000ULL;
    }

    /* CPUID: vendor (leaf 0) */
    {
        uint32_t ebx=0,ecx=0,edx=0;
        __asm__ volatile("cpuid":"=b"(ebx),"=c"(ecx),"=d"(edx):"a"(0u));
        char v[13]={0};
        ((uint32_t*)v)[0]=ebx; ((uint32_t*)v)[1]=edx; ((uint32_t*)v)[2]=ecx;
        safe_copy(o->cpu,              sizeof(o->cpu),              v);
        safe_copy(o->cpu_manufacturer, sizeof(o->cpu_manufacturer), v);
    }

    /* CPUID: leaf 1 */
    uint32_t l1_ecx=0, l1_edx=0;
    {
        uint32_t eax=0,ebx=0;
        __asm__ volatile("cpuid":"=a"(eax),"=b"(ebx),"=c"(l1_ecx),"=d"(l1_edx):"a"(1u));
        o->cpu_hyperthreading_enabled = (l1_edx&(1u<<28)) ? 1 : 0;
        o->cpu_cores   = (ebx>>16)&0xFF; if(!o->cpu_cores) o->cpu_cores=1;
        o->cpu_threads = o->cpu_cores;
        o->cpu_base_mhz=2400; o->cpu_max_mhz=3600;
        o->cpu_cache_l1_kb=32; o->cpu_cache_l2_kb=256; o->cpu_cache_l3_kb=8192;
    }

    /* CPUID: brand (0x80000002-4) */
    {
        uint32_t mx=0;
        __asm__ volatile("cpuid":"=a"(mx):"a"(0x80000000u):"ebx","ecx","edx");
        if (mx >= 0x80000004u) {
            uint32_t b[12]={0};
            __asm__ volatile("cpuid":"=a"(b[0]),"=b"(b[1]),"=c"(b[2]),"=d"(b[3]):"a"(0x80000002u));
            __asm__ volatile("cpuid":"=a"(b[4]),"=b"(b[5]),"=c"(b[6]),"=d"(b[7]):"a"(0x80000003u));
            __asm__ volatile("cpuid":"=a"(b[8]),"=b"(b[9]),"=c"(b[10]),"=d"(b[11]):"a"(0x80000004u));
            const char *bs=(const char*)b; while(*bs==' ') bs++;
            safe_copy(o->cpu_model, sizeof(o->cpu_model), bs);
        } else {
            safe_copy(o->cpu_model, sizeof(o->cpu_model), o->cpu);
        }
    }

    /* CPUID: flags */
    {
        char fl[128]={0}; size_t flen=0;
        ADDF(fl,flen,sizeof(fl),l1_edx&(1u<<25),"SSE");
        ADDF(fl,flen,sizeof(fl),l1_edx&(1u<<26),"SSE2");
        ADDF(fl,flen,sizeof(fl),l1_ecx&(1u<<0), "SSE3");
        ADDF(fl,flen,sizeof(fl),l1_ecx&(1u<<9), "SSSE3");
        ADDF(fl,flen,sizeof(fl),l1_ecx&(1u<<19),"SSE4.1");
        ADDF(fl,flen,sizeof(fl),l1_ecx&(1u<<20),"SSE4.2");
        ADDF(fl,flen,sizeof(fl),l1_ecx&(1u<<28),"AVX");
        ADDF(fl,flen,sizeof(fl),l1_ecx&(1u<<25),"AES");
        ADDF(fl,flen,sizeof(fl),l1_ecx&(1u<<1), "PCLMUL");
        ADDF(fl,flen,sizeof(fl),l1_ecx&(1u<<30),"RDRAND");
        ADDF(fl,flen,sizeof(fl),l1_edx&(1u<<4), "TSC");
        ADDF(fl,flen,sizeof(fl),l1_edx&(1u<<23),"MMX");
        fl[flen]=0;
        safe_copy(o->cpu_flags, sizeof(o->cpu_flags), fl);
    }

    /* CPUID: VM detection */
    if (l1_ecx & (1u<<31)) {
        o->is_virtual_machine = 1;
        uint32_t hv[4]={0};
        __asm__ volatile("cpuid":"=a"(hv[0]),"=b"(hv[1]),"=c"(hv[2]),"=d"(hv[3]):"a"(0x40000000u));
        char hs[13]={0};
        ((uint32_t*)hs)[0]=hv[1]; ((uint32_t*)hs)[1]=hv[2]; ((uint32_t*)hs)[2]=hv[3];
        if      (memcmp(hs,"KVMKVMKVM",9)==0)    safe_copy(o->virtualization_vendor,sizeof(o->virtualization_vendor),"KVM/QEMU");
        else if (memcmp(hs,"VMwareVMware",12)==0) safe_copy(o->virtualization_vendor,sizeof(o->virtualization_vendor),"VMware");
        else if (memcmp(hs,"VBoxVBoxVBox",12)==0) safe_copy(o->virtualization_vendor,sizeof(o->virtualization_vendor),"VirtualBox");
        else if (memcmp(hs,"Microsoft Hv",12)==0) safe_copy(o->virtualization_vendor,sizeof(o->virtualization_vendor),"Hyper-V");
        else safe_copy(o->virtualization_vendor,sizeof(o->virtualization_vendor),hs);
    }
}

typedef struct { int (*get_sysinfo)(void *out, size_t out_size); } md64api_service_t;

static int svc_get_sysinfo(void *out, size_t out_size) {
    if (!out) return -1;
    collect_sysinfo();
    size_t n = out_size < sizeof(g_sysinfo) ? out_size : sizeof(g_sysinfo);
    memcpy(out, &g_sysinfo, n);
    return 0;
}

static const md64api_service_t g_svc = { .get_sysinfo = svc_get_sysinfo };

/* Named ops structs at module scope avoid PIC/GOT issues with block-scope statics. */
static devfs_device_ops_t g_ops_cmdline    = { .name = "cmdline",    .read = rd_cmdline    };
static devfs_device_ops_t g_ops_bootloader = { .name = "bootloader", .read = rd_bootloader };
static devfs_device_ops_t g_ops_ram_lower  = { .name = "lower",      .read = rd_ram_lower  };
static devfs_device_ops_t g_ops_ram_upper  = { .name = "upper",      .read = rd_ram_upper  };
static devfs_device_ops_t g_ops_sysinfo    = { .name = "sysinfo",    .read = rd_sysinfo    };

int sqrm_init(const sqrm_kernel_api_t *api) {
    if (!api || !api->multiboot2_header || !api->devfs_register_path) return -1;

    g_api = api;
    g_mb2 = api->multiboot2_header;

    /* g_sysinfo is in BSS — zeroed by the loader. No explicit memset needed. */

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

