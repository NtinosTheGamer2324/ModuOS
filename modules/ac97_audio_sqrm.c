#include "moduos/kernel/sqrm.h"

/*
 * AC97 audio driver (SQRM module) - minimal PCM out for QEMU -device AC97.
 *
 * IMPORTANT: SQRM modules must not rely on unresolved external symbols.
 * This module only uses the function pointers provided in sqrm_kernel_api_t
 * (port IO, DMA, com_write_string, audio_register_pcm).
 */

static const uint16_t COM1_PORT = 0x3F8;

const sqrm_module_desc_t sqrm_module_desc = {
    .abi_version = 1,
    .type = SQRM_TYPE_AUDIO,
    .name = "ac97",
};

static void *memcpy_local(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t*)dst;
    const uint8_t *s = (const uint8_t*)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}
static void *memset_local(void *dst, int v, size_t n) {
    uint8_t *d = (uint8_t*)dst;
    for (size_t i = 0; i < n; i++) d[i] = (uint8_t)v;
    return dst;
}

/* Intel ICH AC97 (bus master) registers (I/O BAR, commonly BAR0) */
#define AC97_PO_BDBAR   0x10
#define AC97_PO_CIV     0x14
#define AC97_PO_LVI     0x15
#define AC97_PO_SR      0x16 /* 16-bit */
#define AC97_PO_PICB    0x18 /* 16-bit */
#define AC97_PO_CR      0x1B

#define AC97_CR_RPBM    0x01
#define AC97_CR_RR      0x02
#define AC97_CR_LVBIE   0x04
#define AC97_CR_FEIE    0x08
#define AC97_CR_IOCE    0x10

#define AC97_SR_DCH     0x0001
#define AC97_SR_CELV    0x0002
#define AC97_SR_LVBCI   0x0004
#define AC97_SR_BCIS    0x0008
#define AC97_SR_FIFOE   0x0010
#define AC97_SR_BMINT   0x0020
#define AC97_SR_LVBE    0x0040
#define AC97_SR_FIFOR   0x0080

/* AC97 mixer (native audio) registers (I/O BAR1, commonly BAR1) */
#define AC97_RESET      0x00
#define AC97_MASTER_VOL 0x02
#define AC97_PCMOUT_VOL 0x18

typedef struct __attribute__((packed)) {
    uint32_t buffer_phys;
    uint16_t length;      /* in samples (?) for AC97: in 16-bit words? We'll use bytes/2 */
    uint16_t control;     /* IOC=0x8000 */
} ac97_bd_t;

#define AC97_BD_IOC 0x8000

typedef struct {
    const sqrm_kernel_api_t *api;
    uint16_t bm_io;   /* bus master base port */
    uint16_t mix_io;  /* mixer base port */
    uint8_t irq_line;

    dma_buffer_t bdl_dma;
    dma_buffer_t buf_dma;

    ac97_bd_t *bdl;
    uint8_t *buf;

    /* DMA ring layout */
    uint32_t seg_count;
    uint32_t seg_bytes;

    volatile uint32_t queued;   /* queued segments (not yet played) */
    uint8_t next_fill;          /* next segment index to fill */
    uint8_t lvi;                /* last valid index programmed */
    uint8_t last_civ;           /* last observed current index */
    int running;
} ac97_state_t;

static ac97_state_t g;

static inline void io_wait_local(const sqrm_kernel_api_t *api) {
    (void)api;
}

static uint32_t pci_cfg_read32(const sqrm_kernel_api_t *api, uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) | ((uint32_t)fn << 8) | (off & 0xFCu);
    api->outl(0xCF8, addr);
    return api->inl(0xCFC);
}
static void pci_cfg_write32(const sqrm_kernel_api_t *api, uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t val) {
    uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) | ((uint32_t)fn << 8) | (off & 0xFCu);
    api->outl(0xCF8, addr);
    api->outl(0xCFC, val);
}
static uint16_t pci_cfg_read16(const sqrm_kernel_api_t *api, uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    uint32_t v = pci_cfg_read32(api, bus, dev, fn, off);
    return (uint16_t)((v >> ((off & 2u) * 8u)) & 0xFFFFu);
}
static uint8_t pci_cfg_read8(const sqrm_kernel_api_t *api, uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    uint32_t v = pci_cfg_read32(api, bus, dev, fn, off);
    return (uint8_t)((v >> ((off & 3u) * 8u)) & 0xFFu);
}

static int ac97_find_pci(ac97_state_t *s, uint8_t *out_bus, uint8_t *out_dev, uint8_t *out_fn) {
    const sqrm_kernel_api_t *api = s->api;
    for (uint8_t bus = 0; bus < 0xFF; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            for (uint8_t fn = 0; fn < 8; fn++) {
                uint16_t vendor = pci_cfg_read16(api, bus, dev, fn, 0x00);
                if (vendor == 0xFFFF) {
                    if (fn == 0) break;
                    continue;
                }
                uint8_t class_code = pci_cfg_read8(api, bus, dev, fn, 0x0B);
                uint8_t subclass = pci_cfg_read8(api, bus, dev, fn, 0x0A);
                if (class_code == 0x04 && subclass == 0x01) {
                    *out_bus = bus; *out_dev = dev; *out_fn = fn;
                    return 0;
                }
            }
        }
    }
    return -1;
}

static uint8_t g_irq_line_for_handler = 0;
static ac97_state_t *g_state_for_handler = NULL;

static void ac97_irq_handler(void) {
    if (!g_state_for_handler || !g_state_for_handler->api) return;
    ac97_state_t *s = g_state_for_handler;
    const sqrm_kernel_api_t *api = s->api;

    uint16_t sr = api->inw(s->bm_io + AC97_PO_SR);
    api->outw(s->bm_io + AC97_PO_SR, sr); /* W1C */

    if (sr & (AC97_SR_BCIS | AC97_SR_LVBCI)) {
        uint8_t civ = api->inb(s->bm_io + AC97_PO_CIV);
        /* In practice, BCIS means one buffer completed. */
        uint8_t done = (uint8_t)((sr & AC97_SR_BCIS) ? 1 : 0);
        /* If LVBCI is set, we may have drained to the last valid index. */
        if ((sr & AC97_SR_LVBCI) && done == 0) done = 1;

        if (done) {
            uint32_t q = s->queued;
            if (q > done) s->queued = q - done;
            else s->queued = 0;
        }
        s->last_civ = civ;
    }

    if (api->pic_send_eoi) api->pic_send_eoi(g_irq_line_for_handler);
}

static int ac97_hw_init(ac97_state_t *s) {
    const sqrm_kernel_api_t *api = s->api;

    uint8_t bus=0, dev=0, fn=0;
    if (ac97_find_pci(s, &bus, &dev, &fn) != 0) {
        api->com_write_string(COM1_PORT, "[ac97] PCI audio device not found\n");
        return -1;
    }

    /* Enable IO space + bus mastering */
    uint16_t cmd = pci_cfg_read16(api, bus, dev, fn, 0x04);
    cmd |= 0x0001; /* IO */
    cmd |= 0x0004; /* bus master */
    pci_cfg_write32(api, bus, dev, fn, 0x04, (uint32_t)cmd);

    s->irq_line = pci_cfg_read8(api, bus, dev, fn, 0x3C);
    uint32_t bar0 = pci_cfg_read32(api, bus, dev, fn, 0x10);
    uint32_t bar1 = pci_cfg_read32(api, bus, dev, fn, 0x14);
    if ((bar0 & 1u) == 0 || (bar1 & 1u) == 0) {
        api->com_write_string(COM1_PORT, "[ac97] Expected IO BARs\n");
        return -2;
    }

    s->bm_io = (uint16_t)(bar0 & ~3u);
    s->mix_io = (uint16_t)(bar1 & ~3u);

    api->com_write_string(COM1_PORT, "[ac97] found PCI audio (class 0x0401)\n");

    /* Reset mixer */
    api->outw(s->mix_io + AC97_RESET, 0);

    /* Set volumes (0 = max, 0x1f = min/mute). Unmute/max volume for now. */
    api->outw(s->mix_io + AC97_MASTER_VOL, 0x0000);
    api->outw(s->mix_io + AC97_PCMOUT_VOL, 0x0000);

    /* Reset PCM out bus master */
    api->outb(s->bm_io + AC97_PO_CR, AC97_CR_RR);

    /* Install IRQ handler */
    if (api->irq_install_handler && api->pic_send_eoi && s->irq_line < 16) {
        g_irq_line_for_handler = s->irq_line;
        g_state_for_handler = s;
        api->irq_install_handler((int)s->irq_line, ac97_irq_handler);
        api->com_write_string(COM1_PORT, "[ac97] IRQ handler installed\n");
    } else {
        api->com_write_string(COM1_PORT, "[ac97] IRQ not available; will still attempt playback\n");
    }

    return 0;
}

static int ac97_pcm_open(void *ctx) {
    (void)ctx;
    return 0;
}

static int ac97_pcm_set_config(void *ctx, const audio_pcm_config_t *cfg) {
    (void)ctx;
    /* For v1 we support only 48kHz stereo S16LE */
    if (!cfg) return -1;
    if (cfg->sample_rate != 48000 || cfg->channels != 2 || cfg->format != AUDIO_FMT_S16_LE) return -2;
    return 0;
}

static void ac97_start_if_needed(ac97_state_t *s) {
    const sqrm_kernel_api_t *api = s->api;
    if (s->running) return;
    if (s->queued < 2) return; /* need some buffered audio */

    /* reset run */
    api->outb(s->bm_io + AC97_PO_CR, AC97_CR_RR);

    api->outw(s->bm_io + AC97_PO_SR, 0xFFFF); /* clear */
    api->outl(s->bm_io + AC97_PO_BDBAR, (uint32_t)s->bdl_dma.phys);
    api->outb(s->bm_io + AC97_PO_LVI, s->lvi);

    api->outb(s->bm_io + AC97_PO_CR, (uint8_t)(AC97_CR_RPBM | AC97_CR_IOCE | AC97_CR_FEIE | AC97_CR_LVBIE));
    s->running = 1;
    s->last_civ = api->inb(s->bm_io + AC97_PO_CIV);

    /* Debug current state */
    {
        uint8_t civ = api->inb(s->bm_io + AC97_PO_CIV);
        uint8_t lvi = api->inb(s->bm_io + AC97_PO_LVI);
        uint8_t cr  = api->inb(s->bm_io + AC97_PO_CR);
        uint16_t sr = api->inw(s->bm_io + AC97_PO_SR);
        com_printf(COM1_PORT, "[ac97] start: CIV=%u LVI=%u CR=0x%02x SR=0x%04x queued=%u\n",
                   (unsigned)civ, (unsigned)lvi, (unsigned)cr, (unsigned)sr, (unsigned)s->queued);
    }
}

static long ac97_pcm_write(void *ctx, const void *buf, size_t bytes) {
    ac97_state_t *s = (ac97_state_t*)ctx;
    const sqrm_kernel_api_t *api = s->api;
    if (!s || !api || !buf || bytes == 0) return 0;

    const uint8_t *src = (const uint8_t*)buf;
    size_t written = 0;

    /* debug once */
    static int logged_once = 0;
    if (!logged_once) {
        logged_once = 1;
        api->com_write_string(COM1_PORT, "[ac97] first write() received\n");
    }

    while (written < bytes) {
        /* ring full? leave one segment to avoid LVI overrun */
        if (s->queued >= (s->seg_count - 1)) break;

        size_t chunk = s->seg_bytes;
        if (chunk > (bytes - written)) chunk = bytes - written;

        uint32_t idx = s->next_fill;
        uint8_t *dst = s->buf + (size_t)idx * s->seg_bytes;
        memcpy_local(dst, src + written, chunk);

        /* zero any tail to avoid clicks */
        if (chunk < s->seg_bytes) {
            memset_local(dst + chunk, 0, s->seg_bytes - chunk);
        }

        /* Update descriptor length in 16-bit samples */
        s->bdl[idx].length = (uint16_t)(s->seg_bytes / 2);

        /* Advance LVI and queue count */
        s->lvi = (uint8_t)idx;
        s->next_fill = (uint8_t)((idx + 1) % s->seg_count);
        s->queued++;

        if (s->running) {
            api->outb(s->bm_io + AC97_PO_LVI, s->lvi);
        }

        written += chunk;
    }

    ac97_start_if_needed(s);
    return (long)written;
}

static int ac97_pcm_drain(void *ctx) {
    (void)ctx;
    return 0;
}

static int ac97_pcm_close(void *ctx) {
    (void)ctx;
    return 0;
}

static int ac97_pcm_get_info(void *ctx, audio_device_info_t *out) {
    (void)ctx;
    if (!out) return -1;
    memset_local(out, 0, sizeof(*out));
    /* name */
    const char *nm = "ac97";
    for (size_t i = 0; i < sizeof(out->name) - 1 && nm[i]; i++) out->name[i] = nm[i];
    out->preferred.sample_rate = 48000;
    out->preferred.channels = 2;
    out->preferred.format = AUDIO_FMT_S16_LE;
    return 0;
}

static const audio_pcm_ops_t g_ops = {
    .open = ac97_pcm_open,
    .set_config = ac97_pcm_set_config,
    .write = (ssize_t(*)(void*,const void*,size_t))ac97_pcm_write,
    .drain = ac97_pcm_drain,
    .close = ac97_pcm_close,
    .get_info = ac97_pcm_get_info,
};

int sqrm_module_init(const sqrm_kernel_api_t *api) {
    if (!api) return -1;
    g.api = api;

    if (!api->audio_register_pcm || !api->dma_alloc || !api->outb || !api->outw || !api->outl) {
        api->com_write_string(COM1_PORT, "[ac97] missing required kernel APIs\n");
        return -2;
    }

    if (ac97_hw_init(&g) != 0) return -3;

    /* Allocate DMA buffers */
    if (api->dma_alloc(&g.bdl_dma, 4096, 16) != 0) {
        api->com_write_string(COM1_PORT, "[ac97] dma_alloc bdl failed\n");
        return -4;
    }
    /* 32 segments * 4096 bytes = 131072 */
    if (api->dma_alloc(&g.buf_dma, 131072, 16) != 0) {
        api->com_write_string(COM1_PORT, "[ac97] dma_alloc buf failed\n");
        return -5;
    }

    g.bdl = (ac97_bd_t*)g.bdl_dma.virt;
    g.buf = (uint8_t*)g.buf_dma.virt;

    g.seg_bytes = 4096;
    g.seg_count = (uint32_t)(g.buf_dma.size / g.seg_bytes);
    if (g.seg_count > 32) g.seg_count = 32;
    if (g.seg_count < 4) g.seg_count = 4;
    g.queued = 0;
    g.next_fill = 0;
    g.lvi = 0;
    g.last_civ = 0;
    g.running = 0;

    memset_local(g.bdl, 0, 4096);
    memset_local(g.buf, 0, g.buf_dma.size);

    /* Build BDL ring: each entry points at a fixed segment */
    for (uint32_t i = 0; i < g.seg_count; i++) {
        g.bdl[i].buffer_phys = (uint32_t)(g.buf_dma.phys + (uint64_t)i * g.seg_bytes);
        g.bdl[i].length = (uint16_t)(g.seg_bytes / 2);
        g.bdl[i].control = AC97_BD_IOC;
    }

    /* Program BDL base once */
    api->outl(g.bm_io + AC97_PO_BDBAR, (uint32_t)g.bdl_dma.phys);

    int r = api->audio_register_pcm("pcm0", &g_ops, &g);
    if (r != 0) {
        api->com_write_string(COM1_PORT, "[ac97] audio_register_pcm failed\n");
        return -6;
    }

    api->com_write_string(COM1_PORT, "[ac97] registered /dev/audio/pcm0\n");
    return 0;
}
