/*
 * md64api_sqrm.c - System information SQRM module (SQRM_TYPE_GENERIC)
 *
 * All system information collection lives here, not in the kernel.
 * Exposes data via $/dev/md64api/ devfs nodes.
 * Exports SQRM service "md64api" for inter-module use.
 *
 * DEBUG BUILD: every step of collect_sysinfo() is logged to COM1.
 * The last "[md64api]" line before a fault tells you exactly where it died.
 */

#include <stdint.h>
#include <stddef.h>

#include "sqrm_sdk.h"
#include "moduos/kernel/multiboot2.h"
#include "moduos/fs/devfs.h"
#include "moduos/kernel/md64api_user.h"

/*
 * ENTRY: every function the kernel/dispatcher can call into is marked so GCC
 * re-aligns the stack to 16 bytes on entry. Without this, any SSE aligned
 * store (movaps/movdqa) the compiler emits for a local array or initializer
 * raises #GP (error code 0) when the caller's stack is only 8-byte aligned.
 */
#define ENTRY  __attribute__((force_align_arg_pointer))
#define UNUSED __attribute__((unused))

/*
 * Memory primitives as inline asm (rep movsb / rep stosb).
 * GCC cannot turn these into calls to memset/memcpy (no self-recursion),
 * and they never emit SSE.
 */
static void *memset(void *dst, int c, size_t n) {
    void *d = dst;
    __asm__ volatile("cld; rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
    return dst;
}

static void *memcpy(void *dst, const void *src, size_t n) {
    void       *d = dst;
    const void *s = src;
    __asm__ volatile("cld; rep movsb" : "+D"(d), "+S"(s), "+c"(n) :: "memory");
    return dst;
}

UNUSED static void *memmove(void *dst, const void *src, size_t n) {
    unsigned char       *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d == s || n == 0) return dst;
    if (d < s) {
        while (n--) { *d++ = *s++; __asm__ volatile("" ::: "memory"); }
    } else {
        d += n; s += n;
        while (n--) { *--d = *--s; __asm__ volatile("" ::: "memory"); }
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

UNUSED static void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = (const unsigned char *)s;
    unsigned char v = (unsigned char)c;
    for (size_t i = 0; i < n; i++) {
        if (p[i] == v) return (void *)(p + i);
    }
    return (void *)0;
}


SQRM_DEFINE_MODULE(SQRM_TYPE_GENERIC, "md64api");

static ENTRY void collect_sysinfo(void);

static const sqrm_kernel_api_t *g_api;
static const void              *g_mb2;
static md64api_sysinfo_data_u   g_sysinfo;

/* ---- Serial logging helpers ---- */

static void mlog(const char *s) {
    if (g_api && g_api->com_write_string) g_api->com_write_string(0x3F8, s);
}

static void mlog_hex(const char *label, uint64_t v) {
    static const char hx[] = "0123456789abcdef";
    char buf[20];
    mlog(label);
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 16; i++) buf[2 + i] = hx[(v >> (60 - 4 * i)) & 0xF];
    buf[18] = '\n';
    buf[19] = 0;
    mlog(buf);
}

#define LOG(s)          mlog("[md64api] " s "\n")
#define LOGHEX(s, v)    mlog_hex("[md64api] " s " = ", (uint64_t)(v))
#define LOGPTR(s, p)    mlog_hex("[md64api] " s " = ", (uint64_t)(uintptr_t)(p))

static inline uint64_t get_rsp(void) {
    uint64_t r;
    __asm__ volatile("mov %%rsp, %0" : "=r"(r));
    return r;
}

/* cpuid wrapper: all four registers declared as outputs, leaf+subleaf as inputs */
static inline void do_cpuid(uint32_t leaf, uint32_t sub,
                            uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    uint32_t ra, rb, rc, rd;
    __asm__ volatile("cpuid"
                     : "=a"(ra), "=b"(rb), "=c"(rc), "=d"(rd)
                     : "a"(leaf), "c"(sub));
    if (a) *a = ra;
    if (b) *b = rb;
    if (c) *c = rc;
    if (d) *d = rd;
}

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

static ENTRY void *devfs_open_stub(void *ctx, int flags) {
    (void)flags;
    return ctx; /* return ctx as the handle */
}

static ENTRY int devfs_close_stub(void *ctx) {
    (void)ctx;
    return 0;
}

static ENTRY ssize_t devfs_write_stub(void *ctx, const void *buf, size_t count) {
    (void)ctx; (void)buf; (void)count;
    return -1; /* read-only nodes */
}

/* ---- urandom: ChaCha20 DRBG with fast key erasure ---- */

#define URANDOM_RESEED_BYTES (1ULL << 20)

static uint32_t g_rng_key[8];
static uint64_t g_rng_ctr;
static uint64_t g_rng_since_seed;
static int      g_rng_seeded;
static int      g_rng_weak_logged;

static uint64_t rng_irq_save(void) {
    uint64_t f;
    __asm__ volatile("pushfq; cli; popq %0" : "=r"(f) :: "memory");
    return f;
}

static void rng_irq_restore(uint64_t f) {
    __asm__ volatile("pushq %0; popfq" :: "r"(f) : "memory", "cc");
}

static void rng_wipe(void *p, size_t n) {
    volatile unsigned char *v = (volatile unsigned char *)p;
    while (n--) *v++ = 0;
}

static uint64_t rng_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static uint64_t rng_mix64(uint64_t z) {
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/* Last resort when no hardware RNG exists: not suitable as a sole entropy
 * source for long-lived keys. */
static uint64_t rng_jitter64(void) {
    uint64_t acc = rng_rdtsc();
    for (int i = 0; i < 512; i++) {
        uint64_t t = rng_rdtsc();
        for (volatile unsigned k = 0; k < (unsigned)(acc & 0x3F); k++) { }
        acc = rng_mix64(acc ^ (rng_rdtsc() - t) ^ (acc << 7));
    }
    return acc;
}

static int rng_hw_caps(void) {
    uint32_t a, b, c, d;
    int caps = 0;

    do_cpuid(0, 0, &a, &b, &c, &d);
    uint32_t max_leaf = a;
    do_cpuid(1, 0, &a, &b, &c, &d);
    if (c & (1u << 30)) caps |= 1;                 /* RDRAND */
    if (max_leaf >= 7) {
        do_cpuid(7, 0, &a, &b, &c, &d);
        if (b & (1u << 18)) caps |= 2;             /* RDSEED */
    }
    return caps;
}

static int rng_hw64(int caps, uint64_t *out) {
    unsigned char ok;
    uint64_t v;

    if (caps & 2) {
        for (int i = 0; i < 64; i++) {
            __asm__ volatile("rdseed %0; setc %1" : "=r"(v), "=qm"(ok) :: "cc");
            if (ok) { *out = v; return 0; }
            __asm__ volatile("pause");
        }
    }
    if (caps & 1) {
        for (int i = 0; i < 10; i++) {
            __asm__ volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok) :: "cc");
            if (ok) { *out = v; return 0; }
        }
    }
    return -1;
}

#define ROTL32(v, n) (((v) << (n)) | ((v) >> (32 - (n))))
#define CHACHA_QR(a, b, c, d) do {                       \
    a += b; d ^= a; d = ROTL32(d, 16);                   \
    c += d; b ^= c; b = ROTL32(b, 12);                   \
    a += b; d ^= a; d = ROTL32(d, 8);                    \
    c += d; b ^= c; b = ROTL32(b, 7);                    \
} while (0)

static ENTRY void chacha20_block(const uint32_t key[8], uint64_t ctr,
                                 uint32_t out[16]) {
    uint32_t s[16] = {
        0x61707865u, 0x3320646Eu, 0x79622D32u, 0x6B206574u,
        key[0], key[1], key[2], key[3], key[4], key[5], key[6], key[7],
        (uint32_t)ctr, (uint32_t)(ctr >> 32), 0, 0
    };
    uint32_t x[16];

    for (int i = 0; i < 16; i++) x[i] = s[i];
    for (int i = 0; i < 10; i++) {
        CHACHA_QR(x[0], x[4], x[8],  x[12]);
        CHACHA_QR(x[1], x[5], x[9],  x[13]);
        CHACHA_QR(x[2], x[6], x[10], x[14]);
        CHACHA_QR(x[3], x[7], x[11], x[15]);
        CHACHA_QR(x[0], x[5], x[10], x[15]);
        CHACHA_QR(x[1], x[6], x[11], x[12]);
        CHACHA_QR(x[2], x[7], x[8],  x[13]);
        CHACHA_QR(x[3], x[4], x[9],  x[14]);
    }
    for (int i = 0; i < 16; i++) out[i] = x[i] + s[i];
    rng_wipe(x, sizeof(x));
    rng_wipe(s, sizeof(s));
}

static ENTRY void rng_reseed(void) {
    uint32_t blk[16];
    uint64_t ent[4];
    int caps = rng_hw_caps();
    int weak = 0;

    for (int i = 0; i < 4; i++) {
        if (rng_hw64(caps, &ent[i]) != 0) {
            ent[i] = rng_jitter64();
            weak = 1;
        }
        ent[i] ^= rng_rdtsc();
    }
    if (weak && !g_rng_weak_logged) {
        LOG("urandom: no usable RDSEED/RDRAND, using TSC jitter");
        g_rng_weak_logged = 1;
    }

    for (int i = 0; i < 4; i++) {
        g_rng_key[2 * i]     ^= (uint32_t)ent[i];
        g_rng_key[2 * i + 1] ^= (uint32_t)(ent[i] >> 32);
    }

    chacha20_block(g_rng_key, g_rng_ctr++, blk);
    for (int i = 0; i < 8; i++) g_rng_key[i] = blk[i];

    rng_wipe(blk, sizeof(blk));
    rng_wipe(ent, sizeof(ent));
    g_rng_since_seed = 0;
    g_rng_seeded = 1;
}

static ENTRY ssize_t rd_urandom(void *ctx, void *buf, size_t n) {
    (void)ctx;
    unsigned char *dst = (unsigned char *)buf;
    size_t left = n;

    if (!buf) return -1;

    while (left) {
        uint32_t blk[16];
        size_t take = left < 32 ? left : 32;
        uint64_t f = rng_irq_save();

        if (!g_rng_seeded || g_rng_since_seed >= URANDOM_RESEED_BYTES)
            rng_reseed();

        chacha20_block(g_rng_key, g_rng_ctr++, blk);
        for (int i = 0; i < 8; i++) g_rng_key[i] = blk[i];
        memcpy(dst, &blk[8], take);
        g_rng_since_seed += take;
        rng_irq_restore(f);

        rng_wipe(blk, sizeof(blk));
        dst += take;
        left -= take;
    }
    return (ssize_t)n;
}

/* ---- Read handlers ---- */

static ENTRY ssize_t rd_cmdline(void *ctx, void *buf, size_t n) {
    (void)ctx;
    struct multiboot_tag_string *t = (struct multiboot_tag_string *)find_tag(MULTIBOOT_TAG_TYPE_CMDLINE);
    return str_read(t ? t->string : "", buf, n);
}

static ENTRY ssize_t rd_bootloader(void *ctx, void *buf, size_t n) {
    (void)ctx;
    struct multiboot_tag_string *t = (struct multiboot_tag_string *)find_tag(MULTIBOOT_TAG_TYPE_BOOT_LOADER_NAME);
    return str_read(t ? t->string : "", buf, n);
}

static ENTRY ssize_t rd_ram_lower(void *ctx, void *buf, size_t n) {
    (void)ctx;
    struct multiboot_tag_basic_meminfo *t = (struct multiboot_tag_basic_meminfo *)find_tag(MULTIBOOT_TAG_TYPE_BASIC_MEMINFO);
    return u32_read(t ? t->mem_lower : 0, buf, n);
}

static ENTRY ssize_t rd_ram_upper(void *ctx, void *buf, size_t n) {
    (void)ctx;
    struct multiboot_tag_basic_meminfo *t = (struct multiboot_tag_basic_meminfo *)find_tag(MULTIBOOT_TAG_TYPE_BASIC_MEMINFO);
    return u32_read(t ? t->mem_upper : 0, buf, n);
}

static ENTRY ssize_t rd_sysinfo(void *ctx, void *buf, size_t count) {
    (void)ctx;
    LOG("rd_sysinfo: enter");
    LOGPTR("rd_sysinfo buf", buf);
    LOGHEX("rd_sysinfo count", count);
    LOGHEX("rd_sysinfo sizeof(g_sysinfo)", sizeof(g_sysinfo));
    LOGHEX("rd_sysinfo rsp", get_rsp());

    collect_sysinfo();

    size_t n = count < sizeof(g_sysinfo) ? count : sizeof(g_sysinfo);
    LOGHEX("rd_sysinfo: copying n", n);
    memcpy(buf, &g_sysinfo, n);
    LOG("rd_sysinfo: copy done, returning");
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

/* Compare a 12-byte hypervisor vendor string with a (possibly shorter) name,
 * treating the name as NUL-padded to 12 bytes. Never reads past name's NUL. */
static int vendor_is(const char *hs, const char *name) {
    int ended = 0;
    for (int i = 0; i < 12; i++) {
        char want = 0;
        if (!ended) {
            want = name[i];
            if (!want) ended = 1;
        }
        if (hs[i] != want) return 0;
    }
    return 1;
}

static ENTRY void collect_sysinfo(void) {
    md64api_sysinfo_data_u *o = &g_sysinfo;

    LOG("collect_sysinfo: ENTER");
    LOGPTR("collect_sysinfo fn addr (RIP minus this = offset)", collect_sysinfo);
    LOGPTR("rd_sysinfo fn addr", rd_sysinfo);
    LOGHEX("rsp at entry", get_rsp());
    LOGHEX("rsp & 0xF at entry (0 = 16-byte aligned)", get_rsp() & 0xF);
    LOGPTR("g_api", g_api);
    LOGPTR("g_sysinfo addr", &g_sysinfo);
    LOGHEX("sizeof(g_sysinfo)", sizeof(g_sysinfo));
    LOGPTR("api->phys_total_frames",      g_api->phys_total_frames);
    LOGPTR("api->phys_count_free_frames", g_api->phys_count_free_frames);
    LOGPTR("api->get_smbios_field",       g_api->get_smbios_field);
    LOGPTR("api->get_gpu_driver_name",    g_api->get_gpu_driver_name);
    LOGPTR("api->block_get_info",         g_api->block_get_info);
    LOGPTR("api->get_system_ticks",       g_api->get_system_ticks);
    LOGPTR("api->ticks_to_ms",            g_api->ticks_to_ms);

    /* ---------------- Memory ---------------- */
    LOG("stage 1: memory");
    if (g_api->phys_total_frames && g_api->phys_count_free_frames) {
        LOG("stage 1: calling phys_total_frames");
        uint64_t tot = g_api->phys_total_frames();
        LOGHEX("stage 1: total frames", tot);
        LOG("stage 1: calling phys_count_free_frames");
        uint64_t fr  = g_api->phys_count_free_frames();
        LOGHEX("stage 1: free frames", fr);
        o->sys_total_ram     = (tot * 4096ULL) / (1024ULL * 1024ULL);
        o->sys_available_ram = (fr  * 4096ULL) / (1024ULL * 1024ULL);
    } else {
        LOG("stage 1: using multiboot meminfo fallback");
        struct multiboot_tag_basic_meminfo *mt =
            (struct multiboot_tag_basic_meminfo *)find_tag(MULTIBOOT_TAG_TYPE_BASIC_MEMINFO);
        LOGPTR("stage 1: meminfo tag", mt);
        if (mt) o->sys_total_ram = o->sys_available_ram =
            (uint64_t)(mt->mem_upper + mt->mem_lower) / 1024ULL;
    }
    LOG("stage 1: memory done");

    /* ---------------- Static strings ---------------- */
    LOG("stage 2: static strings");
    safe_copy(o->SystemVersion, sizeof(o->SystemVersion), "0.6.8");
    safe_copy(o->KernelVersion, sizeof(o->KernelVersion), "0.6.8");
    safe_copy(o->KernelVendor,  sizeof(o->KernelVendor),  "NTSoftware");
    safe_copy(o->os_name,       sizeof(o->os_name),       "ModuOS");
    safe_copy(o->os_arch,       sizeof(o->os_arch),       "AMD64");
    safe_copy(o->username,      sizeof(o->username),      "{dont_use_me}");
    safe_copy(o->kconsole,      sizeof(o->kconsole),      "TTYMAN.CTL");
    LOG("stage 2: static strings done");

    /* ---------------- SMBIOS ---------------- */
    LOG("stage 3: smbios");
    if (g_api->get_smbios_field) {
        LOG("stage 3: get_smbios_field(0)");
        const char *mfr  = g_api->get_smbios_field(0);
        LOGPTR("stage 3: mfr ptr", mfr);
        LOG("stage 3: get_smbios_field(1)");
        const char *prod = g_api->get_smbios_field(1);
        LOGPTR("stage 3: prod ptr", prod);
        LOG("stage 3: get_smbios_field(2)");
        const char *bv   = g_api->get_smbios_field(2);
        LOGPTR("stage 3: bios vendor ptr", bv);
        LOG("stage 3: get_smbios_field(3)");
        const char *bver = g_api->get_smbios_field(3);
        LOGPTR("stage 3: bios version ptr", bver);

        if (mfr)  { LOG("stage 3: copy mfr");  safe_copy(o->motherboard_model, sizeof(o->motherboard_model), mfr); }
        if (prod) { LOG("stage 3: copy prod"); safe_copy(o->pcname,            sizeof(o->pcname),            prod); }
        if (bv)   { LOG("stage 3: copy bv");   safe_copy(o->bios_vendor,       sizeof(o->bios_vendor),       bv); }
        if (bver) { LOG("stage 3: copy bver"); safe_copy(o->bios_version,      sizeof(o->bios_version),      bver); }
    } else {
        LOG("stage 3: get_smbios_field is NULL, skipping");
    }
    LOG("stage 3: smbios done");

    /* ---------------- GPU ---------------- */
    LOG("stage 4: gpu");
    if (g_api->get_gpu_driver_name) {
        LOG("stage 4: calling get_gpu_driver_name");
        const char *gpu = g_api->get_gpu_driver_name();
        LOGPTR("stage 4: gpu name ptr", gpu);
        if (gpu) {
            safe_copy(o->gpu_name,   sizeof(o->gpu_name),   gpu);
            safe_copy(o->gpu_driver, sizeof(o->gpu_driver), gpu);
            LOG("stage 4: gpu strings copied");
        }
    } else {
        LOG("stage 4: get_gpu_driver_name is NULL, skipping");
    }
    o->gpu_vram_mb = 64;
    LOG("stage 4: gpu done");

    /* ---------------- Primary disk model ---------------- */
    LOG("stage 5: primary disk");
    if (g_api->block_get_info) {
        for (blockdev_handle_t h = 1; h <= 4; h++) {
            LOGHEX("stage 5: checking handle", (uint64_t)h);

            blockdev_info_t info;
            memset(&info, 0, sizeof(info));
            LOGHEX("stage 5: sizeof(blockdev_info_t)", sizeof(info));

            int rc = g_api->block_get_info(h, &info);
            LOGHEX("stage 5: block_get_info rc", (uint64_t)(int64_t)rc);

            if (rc != 0) {
                LOG("stage 5: failed, skipping");
                continue;
            }
            if (info.flags & BLOCKDEV_F_REMOVABLE) {
                LOG("stage 5: removable, skipping");
                continue;
            }
            if (info.model[0]) {
                mlog("[md64api] stage 5: model found: ");
                mlog(info.model);
                mlog("\n");
                safe_copy(o->primary_disk_model, sizeof(o->primary_disk_model), info.model);
                LOG("stage 5: model copied, breaking");
                break;
            }
            LOG("stage 5: model empty, continuing");
        }
    } else {
        LOG("stage 5: block_get_info is NULL, skipping");
    }
    LOG("stage 5: primary disk done");

    /* ---------------- Uptime ---------------- */
    LOG("stage 6: uptime");
    if (g_api->get_system_ticks && g_api->ticks_to_ms) {
        LOG("stage 6: calling get_system_ticks");
        uint64_t ticks = g_api->get_system_ticks();
        LOGHEX("stage 6: ticks", ticks);
        LOG("stage 6: calling ticks_to_ms");
        uint64_t ms = g_api->ticks_to_ms(ticks);
        LOGHEX("stage 6: ms", ms);
        o->uptime_seconds = ms / 1000ULL;
    } else {
        LOG("stage 6: tick functions missing, skipping");
    }
    LOG("stage 6: uptime done");

    /* ---------------- CPUID leaf 0: vendor ---------------- */
    LOG("stage 7: cpuid leaf 0");
    {
        uint32_t a, b, c, d;
        char v[13];
        do_cpuid(0, 0, &a, &b, &c, &d);
        LOGHEX("stage 7: max leaf", a);
        memcpy(v + 0, &b, 4);
        memcpy(v + 4, &d, 4);
        memcpy(v + 8, &c, 4);
        v[12] = 0;
        mlog("[md64api] stage 7: vendor: ");
        mlog(v);
        mlog("\n");
        safe_copy(o->cpu,              sizeof(o->cpu),              v);
        safe_copy(o->cpu_manufacturer, sizeof(o->cpu_manufacturer), v);
    }
    LOG("stage 7: done");

    /* ---------------- CPUID leaf 1 ---------------- */
    LOG("stage 8: cpuid leaf 1");
    uint32_t l1_ecx = 0, l1_edx = 0;
    {
        uint32_t a, b;
        do_cpuid(1, 0, &a, &b, &l1_ecx, &l1_edx);
        LOGHEX("stage 8: eax", a);
        LOGHEX("stage 8: ebx", b);
        LOGHEX("stage 8: ecx", l1_ecx);
        LOGHEX("stage 8: edx", l1_edx);
        o->cpu_hyperthreading_enabled = (l1_edx & (1u << 28)) ? 1 : 0;
        o->cpu_cores   = (b >> 16) & 0xFF;
        if (!o->cpu_cores) o->cpu_cores = 1;
        o->cpu_threads  = o->cpu_cores;
        o->cpu_base_mhz = 2400;
        o->cpu_max_mhz  = 3600;
        o->cpu_cache_l1_kb = 32;
        o->cpu_cache_l2_kb = 256;
        o->cpu_cache_l3_kb = 8192;
    }
    LOG("stage 8: done");

    /* ---------------- CPUID brand string ---------------- */
    LOG("stage 9: cpuid brand");
    {
        uint32_t mx = 0;
        do_cpuid(0x80000000u, 0, &mx, NULL, NULL, NULL);
        LOGHEX("stage 9: max extended leaf", mx);
        if (mx >= 0x80000004u) {
            uint32_t br[12];
            memset(br, 0, sizeof(br));
            do_cpuid(0x80000002u, 0, &br[0], &br[1], &br[2],  &br[3]);
            do_cpuid(0x80000003u, 0, &br[4], &br[5], &br[6],  &br[7]);
            do_cpuid(0x80000004u, 0, &br[8], &br[9], &br[10], &br[11]);
            char *bs = (char *)br;
            bs[47] = 0;                       /* guarantee termination */
            size_t skip = 0;
            while (skip < 47 && bs[skip] == ' ') skip++;
            mlog("[md64api] stage 9: brand: ");
            mlog(bs + skip);
            mlog("\n");
            safe_copy(o->cpu_model, sizeof(o->cpu_model), bs + skip);
        } else {
            LOG("stage 9: no brand string, using vendor");
            safe_copy(o->cpu_model, sizeof(o->cpu_model), o->cpu);
        }
    }
    LOG("stage 9: done");

    /* ---------------- CPUID flags ---------------- */
    LOG("stage 10: cpu flags");
    {
        char fl[128];
        size_t flen = 0;
        memset(fl, 0, sizeof(fl));
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
        mlog("[md64api] stage 10: flags: ");
        mlog(fl);
        mlog("\n");
        safe_copy(o->cpu_flags, sizeof(o->cpu_flags), fl);
    }
    LOG("stage 10: done");

    /* ---------------- VM detection ---------------- */
    LOG("stage 11: vm detection");
    if (l1_ecx & (1u << 31)) {
        o->is_virtual_machine = 1;
        uint32_t ha, hb, hc, hd;
        char hs[13];
        do_cpuid(0x40000000u, 0, &ha, &hb, &hc, &hd);
        LOGHEX("stage 11: hv max leaf", ha);
        memcpy(hs + 0, &hb, 4);
        memcpy(hs + 4, &hc, 4);
        memcpy(hs + 8, &hd, 4);
        hs[12] = 0;
        mlog("[md64api] stage 11: hypervisor: ");
        mlog(hs);
        mlog("\n");
        if      (vendor_is(hs, "KVMKVMKVM"))    safe_copy(o->virtualization_vendor, sizeof(o->virtualization_vendor), "KVM/QEMU");
        else if (vendor_is(hs, "VMwareVMware")) safe_copy(o->virtualization_vendor, sizeof(o->virtualization_vendor), "VMware");
        else if (vendor_is(hs, "VBoxVBoxVBox")) safe_copy(o->virtualization_vendor, sizeof(o->virtualization_vendor), "VirtualBox");
        else if (vendor_is(hs, "Microsoft Hv")) safe_copy(o->virtualization_vendor, sizeof(o->virtualization_vendor), "Hyper-V");
        else safe_copy(o->virtualization_vendor, sizeof(o->virtualization_vendor), hs);
    } else {
        LOG("stage 11: hypervisor bit not set");
    }
    LOG("stage 11: done");

    LOG("collect_sysinfo: EXIT OK");
}

/* ---- Service ---- */

typedef struct { int (*get_sysinfo)(void *out, size_t out_size); } md64api_service_t;

static ENTRY int svc_get_sysinfo(void *out, size_t out_size) {
    if (!out) return -1;
    LOG("svc_get_sysinfo: enter");
    collect_sysinfo();
    size_t n = out_size < sizeof(g_sysinfo) ? out_size : sizeof(g_sysinfo);
    memcpy(out, &g_sysinfo, n);
    LOG("svc_get_sysinfo: done");
    return 0;
}

static const md64api_service_t g_svc = { .get_sysinfo = svc_get_sysinfo };

/* ---- DevFS ops structs ---- */

UNUSED static const devfs_owner_t g_owner = {
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

static devfs_device_ops_t g_ops_urandom = {
    .name        = "urandom",
    .open        = devfs_open_stub,
    .read        = rd_urandom,
    .write       = devfs_write_stub,
    .close       = devfs_close_stub,
    .can_replace = NULL,
};

/* ---- Module init ---- */

ENTRY int sqrm_init(const sqrm_kernel_api_t *api) {
    if (!api || !api->multiboot2_header || !api->devfs_register_path) return -1;

    g_api = api;
    g_mb2 = api->multiboot2_header;

    LOG("sqrm_init: enter (debug build)");
    LOGPTR("sqrm_init fn addr", sqrm_init);
    LOGPTR("collect_sysinfo fn addr", collect_sysinfo);
    LOGPTR("rd_sysinfo fn addr", rd_sysinfo);
    LOGHEX("sizeof(md64api_sysinfo_data_u)", sizeof(g_sysinfo));
    LOGHEX("rsp & 0xF in init", get_rsp() & 0xF);

    rng_reseed();
    LOG("sqrm_init: rng seeded");

    api->devfs_register_path("md64api/cmdline",    &g_ops_cmdline,    NULL);
    api->devfs_register_path("md64api/bootloader", &g_ops_bootloader, NULL);
    api->devfs_register_path("md64api/ram/lower",  &g_ops_ram_lower,  NULL);
    api->devfs_register_path("md64api/ram/upper",  &g_ops_ram_upper,  NULL);
    api->devfs_register_path("md64api/sysinfo",    &g_ops_sysinfo,    NULL);
    api->devfs_register_path("md64api/urandom",    &g_ops_urandom,    NULL);
    LOG("sqrm_init: devfs nodes registered");

    if (api->sqrm_service_register)
        api->sqrm_service_register("md64api", &g_svc, sizeof(g_svc));

    api->com_write_string(0x3F8, "[md64api] $/dev/md64api/ registered\n");
    return 0;
}