#include "sqrm_sdk.h"
#include "moduos/kernel/COM/com.h"

/*
 * Intel HDA (High Definition Audio / Azalia) SQRM audio module.
 *
 * Mirrors the structure of the AC97 driver so the ModuOS kernel loader
 * accepts it identically:
 *   - same #include paths
 *   - same const sqrm_module_desc_t definition style (not the macro)
 *   - raw PCI CF8/CFC I/O scan (class 0x04 / subclass 0x03)
 *   - MMIO via api->ioremap (BAR0 is a 64-bit memory BAR for HDA)
 *   - controller reset, CORB/RIRB, codec enumeration, BDL output stream
 *   - same IRQ install pattern
 *   - audio_pcm_ops_t registered as "hda0"
 */

SQRM_DEFINE_MODULE(SQRM_TYPE_AUDIO, "intel-hda");

/* ------------------------------------------------------------------ */
/* Local helpers (no libc)                                             */
/* ------------------------------------------------------------------ */

static void *memcpy_local(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}
static void *memset_local(void *dst, int v, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (size_t i = 0; i < n; i++) d[i] = (uint8_t)v;
    return dst;
}

/* ------------------------------------------------------------------ */
/* PCI helpers — identical pattern to AC97                             */
/* ------------------------------------------------------------------ */

static uint32_t pci_read32(const sqrm_kernel_api_t *api,
                            uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    uint32_t addr = 0x80000000u | ((uint32_t)bus<<16) |
                    ((uint32_t)dev<<11) | ((uint32_t)fn<<8) | (off & 0xFCu);
    api->outl(0xCF8, addr);
    return api->inl(0xCFC);
}
static void pci_write32(const sqrm_kernel_api_t *api,
                         uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t val) {
    uint32_t addr = 0x80000000u | ((uint32_t)bus<<16) |
                    ((uint32_t)dev<<11) | ((uint32_t)fn<<8) | (off & 0xFCu);
    api->outl(0xCF8, addr);
    api->outl(0xCFC, val);
}
static uint16_t pci_read16(const sqrm_kernel_api_t *api,
                            uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    return (uint16_t)((pci_read32(api,bus,dev,fn,off) >> ((off&2u)*8u)) & 0xFFFFu);
}
static uint8_t pci_read8(const sqrm_kernel_api_t *api,
                          uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    return (uint8_t)((pci_read32(api,bus,dev,fn,off) >> ((off&3u)*8u)) & 0xFFu);
}

/* Find HDA controller: PCI class 0x04, subclass 0x03 */
static int hda_find_pci(const sqrm_kernel_api_t *api,
                         uint8_t *ob, uint8_t *od, uint8_t *of_) {
    for (uint8_t bus = 0; bus < 0xFF; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            for (uint8_t fn = 0; fn < 8; fn++) {
                uint16_t vendor = pci_read16(api, bus, dev, fn, 0x00);
                if (vendor == 0xFFFF) { if (fn == 0) break; continue; }
                uint8_t class_ = pci_read8(api, bus, dev, fn, 0x0B);
                uint8_t subcl  = pci_read8(api, bus, dev, fn, 0x0A);
                if (class_ == 0x04 && subcl == 0x03) {
                    *ob = bus; *od = dev; *of_ = fn; return 0;
                }
            }
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* HDA MMIO register map                                               */
/* ------------------------------------------------------------------ */

#define HDAC_GCAP       0x00u
#define HDAC_GCTL       0x08u
#define HDAC_STATESTS   0x0Eu
#define HDAC_CORBLBASE  0x40u
#define HDAC_CORBUBASE  0x44u
#define HDAC_CORBWP     0x48u
#define HDAC_CORBRP     0x4Au
#define HDAC_CORBCTL    0x4Cu
#define HDAC_CORBSIZE   0x4Eu
#define HDAC_RIRBLBASE  0x50u
#define HDAC_RIRBUBASE  0x54u
#define HDAC_RIRBWP     0x58u
#define HDAC_RINTCNT    0x5Au
#define HDAC_RIRBCTL    0x5Cu
#define HDAC_RIRBSIZE   0x5Eu

#define GCTL_CRST       (1u << 0)
#define GCTL_UNSOL      (1u << 8)
#define CORBCTL_RUN     (1u << 1)
#define RIRBCTL_RUN     (1u << 1)
#define RIRBCTL_RINTCTL (1u << 0)
#define RINGSIZE_256    0x02u

/* Stream descriptor registers (offset from sd_base) */
#define HDAC_SDBASE     0x80u
#define HDAC_SD_SZ      0x20u
#define SD_CTL          0x00u
#define SD_STS          0x03u
#define SD_CBL          0x08u
#define SD_LVI          0x0Cu
#define SD_FMT          0x12u
#define SD_BDLPL        0x18u
#define SD_BDLPU        0x1Cu

#define SDC_SRST        (1u << 0)
#define SDC_RUN         (1u << 1)
#define SDC_IOCE        (1u << 2)
#define SDC_DIR         (1u << 19)
#define SDC_STRM_SHIFT  20

/* 48 kHz, stereo, 16-bit PCM format word */
#define HDA_FMT_48K_S16_2CH  ((0u<<14)|(0u<<11)|(0u<<8)|(1u<<4)|1u)

/* ------------------------------------------------------------------ */
/* BDL                                                                  */
/* ------------------------------------------------------------------ */

#define HDA_BDL_ENTRIES  32u
#define HDA_SEG_BYTES    4096u
#define HDA_BUF_BYTES    (HDA_BDL_ENTRIES * HDA_SEG_BYTES)

typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint32_t length;
    uint32_t ioc;
} hda_bdl_entry_t;

/* ------------------------------------------------------------------ */
/* Verb macros                                                          */
/* ------------------------------------------------------------------ */

/* 12-bit verb + 8-bit param */
#define HDA_VERB(cad,nid,verb,param) \
    (((uint32_t)(cad)<<28)|((uint32_t)(nid)<<20)|((uint32_t)(verb)<<8)|((uint32_t)(param)&0xFFu))

/* 4-bit verb + 16-bit payload (SET_FORMAT=0xA, SET_AMP=0x3) */
#define HDA_VERB4(cad,nid,v4,pay16) \
    (((uint32_t)(cad)<<28)|((uint32_t)(nid)<<20)|(((uint32_t)(v4)&0xFu)<<16)|((uint32_t)(pay16)&0xFFFFu))

#define V_GET_PARAM  0xF00u
#define V_SET_POWER  0x705u
#define V_SET_STREAM 0x706u
#define V_SET_PIN    0x707u
#define V_SET_EAPD   0x70Cu
#define V4_SET_FMT   0xAu
#define V4_SET_AMP   0x3u
#define P_NODE_COUNT 0x04u
#define P_FUNC_TYPE  0x05u
#define P_WIDGET_CAP 0x09u
#define P_PIN_CAPS   0x0Cu
#define WT_AUDIO_OUT 0x0u
#define WT_PIN       0x4u

/* ------------------------------------------------------------------ */
/* Driver state                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    const sqrm_kernel_api_t *api;
    uint8_t  *base;
    uint8_t   irq_line;

    sqrm_dma_buffer_t corb_dma;   /* FIX: was dma_buffer_t (kernel-only type) */
    uint32_t         *corb;
    uint16_t          corb_wp;

    sqrm_dma_buffer_t rirb_dma;   /* FIX: was dma_buffer_t */
    uint64_t         *rirb;
    uint16_t          rirb_rp;

    uint32_t          sd_base;
    sqrm_dma_buffer_t bdl_dma;    /* FIX: was dma_buffer_t */
    hda_bdl_entry_t  *bdl;
    sqrm_dma_buffer_t buf_dma;    /* FIX: was dma_buffer_t */
    uint8_t          *buf;

    uint32_t next_fill;
    uint32_t queued;
    int      running;

    uint8_t codec_addr;
    uint8_t afg_nid;
    uint8_t out_nid;
    uint8_t pin_nid;
} hda_state_t;

static hda_state_t  g;
static hda_state_t *g_state_for_handler = NULL;
static uint8_t      g_irq_line_for_handler = 0;

/* ------------------------------------------------------------------ */
/* MMIO accessors                                                       */
/* ------------------------------------------------------------------ */

static inline uint8_t  hr8 (hda_state_t *h,uint32_t o){return *((volatile uint8_t *)(h->base+o));}
static inline uint16_t hr16(hda_state_t *h,uint32_t o){return *((volatile uint16_t*)(h->base+o));}
static inline uint32_t hr32(hda_state_t *h,uint32_t o){return *((volatile uint32_t*)(h->base+o));}
static inline void hw8 (hda_state_t *h,uint32_t o,uint8_t  v){*((volatile uint8_t *)(h->base+o))=v;}
static inline void hw16(hda_state_t *h,uint32_t o,uint16_t v){*((volatile uint16_t*)(h->base+o))=v;}
static inline void hw32(hda_state_t *h,uint32_t o,uint32_t v){*((volatile uint32_t*)(h->base+o))=v;}

static inline uint32_t sd32 (hda_state_t *h,uint32_t r)          {return hr32(h,h->sd_base+r);}
static inline void     sdw32(hda_state_t *h,uint32_t r,uint32_t v){hw32(h,h->sd_base+r,v);}
static inline void     sdw16(hda_state_t *h,uint32_t r,uint16_t v){hw16(h,h->sd_base+r,v);}

static inline void hda_sleep(hda_state_t *h, uint64_t ms) {
    if (h->api->sleep_ms) h->api->sleep_ms(ms);
}

/* ------------------------------------------------------------------ */
/* CORB / RIRB                                                          */
/* ------------------------------------------------------------------ */

static int hda_corb_put(hda_state_t *h, uint32_t verb) {
    uint16_t wp = (uint16_t)((h->corb_wp + 1u) % 256u);
    h->corb[wp] = verb;
    h->corb_wp  = wp;
    hw16(h, HDAC_CORBWP, wp);
    return 0;
}
static int hda_rirb_get(hda_state_t *h, uint32_t *resp) {
    for (int i = 0; i < 3000; i++) {
        uint16_t hw_wp = hr16(h, HDAC_RIRBWP) & 0xFFu;
        if (hw_wp != h->rirb_rp) {
            h->rirb_rp = (uint16_t)((h->rirb_rp + 1u) % 256u);
            if (resp) *resp = (uint32_t)(h->rirb[h->rirb_rp] & 0xFFFFFFFFu);
            return 0;
        }
        hda_sleep(h, 1);
    }
    return -1;
}
static int hda_verb(hda_state_t *h, uint8_t cad, uint8_t nid,
                    uint32_t v12, uint8_t param, uint32_t *resp) {
    if (hda_corb_put(h, HDA_VERB(cad,nid,v12,param)) < 0) return -1;
    return hda_rirb_get(h, resp);
}
static int hda_verb4(hda_state_t *h, uint8_t cad, uint8_t nid,
                     uint32_t v4, uint16_t pay, uint32_t *resp) {
    if (hda_corb_put(h, HDA_VERB4(cad,nid,v4,pay)) < 0) return -1;
    return hda_rirb_get(h, resp);
}

/* ------------------------------------------------------------------ */
/* Controller reset                                                     */
/* ------------------------------------------------------------------ */

static int hda_reset_ctrl(hda_state_t *h) {
    hw32(h, HDAC_GCTL, 0);          hda_sleep(h, 10);
    hw32(h, HDAC_GCTL, GCTL_CRST);
    for (int i = 0; i < 200; i++) {
        if (hr32(h, HDAC_GCTL) & GCTL_CRST) break;
        hda_sleep(h, 1);
    }
    if (!(hr32(h, HDAC_GCTL) & GCTL_CRST)) return -1;
    hda_sleep(h, 5);
    hw32(h, HDAC_GCTL, GCTL_CRST | GCTL_UNSOL);
    return 0;
}

static void hda_init_corb(hda_state_t *h) {
    hw8(h, HDAC_CORBCTL, 0);        hda_sleep(h, 2);
    hw32(h, HDAC_CORBLBASE, (uint32_t)(h->corb_dma.phys & 0xFFFFFFFFu));
    hw32(h, HDAC_CORBUBASE, (uint32_t)(h->corb_dma.phys >> 32));
    hw8(h, HDAC_CORBSIZE, RINGSIZE_256);
    h->corb_wp = 0;
    hw16(h, HDAC_CORBWP, 0);
    hw16(h, HDAC_CORBRP, 0x8000u);
    for (int i = 0; i < 100; i++) { if (hr16(h, HDAC_CORBRP) & 0x8000u) break; hda_sleep(h,1); }
    hw16(h, HDAC_CORBRP, 0);
    hw8(h, HDAC_CORBCTL, CORBCTL_RUN);
}

static void hda_init_rirb(hda_state_t *h) {
    hw8(h, HDAC_RIRBCTL, 0);        hda_sleep(h, 2);
    hw32(h, HDAC_RIRBLBASE, (uint32_t)(h->rirb_dma.phys & 0xFFFFFFFFu));
    hw32(h, HDAC_RIRBUBASE, (uint32_t)(h->rirb_dma.phys >> 32));
    hw8(h, HDAC_RIRBSIZE, RINGSIZE_256);
    hw16(h, HDAC_RINTCNT, 1);
    hw16(h, HDAC_RIRBWP, 0x8000u);
    h->rirb_rp = 0;
    hw8(h, HDAC_RIRBCTL, RIRBCTL_RUN | RIRBCTL_RINTCTL);
}

/* ------------------------------------------------------------------ */
/* Codec probe                                                          */
/* ------------------------------------------------------------------ */

static int hda_find_codec(hda_state_t *h) {
    uint16_t st = hr16(h, HDAC_STATESTS);
    for (int i = 0; i < 15; i++)
        if (st & (1u << i)) { h->codec_addr = (uint8_t)i; return 0; }
    return -1;
}

static int hda_probe_widgets(hda_state_t *h) {
    uint32_t resp = 0;
    hda_verb(h, h->codec_addr, 0, V_GET_PARAM, P_NODE_COUNT, &resp);
    uint8_t rs = (uint8_t)((resp >> 16) & 0xFFu);
    uint8_t rc = (uint8_t)(resp & 0xFFu);
    for (uint8_t n = rs; n < rs + rc; n++) {
        hda_verb(h, h->codec_addr, n, V_GET_PARAM, P_FUNC_TYPE, &resp);
        if ((resp & 0xFFu) == 0x01u) { h->afg_nid = n; break; }
    }
    if (!h->afg_nid) return -1;

    hda_verb(h, h->codec_addr, h->afg_nid, V_SET_POWER, 0x00, NULL);
    hda_sleep(h, 5);

    hda_verb(h, h->codec_addr, h->afg_nid, V_GET_PARAM, P_NODE_COUNT, &resp);
    uint8_t ws = (uint8_t)((resp >> 16) & 0xFFu);
    uint8_t wc = (uint8_t)(resp & 0xFFu);
    for (uint8_t n = ws; n < ws + wc; n++) {
        uint32_t wcap = 0;
        hda_verb(h, h->codec_addr, n, V_GET_PARAM, P_WIDGET_CAP, &wcap);
        uint8_t wt = (uint8_t)((wcap >> 20) & 0xFu);
        if (wt == WT_AUDIO_OUT && !h->out_nid) h->out_nid = n;
        if (wt == WT_PIN && !h->pin_nid) {
            uint32_t pc = 0;
            hda_verb(h, h->codec_addr, n, V_GET_PARAM, P_PIN_CAPS, &pc);
            if (pc & (1u << 4)) h->pin_nid = n;
        }
    }
    return (h->out_nid && h->pin_nid) ? 0 : -2;
}

static void hda_configure_widgets(hda_state_t *h, uint8_t stream_tag) {
    hda_verb(h, h->codec_addr, h->out_nid, V_SET_POWER, 0x00, NULL);
    hda_verb(h, h->codec_addr, h->pin_nid, V_SET_POWER, 0x00, NULL);
    hda_sleep(h, 5);
    hda_verb4(h, h->codec_addr, h->out_nid, V4_SET_FMT, HDA_FMT_48K_S16_2CH, NULL);
    hda_verb(h, h->codec_addr, h->out_nid, V_SET_STREAM,
             (uint8_t)((stream_tag << 4) | 0u), NULL);
    hda_verb4(h, h->codec_addr, h->out_nid, V4_SET_AMP, 0xB07Fu, NULL);
    hda_verb(h, h->codec_addr, h->pin_nid, V_SET_PIN, 0xC0u, NULL);
    hda_verb(h, h->codec_addr, h->pin_nid, V_SET_EAPD, 0x02u, NULL);
    hda_verb4(h, h->codec_addr, h->pin_nid, V4_SET_AMP, 0xB07Fu, NULL);
}

/* ------------------------------------------------------------------ */
/* Output stream                                                        */
/* ------------------------------------------------------------------ */

static int hda_setup_stream(hda_state_t *h) {
    uint16_t gcap = hr16(h, HDAC_GCAP);
    uint8_t  iss  = (uint8_t)((gcap >> 8) & 0xFu);
    h->sd_base    = HDAC_SDBASE + (uint32_t)iss * HDAC_SD_SZ;

    sdw32(h, SD_CTL, SDC_SRST);
    for (int i=0;i<100;i++){if (sd32(h,SD_CTL)&SDC_SRST) break; hda_sleep(h,1);}
    sdw32(h, SD_CTL, 0);
    for (int i=0;i<100;i++){if (!(sd32(h,SD_CTL)&SDC_SRST)) break; hda_sleep(h,1);}

    for (uint32_t i = 0; i < HDA_BDL_ENTRIES; i++) {
        h->bdl[i].addr   = h->buf_dma.phys + (uint64_t)i * HDA_SEG_BYTES;
        h->bdl[i].length = HDA_SEG_BYTES;
        h->bdl[i].ioc    = 1u;
    }

    sdw16(h, SD_FMT,  HDA_FMT_48K_S16_2CH);
    sdw32(h, SD_CBL,  HDA_BUF_BYTES);
    sdw16(h, SD_LVI,  (uint16_t)(HDA_BDL_ENTRIES - 1u));
    sdw32(h, SD_BDLPL,(uint32_t)(h->bdl_dma.phys & 0xFFFFFFFFu));
    sdw32(h, SD_BDLPU,(uint32_t)(h->bdl_dma.phys >> 32));

    uint32_t ctl = (1u << SDC_STRM_SHIFT) | SDC_DIR | SDC_IOCE;
    sdw32(h, SD_CTL, ctl);
    sdw32(h, SD_CTL, ctl | SDC_RUN);

    h->next_fill = 0;
    h->queued    = 0;
    h->running   = 1;

    com_printf(COM1_PORT, "[hda] stream started: sd_base=0x%x ISS=%u LVI=%u FMT=0x%04x\n",
               (unsigned)h->sd_base, (unsigned)iss,
               (unsigned)(HDA_BDL_ENTRIES-1u), (unsigned)HDA_FMT_48K_S16_2CH);
    return 0;
}

/* ------------------------------------------------------------------ */
/* IRQ handler                                                          */
/* ------------------------------------------------------------------ */

static void hda_irq_handler(void) {
    if (!g_state_for_handler) return;
    hda_state_t *h = g_state_for_handler;
    uint8_t sts = *((volatile uint8_t *)(h->base + h->sd_base + SD_STS));
    *((volatile uint8_t *)(h->base + h->sd_base + SD_STS)) = sts;
    if ((sts & 0x04u) && h->queued > 0) h->queued--;
    if (h->api->pic_send_eoi) h->api->pic_send_eoi(g_irq_line_for_handler);
}

/* ------------------------------------------------------------------ */
/* PCM ops                                                              */
/* ------------------------------------------------------------------ */

static int hda_pcm_open(void *ctx) { (void)ctx; return 0; }

static int hda_pcm_set_config(void *ctx, const audio_pcm_config_t *cfg) {
    (void)ctx;
    if (!cfg) return -1;
    if (cfg->sample_rate != 48000 || cfg->channels != 2 ||
        cfg->format != AUDIO_FMT_S16_LE) return -2;
    return 0;
}

static long hda_pcm_write(void *ctx, const void *buf, size_t bytes) {
    hda_state_t *h = (hda_state_t *)ctx;
    if (!h || !buf || !bytes) return 0;

    static int logged_once = 0;
    if (!logged_once) {
        logged_once = 1;
        h->api->com_write_string(COM1_PORT, "[hda] first write() received\n");
    }

    const uint8_t *src = (const uint8_t *)buf;
    size_t written = 0;

    while (written < bytes) {
        if (h->queued >= (HDA_BDL_ENTRIES - 1u)) break;

        size_t chunk = HDA_SEG_BYTES;
        if (chunk > (bytes - written)) chunk = bytes - written;

        uint8_t *dst = h->buf + (size_t)h->next_fill * HDA_SEG_BYTES;
        memcpy_local(dst, src + written, chunk);
        if (chunk < HDA_SEG_BYTES)
            memset_local(dst + chunk, 0, HDA_SEG_BYTES - chunk);

        h->next_fill = (h->next_fill + 1u) % HDA_BDL_ENTRIES;
        h->queued++;
        written += chunk;

        uint16_t new_lvi = (uint16_t)((h->next_fill == 0u)
                           ? HDA_BDL_ENTRIES - 1u : h->next_fill - 1u);
        sdw16(h, SD_LVI, new_lvi);
    }
    return (long)written;
}

static int hda_pcm_drain(void *ctx) {
    hda_state_t *h = (hda_state_t *)ctx;
    if (!h) return -1;
    uint64_t ms = (uint64_t)HDA_BUF_BYTES * 1000ULL / (48000ULL * 2ULL * 2ULL);
    if (h->api->sleep_ms) h->api->sleep_ms(ms);
    return 0;
}

static int hda_pcm_close(void *ctx) { (void)ctx; return 0; }

static int hda_pcm_get_info(void *ctx, audio_device_info_t *out) {
    (void)ctx;
    if (!out) return -1;
    memset_local(out, 0, sizeof(*out));
    const char *nm = "hda0";
    for (size_t i = 0; i < sizeof(out->name)-1u && nm[i]; i++) out->name[i] = nm[i];
    out->preferred.sample_rate = 48000;
    out->preferred.channels    = 2;
    out->preferred.format      = AUDIO_FMT_S16_LE;
    return 0;
}

/* FIX: removed bogus (ssize_t(*)(void*,const void*,size_t)) cast —
 * ssize_t is a POSIX type unavailable in freestanding builds, and
 * hda_pcm_write already returns long which matches audio_pcm_ops_t.write. */
static const audio_pcm_ops_t g_ops = {
    .open       = hda_pcm_open,
    .set_config = hda_pcm_set_config,
    .write      = hda_pcm_write,
    .drain      = hda_pcm_drain,
    .close      = hda_pcm_close,
    .get_info   = hda_pcm_get_info,
};

/* ------------------------------------------------------------------ */
/* Module entry point                                                   */
/* ------------------------------------------------------------------ */

int sqrm_module_init(const sqrm_kernel_api_t *api) {
    if (!api) return -1;

#define LOG(s) do { if (api->com_write_string) api->com_write_string(COM1_PORT, s); } while(0)

    LOG("[hda] init\n");

    if (!api->audio_register_pcm || !api->dma_alloc ||
        !api->ioremap || !api->outl || !api->inl) {
        LOG("[hda] missing required kernel APIs\n");
        return -2;
    }

    /* PCI scan — raw CF8/CFC, same as AC97 */
    uint8_t bus = 0, dev = 0, fn = 0;
    if (hda_find_pci(api, &bus, &dev, &fn) != 0) {
        LOG("[hda] no HDA controller found (class 0x04 subclass 0x03)\n");
        return -3;
    }
    LOG("[hda] found HDA PCI device (class 0x0403)\n");

    /* Enable memory space + bus mastering */
    uint16_t cmd = pci_read16(api, bus, dev, fn, 0x04);
    cmd |= 0x0002u | 0x0004u;
    pci_write32(api, bus, dev, fn, 0x04, (uint32_t)cmd);

    uint8_t  irq_line = pci_read8 (api, bus, dev, fn, 0x3C);
    uint32_t bar0_lo  = pci_read32(api, bus, dev, fn, 0x10);
    uint32_t bar0_hi  = pci_read32(api, bus, dev, fn, 0x14);
    uint64_t bar0     = ((uint64_t)bar0_hi << 32) | (bar0_lo & ~0xFu);

    if (!bar0) {
        LOG("[hda] BAR0 not assigned\n");
        return -4;
    }

    memset_local(&g, 0, sizeof(g));
    g.api      = api;
    g.irq_line = irq_line;

    g.base = (uint8_t *)api->ioremap(bar0, 0x4000u);
    if (!g.base) { LOG("[hda] ioremap failed\n"); return -5; }

    if (api->dma_alloc(&g.corb_dma, 256u*4u,  128u) != 0) { LOG("[hda] CORB alloc\n");    return -6; }
    if (api->dma_alloc(&g.rirb_dma, 256u*8u,  128u) != 0) { LOG("[hda] RIRB alloc\n");    return -7; }
    if (api->dma_alloc(&g.bdl_dma,  HDA_BDL_ENTRIES*sizeof(hda_bdl_entry_t), 128u) != 0)
                                                           { LOG("[hda] BDL alloc\n");     return -8; }
    if (api->dma_alloc(&g.buf_dma,  HDA_BUF_BYTES, 128u) != 0)
                                                           { LOG("[hda] buf alloc\n");     return -9; }

    g.corb = (uint32_t *)g.corb_dma.virt;
    g.rirb = (uint64_t *)g.rirb_dma.virt;
    g.bdl  = (hda_bdl_entry_t *)g.bdl_dma.virt;
    g.buf  = (uint8_t *)g.buf_dma.virt;

    memset_local(g.corb, 0, 256u*4u);
    memset_local(g.rirb, 0, 256u*8u);
    memset_local(g.bdl,  0, HDA_BDL_ENTRIES*sizeof(hda_bdl_entry_t));
    memset_local(g.buf,  0, HDA_BUF_BYTES);

    if (hda_reset_ctrl(&g) != 0)   { LOG("[hda] reset failed\n");          return -10; }
    LOG("[hda] controller reset OK\n");

    hda_init_corb(&g);
    hda_init_rirb(&g);
    LOG("[hda] CORB/RIRB ready\n");

    if (hda_find_codec(&g) != 0)   { LOG("[hda] no codec\n");              return -11; }
    LOG("[hda] codec found\n");

    if (hda_probe_widgets(&g) != 0){ LOG("[hda] no DAC/pin found\n");       return -12; }
    LOG("[hda] widgets probed OK\n");

    hda_configure_widgets(&g, 1u);
    LOG("[hda] codec configured\n");

    if (hda_setup_stream(&g) != 0) { LOG("[hda] stream setup failed\n");   return -13; }
    LOG("[hda] output stream running\n");

    if (api->irq_install_handler && api->pic_send_eoi && irq_line < 16u) {
        g_irq_line_for_handler = irq_line;
        g_state_for_handler    = &g;
        api->irq_install_handler((int)irq_line, hda_irq_handler);
        LOG("[hda] IRQ handler installed\n");
    } else {
        LOG("[hda] no IRQ; polled mode\n");
    }

    int r = api->audio_register_pcm("hda0", &g_ops, &g);
    if (r != 0) { LOG("[hda] audio_register_pcm failed\n"); return -14; }

    LOG("[hda] registered /dev/audio/hda0\n");
    return 0;

#undef LOG
}