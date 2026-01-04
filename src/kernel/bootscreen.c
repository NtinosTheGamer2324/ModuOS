#include "moduos/kernel/bootscreen.h"
#include "moduos/kernel/burninimg/boot.h"

#include "moduos/kernel/multiboot2.h"
#include "moduos/kernel/md64api.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/memory/memory.h" // kmalloc/kfree
#include "moduos/kernel/COM/com.h"
#include "moduos/kernel/debug.h"
#include "moduos/fs/fs.h"
#include "moduos/kernel/kernel.h" // kernel_get_boot_mount
#include "moduos/drivers/graphics/VGA.h"
#include "moduos/drivers/graphics/bmp.h"
#include "moduos/kernel/panic.h"

/* Multiboot2 SMBIOS tag (type 13) */
struct __attribute__((packed)) multiboot_tag_smbios {
    uint32_t type;
    uint32_t size;
    uint8_t major;
    uint8_t minor;
    uint8_t reserved[6];
    uint8_t tables[0];
};

static inline char ascii_tolower(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c - 'A' + 'a');
    return c;
}

static int str_ieq_ascii(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (ascii_tolower(*a) != ascii_tolower(*b)) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

static int str_icontains(const char *hay, const char *needle) {
    if (!hay || !needle) return 0;
    size_t nlen = strlen(needle);
    if (nlen == 0) return 1;
    for (size_t i = 0; hay[i]; i++) {
        size_t j = 0;
        while (hay[i + j] && needle[j] && ascii_tolower(hay[i + j]) == ascii_tolower(needle[j])) {
            j++;
        }
        if (j == nlen) return 1;
    }
    return 0;
}

static const char *smbios_get_string(const uint8_t *struct_start, uint8_t struct_len, uint8_t idx, const uint8_t *limit) {
    if (idx == 0) return "";
    const uint8_t *p = struct_start + struct_len;
    uint8_t cur = 1;
    while (p < limit && !(p[0] == 0 && p[1] == 0)) {
        const char *s = (const char*)p;
        if (cur == idx) return s;
        // advance to next NUL
        while (p < limit && *p) p++;
        if (p < limit) p++; // skip NUL
        cur++;
    }
    return "";
}

typedef struct {
    const char *needle;
    const char *bmp;
} bootscreen_rule_t;

static int str_any_icontains(const char *a, const char *b, const char *needle) {
    return str_icontains(a, needle) || str_icontains(b, needle);
}

static int is_oem_placeholder(const char *s) {
    if (!s || !*s) return 1;
    if (str_icontains(s, "To Be Filled By O.E.M.")) return 1;
    if (str_icontains(s, "O.E.M.")) return 1;
    if (str_icontains(s, "System manufacturer")) return 1;
    if (str_icontains(s, "System Product Name")) return 1;
    if (str_icontains(s, "Default string")) return 1;
    return 0;
}

static int booted_via_uefi(void *mb2) {
    // Multiboot2: EFI system table tag (type 12) indicates UEFI boot.
    struct multiboot_tag *t = multiboot2_find_tag(mb2, MULTIBOOT_TAG_TYPE_EFI64);
    if (t) return 1;
    t = multiboot2_find_tag(mb2, MULTIBOOT_TAG_TYPE_EFI32);
    return t != NULL;
}

const char *bootscreen_pick_bmp_basename(void *mb2) {
    static const char *fallback = "Generic_bootimg.bmp";

    const char *manu = md64api_get_smbios_system_manufacturer();
    const char *prod = md64api_get_smbios_system_product();

    // Log detected strings (requested)
    com_write_string(COM1_PORT, "[BOOTSCREEN] SMBIOS manu=\"");
    com_write_string(COM1_PORT, manu ? manu : "");
    com_write_string(COM1_PORT, "\" prod=\"");
    com_write_string(COM1_PORT, prod ? prod : "");
    com_write_string(COM1_PORT, "\"\n");

    // 1) Branding/product keywords (wins)
    static const bootscreen_rule_t branding[] = {
        // Gaming/series branding (wins over manufacturer)
        {"ROG", "ROG_bootimg.bmp"},
        {"Republic of Gamers", "ROG_bootimg.bmp"},
        {"TUF", "TUF_bootimg.bmp"},
        {"AORUS", "AORUS_bootimg.bmp"},
        {"Alienware", "Alienware_bootimg.bmp"},
        {"Legion", "Legion_bootimg.bmp"},
        // Popular OEM / boutique gaming
        {"Razer", "Razer_bootimg.bmp"},
        {"NZXT", "NZXT_bootimg.bmp"},
        {"Origin", "Origin_bootimg.bmp"},
        {"iBUYPOWER", "iBUYPOWER_bootimg.bmp"},
        {"Cyberpower", "Cyberpower_bootimg.bmp"},
        {"CyberPower", "Cyberpower_bootimg.bmp"},
        {"MainGear", "MainGear_bootimg.bmp"},
        {"Maingear", "MainGear_bootimg.bmp"},
        // Framework
        {"Framework", "Framework_bootimg.bmp"},
        // Apple family
        {"Apple", "Apple_bootimg.bmp"},
        {"Apple Inc", "Apple_bootimg.bmp"},
        {"MacBook", "Apple_bootimg.bmp"},
        {"iMac", "Apple_bootimg.bmp"},
        {"Mac", "Apple_bootimg.bmp"},
        {"DevmanPC", "devmanpc_bootimg.bmp"}
    };
    for (size_t i = 0; i < sizeof(branding)/sizeof(branding[0]); i++) {
        if (str_any_icontains(manu, prod, branding[i].needle)) return branding[i].bmp;
    }

    // 2) Manufacturer keywords
    static const bootscreen_rule_t vendors[] = {
        // Motherboard vendors / OEMs
        {"ASRock", "ASRock_bootimg.bmp"},
        {"ASUSTeK", "ASUS_bootimg.bmp"},
        {"ASUS", "ASUS_bootimg.bmp"},
        {"Micro-Star", "MSI_bootimg.bmp"},
        {"Micro Star", "MSI_bootimg.bmp"},
        {"MSI", "MSI_bootimg.bmp"},
        {"Gigabyte", "Gigabyte_bootimg.bmp"},
        {"GIGABYTE", "Gigabyte_bootimg.bmp"},
        {"BIOSTAR", "BIOSTAR_bootimg.bmp"},
        {"EVGA", "EVGA_bootimg.bmp"},
        {"Supermicro", "Supermicro_bootimg.bmp"},
        {"TYAN", "Tyan_bootimg.bmp"},
        {"Tyan", "Tyan_bootimg.bmp"},

        // Major OEMs
        {"Acer", "Acer_bootimg.bmp"},
        {"DELL", "DELL_bootimg.bmp"},
        {"Dell", "DELL_bootimg.bmp"},
        {"HP", "HP_bootimg.bmp"},
        {"Hewlett-Packard", "HP_bootimg.bmp"},
        {"Hewlett Packard", "HP_bootimg.bmp"},
        {"Lenovo", "Lenovo_bootimg.bmp"},
        {"Samsung", "Samsung_bootimg.bmp"},
        {"Sony", "Sony_bootimg.bmp"},
        {"Toshiba", "Toshiba_bootimg.bmp"},
        {"LG", "LG_bootimg.bmp"},
        {"Fujitsu", "Fujitsu_bootimg.bmp"},
        {"Inspur", "Inspur_bootimg.bmp"},

        // Apple (also matched in branding)
        {"Apple", "Apple_bootimg.bmp"},

        // Microsoft hardware (Surface etc.)
        {"Microsoft", "Microsoft_bootimg.bmp"},

        //NTSoftware - NTLLC (idk man.)
        {"NTLLC", "ntllc_bootimg.bmp"}
    };
    for (size_t i = 0; i < sizeof(vendors)/sizeof(vendors[0]); i++) {
        if (str_any_icontains(manu, prod, vendors[i].needle)) return vendors[i].bmp;
    }

    // 3) VM/platform keywords
    static const bootscreen_rule_t platforms[] = {
        // Emulators / VMs (fuzzy matches)
        {"QEMU", "QEMU_bootimg.bmp"},
        {"VMware", "VMWare_bootimg.bmp"},
        {"VMW", "VMWare_bootimg.bmp"},
        {"VirtualBox", "VBox_bootimg.bmp"},
        {"VBox", "VBox_bootimg.bmp"},
        {"innotek", "VBox_bootimg.bmp"},
        {"Oracle", "VBox_bootimg.bmp"},
        {"Hyper-V", "HyperV_bootimg.bmp"},
        {"HyperV", "HyperV_bootimg.bmp"},
        {"Virtual Machine", "HyperV_bootimg.bmp"},
        {"Parallels", "Parallels_bootimg.bmp"},
    };
    for (size_t i = 0; i < sizeof(platforms)/sizeof(platforms[0]); i++) {
        if (str_any_icontains(manu, prod, platforms[i].needle)) return platforms[i].bmp;
    }

    // 4) UEFI "unknown OEM" fallback
    if (booted_via_uefi(mb2) && is_oem_placeholder(manu) && is_oem_placeholder(prod)) {
        return "UEFI_bootimg.bmp";
    }

    // 5) CPU fallback
    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0));

    char vendor[13];
    ((uint32_t*)vendor)[0] = ebx;
    ((uint32_t*)vendor)[1] = edx;
    ((uint32_t*)vendor)[2] = ecx;
    vendor[12] = 0;

    if (str_ieq_ascii(vendor, "GenuineIntel")) return "intel_bootimg.bmp";
    if (str_ieq_ascii(vendor, "AuthenticAMD")) return "AMD_bootimg.bmp";

    return fallback;
}

static uint32_t fb_pack_rgb888(const framebuffer_t *fb, uint8_t r, uint8_t g, uint8_t b) {
    if (!fb) return 0;
    if (fb->red_mask_size && fb->green_mask_size && fb->blue_mask_size) {
        uint32_t rp = fb->red_pos;
        uint32_t gp = fb->green_pos;
        uint32_t bp = fb->blue_pos;

        uint32_t rm = (fb->red_mask_size >= 32) ? 0xFFFFFFFFu : ((1u << fb->red_mask_size) - 1u);
        uint32_t gm = (fb->green_mask_size >= 32) ? 0xFFFFFFFFu : ((1u << fb->green_mask_size) - 1u);
        uint32_t bm = (fb->blue_mask_size >= 32) ? 0xFFFFFFFFu : ((1u << fb->blue_mask_size) - 1u);

        uint32_t rv = ((uint32_t)r * rm) / 255u;
        uint32_t gv = ((uint32_t)g * gm) / 255u;
        uint32_t bv = ((uint32_t)b * bm) / 255u;

        return (rv << rp) | (gv << gp) | (bv << bp);
    }

    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static void fb_put_pixel(const framebuffer_t *fb, uint32_t x, uint32_t y, uint32_t px) {
    if (!fb || !fb->addr) return;
    if (x >= fb->width || y >= fb->height) return;

    uint8_t *base = (uint8_t*)fb->addr;
    uint64_t off = (uint64_t)y * fb->pitch;

    if (fb->bpp == 32) {
        ((uint32_t*)(base + off))[x] = px;
    } else if (fb->bpp == 24) {
        uint8_t *p = base + off + (uint64_t)x * 3u;
        p[0] = (uint8_t)(px & 0xFF);
        p[1] = (uint8_t)((px >> 8) & 0xFF);
        p[2] = (uint8_t)((px >> 16) & 0xFF);
    } else if (fb->bpp == 16) {
        // RGB565 approximation
        uint8_t r = (uint8_t)((px >> 16) & 0xFF);
        uint8_t g = (uint8_t)((px >> 8) & 0xFF);
        uint8_t b = (uint8_t)(px & 0xFF);
        uint16_t rr = (uint16_t)((r * 31u) / 255u);
        uint16_t gg = (uint16_t)((g * 63u) / 255u);
        uint16_t bb = (uint16_t)((b * 31u) / 255u);
        ((uint16_t*)(base + off))[x] = (uint16_t)((rr << 11) | (gg << 5) | bb);
    }
}

static void bootscreen_blit_bmp(const framebuffer_t *fb, const bmp_image_t *img);
static void bootscreen_blit_burnin(const framebuffer_t *fb);

static int bootscreen_is_qemu(void) {
    /*
     * Detect virtualized environments. QEMU often reports:
     *  - "TCGTCGTCG" (software emulation)
     *  - "KVMKVMKVM" (KVM acceleration)
     * We treat both as "QEMU" for bootscreen stability/perf heuristics.
     */
    uint32_t eax=0, ebx=0, ecx=0, edx=0;

    /* Hypervisor present bit */
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
    int hv_present = (ecx >> 31) & 1;
    if (!hv_present) return 0;

    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x40000000));
    char hv[13];
    ((uint32_t*)hv)[0] = ebx;
    ((uint32_t*)hv)[1] = ecx;
    ((uint32_t*)hv)[2] = edx;
    hv[12] = 0;

    return str_ieq_ascii(hv, "TCGTCGTCG") || str_ieq_ascii(hv, "KVMKVMKVM") || str_ieq_ascii(hv, "QEMU");
}

static int g_overlay_enabled = 0;
static bmp_image_t g_overlay_img;
static uint8_t *g_overlay_bmp_buf = NULL;
static size_t g_overlay_bmp_size = 0;
static int g_overlay_ready = 0;

void bootscreen_overlay_set_enabled(int enabled) {
    (void)enabled;
    /*
     * TEMPORARILY DISABLED:
     * Overlay redraw has caused intermittent early-boot page faults.
     * Keep bootscreen rendering as a one-shot draw only.
     */
    g_overlay_enabled = 0;
}

void bootscreen_overlay_redraw(void) {
    /* Overlay disabled (see bootscreen_overlay_set_enabled). */
}

static void bootscreen_blit_burnin(const framebuffer_t *fb) {
    if (!fb || !fb->addr) return;

    /* burn-in image is stored as 0xRRGGBB values */
    const uint32_t src_w = GENERIC_BOOTIMG_WIDTH;
    const uint32_t src_h = GENERIC_BOOTIMG_HEIGHT;

    uint32_t dst_w = src_w;
    uint32_t dst_h = src_h;

    if (dst_w > fb->width || dst_h > fb->height) {
        uint32_t sx = (uint32_t)(((uint64_t)fb->width << 16) / dst_w);
        uint32_t sy = (uint32_t)(((uint64_t)fb->height << 16) / dst_h);
        uint32_t s = (sx < sy) ? sx : sy;
        if (s == 0) s = 1;
        dst_w = (uint32_t)(((uint64_t)dst_w * s) >> 16);
        dst_h = (uint32_t)(((uint64_t)dst_h * s) >> 16);
        if (dst_w == 0) dst_w = 1;
        if (dst_h == 0) dst_h = 1;
    }

    uint32_t off_x = (fb->width > dst_w) ? (fb->width - dst_w) / 2u : 0;
    uint32_t off_y = (fb->height > dst_h) ? (fb->height - dst_h) / 2u : 0;

    for (uint32_t y = 0; y < dst_h; y++) {
        uint32_t src_y = (uint32_t)(((uint64_t)y * src_h) / dst_h);
        for (uint32_t x = 0; x < dst_w; x++) {
            uint32_t src_x = (uint32_t)(((uint64_t)x * src_w) / dst_w);
            uint32_t rgb = (uint32_t)Generic_bootimg[src_y * src_w + src_x];
            uint8_t r = (uint8_t)((rgb >> 16) & 0xFF);
            uint8_t g = (uint8_t)((rgb >> 8) & 0xFF);
            uint8_t b = (uint8_t)(rgb & 0xFF);
            uint32_t px = fb_pack_rgb888(fb, r, g, b);
            fb_put_pixel(fb, off_x + x, off_y + y, px);
        }
    }
}

static void bootscreen_blit_bmp(const framebuffer_t *fb, const bmp_image_t *img) {
    if (!fb || !fb->addr) return;
    if (!fb || !fb->addr || !img || !img->pixel_data) return;

    int qemu_env = bootscreen_is_qemu();

    // Scale down if needed (nearest-neighbor). Always keep centered.
    uint32_t src_w = img->width;
    uint32_t src_h = img->height;
    if (src_w == 0 || src_h == 0) return;

    uint32_t dst_w = src_w;
    uint32_t dst_h = src_h;

    // If the image is larger than the screen, scale to fit.
    if (dst_w > fb->width || dst_h > fb->height) {
        // compute scale factor in fixed-point (16.16)
        uint32_t sx = (uint32_t)(((uint64_t)fb->width << 16) / dst_w);
        uint32_t sy = (uint32_t)(((uint64_t)fb->height << 16) / dst_h);
        uint32_t s = (sx < sy) ? sx : sy;
        if (s == 0) s = 1;
        dst_w = (uint32_t)(((uint64_t)dst_w * s) >> 16);
        dst_h = (uint32_t)(((uint64_t)dst_h * s) >> 16);
        if (dst_w == 0) dst_w = 1;
        if (dst_h == 0) dst_h = 1;
    }

    uint32_t off_x = (fb->width > dst_w) ? (fb->width - dst_w) / 2u : 0;
    uint32_t off_y = (fb->height > dst_h) ? (fb->height - dst_h) / 2u : 0;

    for (uint32_t y = 0; y < dst_h; y++) {
        /* In QEMU this can be very slow; print progress so it doesn't look like a hang. */
        if (qemu_env && (y % 64u) == 0u) {
            com_write_string(COM1_PORT, ".");
        }
        uint32_t src_y = (uint32_t)(((uint64_t)y * src_h) / dst_h);
        for (uint32_t x = 0; x < dst_w; x++) {
            uint32_t src_x = (uint32_t)(((uint64_t)x * src_w) / dst_w);
            uint8_t r,g,b,a;
            if (!bmp_get_pixel_rgba(img, src_x, src_y, &r, &g, &b, &a)) continue;
            (void)a;
            uint32_t px = fb_pack_rgb888(fb, r, g, b);
            fb_put_pixel(fb, off_x + x, off_y + y, px);
        }
    }
    if (qemu_env) com_write_string(COM1_PORT, "\n");
}

int bootscreen_show_early(void) {
    if (VGA_GetFrameBufferMode() != FB_MODE_GRAPHICS) return -1;

    framebuffer_t fb;
    if (VGA_GetFrameBuffer(&fb) != 0 || !fb.addr) return -2;

    bootscreen_blit_burnin(&fb);

    /* Do not enable overlay here; overlay is for the later BMP-based logo caching. */
    return 0;
}

int bootscreen_show(void *mb2) {
    (void)mb2;
    if (VGA_GetFrameBufferMode() != FB_MODE_GRAPHICS) return -1;

    framebuffer_t fb;
    if (VGA_GetFrameBuffer(&fb) != 0 || !fb.addr) return -2;

    const char *base = bootscreen_pick_bmp_basename(mb2);

    char path[256];
    path[0] = 0;
    strcat(path, BOOTSCREEN_DIR);
    strcat(path, base);

    com_write_string(COM1_PORT, "[BOOTSCREEN] Selected: ");
    com_write_string(COM1_PORT, path);
    com_write_string(COM1_PORT, "\n");

    fs_mount_t *mnt = kernel_get_boot_mount();
    if (!mnt || !mnt->valid) {
        // Early boot: filesystem not ready yet; keep silent.
        return -3;
    }

    fs_file_info_t info;
    if (fs_stat(mnt, path, &info) != 0 || info.is_directory || info.size < 64) {
        // fallback to Generic
        path[0] = 0;
        strcat(path, BOOTSCREEN_DIR);
        strcat(path, "Generic_bootimg.bmp");
        com_write_string(COM1_PORT, "[BOOTSCREEN] Fallback: ");
        com_write_string(COM1_PORT, path);
        com_write_string(COM1_PORT, "\n");
        if (fs_stat(mnt, path, &info) != 0 || info.is_directory || info.size < 64) return -3;
    }

    uint8_t *file_buf = (uint8_t*)kmalloc(info.size);
    if (!file_buf) return -4;

    size_t bytes_read = 0;
    if (fs_read_file(mnt, path, file_buf, info.size, &bytes_read) != 0 || bytes_read < 64) {
        /* Avoid kfree() here during early boot stability issues (QEMU). */
        return -5;
    }

    size_t file_sz = bytes_read;

    bmp_image_t img;
    int pr = bmp_parse(&img, file_buf, file_sz);
    if (pr != 0) {
        com_write_string(COM1_PORT, "[BOOTSCREEN] bmp_parse failed for selected image, rc=");
        char tmp[32];
        itoa(pr, tmp, 10);
        com_write_string(COM1_PORT, tmp);
        com_write_string(COM1_PORT, "\n");

        // If the selected image parses fail, fall back to Generic_bootimg.bmp.
        /* Avoid kfree() here during early boot stability issues (QEMU). */

        char gpath[256];
        gpath[0] = 0;
        strcat(gpath, BOOTSCREEN_DIR);
        strcat(gpath, "Generic_bootimg.bmp");

        com_write_string(COM1_PORT, "[BOOTSCREEN] Trying Generic: ");
        com_write_string(COM1_PORT, gpath);
        com_write_string(COM1_PORT, "\n");

        if (fs_stat(mnt, gpath, &info) != 0 || info.is_directory || info.size < 64) return -4;

        file_buf = (uint8_t*)kmalloc(info.size);
        if (!file_buf) return -4;

        bytes_read = 0;
        if (fs_read_file(mnt, gpath, file_buf, info.size, &bytes_read) != 0 || bytes_read < 64) {
            return -5;
        }

        file_sz = bytes_read;
        memset(&img, 0, sizeof(img));
        pr = bmp_parse(&img, file_buf, file_sz);
        if (pr != 0) {
            com_write_string(COM1_PORT, "[BOOTSCREEN] bmp_parse failed for Generic too, rc=");
            itoa(pr, tmp, 10);
            com_write_string(COM1_PORT, tmp);
            com_write_string(COM1_PORT, "\n");
            return -4;
        }
    }

    com_write_string(COM1_PORT, "[BOOTSCREEN] bmp_parse OK ");
    com_write_string(COM1_PORT, "w=");
    char n[32];
    itoa((int)img.width, n, 10); com_write_string(COM1_PORT, n);
    com_write_string(COM1_PORT, " h=");
    itoa((int)img.height, n, 10); com_write_string(COM1_PORT, n);
    com_write_string(COM1_PORT, " bpp=");
    itoa((int)img.bpp, n, 10); com_write_string(COM1_PORT, n);
    com_write_string(COM1_PORT, "\n");

    com_write_string(COM1_PORT, "[BOOTSCREEN] About to blit BMP\n");

    /* QEMU-only stability: occasionally the first blit after a cold start faults.
     * Instead of skipping permanently, do a small delay then try once.
     */
    int qemu_env = bootscreen_is_qemu();
    if (qemu_env) {
        com_write_string(COM1_PORT, "[BOOTSCREEN] QEMU/KVM detected; delaying then attempting BMP blit once\n");
        for (volatile uint64_t i = 0; i < 5000000ULL; i++) {
            __asm__ volatile("pause");
        }
    }

    /* Sanity-check BMP pointers (they must point inside file_buf).
     * We have seen intermittent QEMU-only early-boot faults where these pointers become
     * invalid; if that happens, fall back to the built-in boot image instead of crashing.
     */
    const uint8_t *fbase = file_buf;
    const uint8_t *fend = file_buf + file_sz;
    int bmp_ptrs_ok = 1;
    if (!img.pixel_data || img.pixel_data < fbase || img.pixel_data >= fend) bmp_ptrs_ok = 0;
    if (img.palette && (img.palette < fbase || img.palette >= fend)) bmp_ptrs_ok = 0;
    if (img.width == 0 || img.height == 0) bmp_ptrs_ok = 0;
    if (img.row_stride == 0 || img.pixel_data_size < img.row_stride) bmp_ptrs_ok = 0;
    if (img.bpp != 8 && img.bpp != 16 && img.bpp != 24 && img.bpp != 32) bmp_ptrs_ok = 0;

    if (!bmp_ptrs_ok) {
        com_write_string(COM1_PORT, "[BOOTSCREEN] WARNING: BMP pointers out of range; falling back to burn-in image\n");
        bootscreen_blit_burnin(&fb);
    } else {
        /* One-shot draw: blit directly. */
        bootscreen_blit_bmp(&fb, &img);
    }

    /* NOTE: In QEMU we have seen intermittent early-boot faults around kfree() here.
     * Keep the BMP buffer allocated (small leak) to prioritize boot stability.
     */
    return 0;
}
