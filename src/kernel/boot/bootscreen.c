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
#include "moduos/kernel/panic.h"
#include "moduos/lib/ilib/kilib.h"

/* ── .ilib path ─────────────────────────────────────────────────────── */

#define BIMG_ILIB_PATH  "/ModuOS/shared/assets/bimg.ilib"

/* ── Image IDs ──────────────────────────────────────────────────────────
 *
 * Fill these in to match the IDs assigned by your .ilib compiler.
 * Every vendor/platform entry in bootscreen_pick_id() references one.
 *
 * ────────────────────────────────────────────────────────────────────── */
#define BIMG_ID_GENERIC         14   /* Generic_bootimg      */
#define BIMG_ID_ASUS            6   /* ASUS_bootimg         */
#define BIMG_ID_ROG             34   /* ROG_bootimg          */
#define BIMG_ID_TUF             39   /* TUF_bootimg          */
#define BIMG_ID_ASROCK          5   /* ASRock_bootimg       */
#define BIMG_ID_MSI             27   /* MSI_bootimg          */
#define BIMG_ID_AORUS           3   /* AORUS_bootimg        */
#define BIMG_ID_GIGABYTE        15   /* Gigabyte_bootimg     */
#define BIMG_ID_BIOSTAR         7   /* BIOSTAR_bootimg      */
#define BIMG_ID_EVGA            11   /* EVGA_bootimg         */
#define BIMG_ID_SUPERMICRO      37  /* Supermicro_bootimg   */
#define BIMG_ID_TYAN            40  /* Tyan_bootimg         */
#define BIMG_ID_ACER            0  /* Acer_bootimg         */
#define BIMG_ID_DELL            9  /* DELL_bootimg         */
#define BIMG_ID_HP              16  /* HP_bootimg           */
#define BIMG_ID_LENOVO          22  /* Lenovo_bootimg       */
#define BIMG_ID_SAMSUNG         35  /* Samsung_bootimg      */
#define BIMG_ID_SONY            36  /* Sony_bootimg         */
#define BIMG_ID_TOSHIBA         38  /* Toshiba_bootimg      */
#define BIMG_ID_LG              23  /* LG_bootimg           */
#define BIMG_ID_FUJITSU         13  /* Fujitsu_bootimg      */
#define BIMG_ID_INSPUR          19  /* Inspur_bootimg       */
#define BIMG_ID_APPLE           4  /* Apple_bootimg        */
#define BIMG_ID_MICROSOFT       26  /* Microsoft_bootimg    */
#define BIMG_ID_ALIENWARE       1  /* Alienware_bootimg    */
#define BIMG_ID_LEGION          21  /* Legion_bootimg       */
#define BIMG_ID_RAZER           33  /* Razer_bootimg        */
#define BIMG_ID_NZXT            29  /* NZXT_bootimg         */
#define BIMG_ID_ORIGIN          30  /* Origin_bootimg       */
#define BIMG_ID_IBUYPOWER       18  /* iBUYPOWER_bootimg    */
#define BIMG_ID_CYBERPOWER      8  /* Cyberpower_bootimg   */
#define BIMG_ID_MAINGEAR        24  /* MainGear_bootimg     */
#define BIMG_ID_FRAMEWORK       12  /* Framework_bootimg    */
#define BIMG_ID_QEMU            32  /* QEMU_bootimg         */
#define BIMG_ID_VMWARE          43  /* VMWare_bootimg       */
#define BIMG_ID_VBOX            42  /* VBox_bootimg         */
#define BIMG_ID_HYPERV          17  /* HyperV_bootimg       */
#define BIMG_ID_PARALLELS       31  /* Parallels_bootimg    */
#define BIMG_ID_UEFI            41  /* UEFI_bootimg         */
#define BIMG_ID_INTEL           20  /* intel_bootimg        */
#define BIMG_ID_AMD             2  /* AMD_bootimg          */
#define BIMG_ID_DEVMANPC        10  /* devmanpc_bootimg     */
#define BIMG_ID_MICHAEL         25  /* Michael_bootimg      */
#define BIMG_ID_NTLLC           28  /* ntllc_bootimg        */

/* ── Multiboot2 SMBIOS tag (type 13) ───────────────────────────────── */

struct __attribute__((packed)) multiboot_tag_smbios {
    uint32_t type;
    uint32_t size;
    uint8_t  major;
    uint8_t  minor;
    uint8_t  reserved[6];
    uint8_t  tables[0];
};

/* ── String helpers ─────────────────────────────────────────────────── */

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
        while (hay[i+j] && needle[j]
               && ascii_tolower(hay[i+j]) == ascii_tolower(needle[j]))
            j++;
        if (j == nlen) return 1;
    }
    return 0;
}

static int str_any_icontains(const char *a, const char *b, const char *needle) {
    return str_icontains(a, needle) || str_icontains(b, needle);
}

static int is_oem_placeholder(const char *s) {
    if (!s || !*s) return 1;
    if (str_icontains(s, "To Be Filled By O.E.M.")) return 1;
    if (str_icontains(s, "O.E.M."))                 return 1;
    if (str_icontains(s, "System manufacturer"))     return 1;
    if (str_icontains(s, "System Product Name"))     return 1;
    if (str_icontains(s, "Default string"))          return 1;
    return 0;
}

static int booted_via_uefi(void *mb2) {
    struct multiboot_tag *t = multiboot2_find_tag(mb2, MULTIBOOT_TAG_TYPE_EFI64);
    if (t) return 1;
    t = multiboot2_find_tag(mb2, MULTIBOOT_TAG_TYPE_EFI32);
    return t != NULL;
}

/* ── ID selection (mirrors old basename logic, returns numeric ID) ─── */

typedef struct {
    const char *needle;
    uint16_t    id;
} bootscreen_rule_t;

static uint16_t bootscreen_pick_id(void *mb2)
{
    const char *manu = md64api_get_smbios_system_manufacturer();
    const char *prod = md64api_get_smbios_system_product();

    com_write_string(COM1_PORT, "[BOOTSCREEN] SMBIOS manu=\"");
    com_write_string(COM1_PORT, manu ? manu : "");
    com_write_string(COM1_PORT, "\" prod=\"");
    com_write_string(COM1_PORT, prod ? prod : "");
    com_write_string(COM1_PORT, "\"\n");

    /* 1) Branding / product keywords (highest priority) */
    static const bootscreen_rule_t branding[] = {
        { "ROG",                BIMG_ID_ROG        },
        { "Republic of Gamers", BIMG_ID_ROG        },
        { "TUF",                BIMG_ID_TUF        },
        { "AORUS",              BIMG_ID_AORUS      },
        { "Alienware",          BIMG_ID_ALIENWARE  },
        { "Legion",             BIMG_ID_LEGION     },
        { "Razer",              BIMG_ID_RAZER      },
        { "NZXT",               BIMG_ID_NZXT       },
        { "Origin",             BIMG_ID_ORIGIN     },
        { "iBUYPOWER",          BIMG_ID_IBUYPOWER  },
        { "Cyberpower",         BIMG_ID_CYBERPOWER },
        { "CyberPower",         BIMG_ID_CYBERPOWER },
        { "MainGear",           BIMG_ID_MAINGEAR   },
        { "Maingear",           BIMG_ID_MAINGEAR   },
        { "Framework",          BIMG_ID_FRAMEWORK  },
        { "Apple Inc",          BIMG_ID_APPLE      },
        { "Apple",              BIMG_ID_APPLE      },
        { "MacBook",            BIMG_ID_APPLE      },
        { "iMac",               BIMG_ID_APPLE      },
        { "Mac",                BIMG_ID_APPLE      },
        { "DevmanPC",           BIMG_ID_DEVMANPC   },
        { "Michaelsoft-Binbows",BIMG_ID_MICHAEL    },
    };
    for (size_t i = 0; i < sizeof(branding)/sizeof(branding[0]); i++)
        if (str_any_icontains(manu, prod, branding[i].needle))
            return branding[i].id;

    /* 2) Manufacturer / OEM keywords */
    static const bootscreen_rule_t vendors[] = {
        { "ASRock",           BIMG_ID_ASROCK     },
        { "ASUSTeK",          BIMG_ID_ASUS       },
        { "ASUS",             BIMG_ID_ASUS       },
        { "Micro-Star",       BIMG_ID_MSI        },
        { "Micro Star",       BIMG_ID_MSI        },
        { "MSI",              BIMG_ID_MSI        },
        { "Gigabyte",         BIMG_ID_GIGABYTE   },
        { "GIGABYTE",         BIMG_ID_GIGABYTE   },
        { "BIOSTAR",          BIMG_ID_BIOSTAR    },
        { "EVGA",             BIMG_ID_EVGA       },
        { "Supermicro",       BIMG_ID_SUPERMICRO },
        { "TYAN",             BIMG_ID_TYAN       },
        { "Tyan",             BIMG_ID_TYAN       },
        { "Acer",             BIMG_ID_ACER       },
        { "DELL",             BIMG_ID_DELL       },
        { "Dell",             BIMG_ID_DELL       },
        { "HP",               BIMG_ID_HP         },
        { "Hewlett-Packard",  BIMG_ID_HP         },
        { "Hewlett Packard",  BIMG_ID_HP         },
        { "Lenovo",           BIMG_ID_LENOVO     },
        { "Samsung",          BIMG_ID_SAMSUNG    },
        { "Sony",             BIMG_ID_SONY       },
        { "Toshiba",          BIMG_ID_TOSHIBA    },
        { "LG",               BIMG_ID_LG         },
        { "Fujitsu",          BIMG_ID_FUJITSU    },
        { "Inspur",           BIMG_ID_INSPUR     },
        { "Apple",            BIMG_ID_APPLE      },
        { "Microsoft",        BIMG_ID_MICROSOFT  },
        { "NTLLC",            BIMG_ID_NTLLC      },
        { "Michaelsoft Studios", BIMG_ID_MICHAEL },
    };
    for (size_t i = 0; i < sizeof(vendors)/sizeof(vendors[0]); i++)
        if (str_any_icontains(manu, prod, vendors[i].needle))
            return vendors[i].id;

    /* 3) VM / platform keywords */
    static const bootscreen_rule_t platforms[] = {
        { "QEMU",             BIMG_ID_QEMU      },
        { "VMware",           BIMG_ID_VMWARE    },
        { "VMW",              BIMG_ID_VMWARE    },
        { "VirtualBox",       BIMG_ID_VBOX      },
        { "VBox",             BIMG_ID_VBOX      },
        { "innotek",          BIMG_ID_VBOX      },
        { "Oracle",           BIMG_ID_VBOX      },
        { "Hyper-V",          BIMG_ID_HYPERV    },
        { "HyperV",           BIMG_ID_HYPERV    },
        { "Virtual Machine",  BIMG_ID_HYPERV    },
        { "Parallels",        BIMG_ID_PARALLELS },
    };
    for (size_t i = 0; i < sizeof(platforms)/sizeof(platforms[0]); i++)
        if (str_any_icontains(manu, prod, platforms[i].needle))
            return platforms[i].id;

    /* 4) UEFI unknown-OEM fallback */
    if (booted_via_uefi(mb2) && is_oem_placeholder(manu) && is_oem_placeholder(prod))
        return BIMG_ID_UEFI;

    /* 5) CPU vendor fallback */
    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(0));
    char vendor[13];
    ((uint32_t*)vendor)[0] = ebx;
    ((uint32_t*)vendor)[1] = edx;
    ((uint32_t*)vendor)[2] = ecx;
    vendor[12] = '\0';

    if (str_ieq_ascii(vendor, "GenuineIntel")) return BIMG_ID_INTEL;
    if (str_ieq_ascii(vendor, "AuthenticAMD")) return BIMG_ID_AMD;

    return BIMG_ID_GENERIC;
}

/* ── Framebuffer helpers ─────────────────────────────────────────────── */

static uint32_t fb_pack_rgb888(const framebuffer_t *fb, uint8_t r, uint8_t g, uint8_t b) {
    if (!fb) return 0;
    if (fb->red_mask_size && fb->green_mask_size && fb->blue_mask_size) {
        uint32_t rp = fb->red_pos,   gp = fb->green_pos,   bp = fb->blue_pos;
        uint32_t rm = (fb->red_mask_size   >= 32) ? 0xFFFFFFFFu : ((1u << fb->red_mask_size)   - 1u);
        uint32_t gm = (fb->green_mask_size >= 32) ? 0xFFFFFFFFu : ((1u << fb->green_mask_size) - 1u);
        uint32_t bm = (fb->blue_mask_size  >= 32) ? 0xFFFFFFFFu : ((1u << fb->blue_mask_size)  - 1u);
        return (((uint32_t)r * rm / 255u) << rp)
             | (((uint32_t)g * gm / 255u) << gp)
             | (((uint32_t)b * bm / 255u) << bp);
    }
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static void fb_put_pixel(const framebuffer_t *fb, uint32_t x, uint32_t y, uint32_t px) {
    if (!fb || !fb->addr) return;
    if (x >= fb->width || y >= fb->height) return;

    uint8_t  *base = (uint8_t *)fb->addr;
    uint64_t  off  = (uint64_t)y * fb->pitch;

    if (fb->bpp == 32) {
        ((uint32_t *)(base + off))[x] = px;
    } else if (fb->bpp == 24) {
        uint8_t *p = base + off + (uint64_t)x * 3u;
        p[0] = (uint8_t)(px        & 0xFF);
        p[1] = (uint8_t)((px >> 8) & 0xFF);
        p[2] = (uint8_t)((px >>16) & 0xFF);
    } else if (fb->bpp == 16) {
        uint8_t r = (uint8_t)((px >> 16) & 0xFF);
        uint8_t g = (uint8_t)((px >>  8) & 0xFF);
        uint8_t b = (uint8_t)( px        & 0xFF);
        uint16_t rr = (uint16_t)((r * 31u) / 255u);
        uint16_t gg = (uint16_t)((g * 63u) / 255u);
        uint16_t bb = (uint16_t)((b * 31u) / 255u);
        ((uint16_t *)(base + off))[x] = (uint16_t)((rr << 11) | (gg << 5) | bb);
    }
}

/* ── QEMU detection ─────────────────────────────────────────────────── */

static int bootscreen_is_qemu(void) {
    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(1));
    if (!((ecx >> 31) & 1)) return 0;

    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(0x40000000));
    char hv[13];
    ((uint32_t *)hv)[0] = ebx;
    ((uint32_t *)hv)[1] = ecx;
    ((uint32_t *)hv)[2] = edx;
    hv[12] = '\0';
    return str_ieq_ascii(hv, "TCGTCGTCG")
        || str_ieq_ascii(hv, "KVMKVMKVM")
        || str_ieq_ascii(hv, "QEMU");
}

/* ── Burn-in blit (always available, no FS needed) ──────────────────── */

static void bootscreen_blit_burnin(const framebuffer_t *fb) {
    if (!fb || !fb->addr) return;

    const uint32_t src_w = GENERIC_BOOTIMG_WIDTH;
    const uint32_t src_h = GENERIC_BOOTIMG_HEIGHT;
    uint32_t dst_w = src_w, dst_h = src_h;

    if (dst_w > fb->width || dst_h > fb->height) {
        uint32_t sx = (uint32_t)(((uint64_t)fb->width  << 16) / dst_w);
        uint32_t sy = (uint32_t)(((uint64_t)fb->height << 16) / dst_h);
        uint32_t s  = (sx < sy) ? sx : sy;
        if (s == 0) s = 1;
        dst_w = (uint32_t)(((uint64_t)dst_w * s) >> 16); if (dst_w == 0) dst_w = 1;
        dst_h = (uint32_t)(((uint64_t)dst_h * s) >> 16); if (dst_h == 0) dst_h = 1;
    }

    uint32_t off_x = (fb->width  > dst_w) ? (fb->width  - dst_w) / 2u : 0;
    uint32_t off_y = (fb->height > dst_h) ? (fb->height - dst_h) / 2u : 0;

    for (uint32_t y = 0; y < dst_h; y++) {
        uint32_t src_y = (uint32_t)(((uint64_t)y * src_h) / dst_h);
        for (uint32_t x = 0; x < dst_w; x++) {
            uint32_t src_x = (uint32_t)(((uint64_t)x * src_w) / dst_w);
            uint32_t rgb   = (uint32_t)Generic_bootimg[src_y * src_w + src_x];
            uint8_t  r     = (uint8_t)((rgb >> 16) & 0xFF);
            uint8_t  g     = (uint8_t)((rgb >>  8) & 0xFF);
            uint8_t  b     = (uint8_t)( rgb         & 0xFF);
            fb_put_pixel(fb, off_x + x, off_y + y, fb_pack_rgb888(fb, r, g, b));
        }
    }
}

/* ── RGBA blit (pixels from kilib_load) ─────────────────────────────── */

static void bootscreen_blit_rgba(const framebuffer_t *fb,
                                 const uint8_t *pixels,
                                 uint16_t src_w, uint16_t src_h)
{
    if (!fb || !fb->addr || !pixels || src_w == 0 || src_h == 0) return;

    int qemu_env = bootscreen_is_qemu();

    uint32_t dst_w = src_w, dst_h = src_h;

    if (dst_w > fb->width || dst_h > fb->height) {
        uint32_t sx = (uint32_t)(((uint64_t)fb->width  << 16) / dst_w);
        uint32_t sy = (uint32_t)(((uint64_t)fb->height << 16) / dst_h);
        uint32_t s  = (sx < sy) ? sx : sy;
        if (s == 0) s = 1;
        dst_w = (uint32_t)(((uint64_t)dst_w * s) >> 16); if (dst_w == 0) dst_w = 1;
        dst_h = (uint32_t)(((uint64_t)dst_h * s) >> 16); if (dst_h == 0) dst_h = 1;
    }

    uint32_t off_x = (fb->width  > dst_w) ? (fb->width  - dst_w) / 2u : 0;
    uint32_t off_y = (fb->height > dst_h) ? (fb->height - dst_h) / 2u : 0;

    for (uint32_t y = 0; y < dst_h; y++) {
        if (qemu_env && (y % 64u) == 0u)
            com_write_string(COM1_PORT, ".");

        uint32_t src_y = (uint32_t)(((uint64_t)y * src_h) / dst_h);

        for (uint32_t x = 0; x < dst_w; x++) {
            uint32_t src_x = (uint32_t)(((uint64_t)x * src_w) / dst_w);

            /* RGBA: 4 bytes per pixel, top-down, no padding */
            const uint8_t *px = pixels + ((uint64_t)src_y * src_w + src_x) * 4u;
            uint8_t r = px[0], g = px[1], b = px[2];
            /* px[3] is alpha — ignored; background is always black */

            fb_put_pixel(fb, off_x + x, off_y + y, fb_pack_rgb888(fb, r, g, b));
        }
    }

    if (qemu_env) com_write_string(COM1_PORT, "\n");
}

/* ── Overlay (kept as no-op; same rationale as before) ──────────────── */

static int g_overlay_enabled = 0;

void bootscreen_overlay_set_enabled(int enabled) {
    (void)enabled;
    /*
     * TEMPORARILY DISABLED:
     * Overlay redraw has caused intermittent early-boot page faults.
     */
    g_overlay_enabled = 0;
}

void bootscreen_overlay_redraw(void) {
    /* Overlay disabled; see bootscreen_overlay_set_enabled(). */
}

/* ── Public API ─────────────────────────────────────────────────────── */

int bootscreen_show_early(void) {
    if (VGA_GetFrameBufferMode() != FB_MODE_GRAPHICS) return -1;

    framebuffer_t fb;
    if (VGA_GetFrameBuffer(&fb) != 0 || !fb.addr) return -2;

    bootscreen_blit_burnin(&fb);
    return 0;
}

int bootscreen_show(void *mb2) {
    (void)mb2;
    if (VGA_GetFrameBufferMode() != FB_MODE_GRAPHICS) return -1;

    framebuffer_t fb;
    if (VGA_GetFrameBuffer(&fb) != 0 || !fb.addr) return -2;

    /* ── Open the ilib ── */
    fs_mount_t *mnt = kernel_get_boot_mount();
    if (!mnt || !mnt->valid) {
        com_write_string(COM1_PORT, "[BOOTSCREEN] Boot mount not ready; using burn-in image\n");
        bootscreen_blit_burnin(&fb);
        return 0;
    }

    kilib_t      *lib = (kilib_t *)0;
    kilib_error_t kerr = kilib_open(mnt, BIMG_ILIB_PATH, &lib);
    if (kerr != KILIB_OK) {
        com_write_string(COM1_PORT, "[BOOTSCREEN] kilib_open failed (");
        com_write_string(COM1_PORT, kilib_strerror(kerr));
        com_write_string(COM1_PORT, "); using burn-in image\n");
        bootscreen_blit_burnin(&fb);
        return 0;
    }

    /* ── Pick image ID ── */
    uint16_t id = bootscreen_pick_id(mb2);

    char id_str[8];
    itoa((int)id, id_str, 10);
    com_write_string(COM1_PORT, "[BOOTSCREEN] Selected image ID: ");
    com_write_string(COM1_PORT, id_str);
    com_write_string(COM1_PORT, "\n");

    /* ── Decompress ── */
    uint8_t  *pixels = (uint8_t *)0;
    uint16_t  w = 0, h = 0;

    kerr = kilib_load(lib, id, &pixels, &w, &h);
    if (kerr != KILIB_OK) {
        com_write_string(COM1_PORT, "[BOOTSCREEN] kilib_load id=");
        com_write_string(COM1_PORT, id_str);
        com_write_string(COM1_PORT, " failed (");
        com_write_string(COM1_PORT, kilib_strerror(kerr));
        com_write_string(COM1_PORT, "); trying generic (id=0)\n");

        /* Retry with Generic */
        kerr = kilib_load(lib, BIMG_ID_GENERIC, &pixels, &w, &h);
        if (kerr != KILIB_OK) {
            com_write_string(COM1_PORT, "[BOOTSCREEN] kilib_load generic failed (");
            com_write_string(COM1_PORT, kilib_strerror(kerr));
            com_write_string(COM1_PORT, "); using burn-in image\n");
            kilib_close(lib);
            bootscreen_blit_burnin(&fb);
            return 0;
        }
    }

    com_write_string(COM1_PORT, "[BOOTSCREEN] Decompressed OK w=");
    char n[16];
    itoa((int)w, n, 10); com_write_string(COM1_PORT, n);
    com_write_string(COM1_PORT, " h=");
    itoa((int)h, n, 10); com_write_string(COM1_PORT, n);
    com_write_string(COM1_PORT, "\n");

    /* ── QEMU delay (same rationale as before) ── */
    if (bootscreen_is_qemu()) {
        com_write_string(COM1_PORT,
            "[BOOTSCREEN] QEMU/KVM detected; delaying then blitting\n");
        for (volatile uint64_t i = 0; i < 5000000ULL; i++)
            __asm__ volatile("pause");
    }

    com_write_string(COM1_PORT, "[BOOTSCREEN] About to blit RGBA\n");

    bootscreen_blit_rgba(&fb, pixels, w, h);

    /* Flush paravirtual GPU */
    VGA_FlushRect(0, 0, fb.width, fb.height);

    /* Free decompressed pixel buffer; kilib owns nothing else at this point */
    kfree(pixels);
    kilib_close(lib);

    return 0;
}