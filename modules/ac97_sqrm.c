/*
 * ac97_audio.c — AC'97 driver, registered as SQRM_TYPE_AUDIO.
 *
 * DIAGNOSTIC BUILD: instrumented to determine why the IRQ-driven BDL
 * refill loop never appears to engage after the initial pre-filled
 * buffers play through once. Search "DEBUG" for every change relative
 * to the last known-good version. Remove once resolved.
 */

#include "moduos/kernel/sqrm.h"
#include "audiosqrm/contract.h"
#include <stdint.h>
#include <stddef.h>

const sqrm_kernel_api_t *g_api;

#define COM1_PORT 0x3F8

static const char *deps[] = { "audiomanager" };
SQRM_DEFINE_MODULE_V2(SQRM_TYPE_AUDIO, "ac97", 2, 1, 1, deps);

#define AC97_PCI_VENDOR_INTEL   0x8086
#define AC97_PCI_DEVICE_ICH     0x2415

#define AC97_NAM_RESET          0x00
#define AC97_NAM_MASTER_VOL     0x02
#define AC97_NAM_PCM_OUT_VOL    0x18

#define AC97_NABM_PO_BDBAR      0x10
#define AC97_NABM_PO_CIV        0x14
#define AC97_NABM_PO_LVI        0x15
#define AC97_NABM_PO_SR         0x16
#define AC97_NABM_PO_CR         0x1B
#define AC97_NABM_GLOB_CNT      0x2C
#define AC97_NABM_GLOB_STA      0x30

#define AC97_SR_BCIS            (1 << 3)
#define AC97_SR_LVBCI           (1 << 2)

#define AC97_CR_RPBM            (1 << 0)
#define AC97_CR_IOCE            (1 << 4)

#define AC97_GLOB_STA_PCRDY     (1 << 8)

#define AC97_BDL_ENTRIES        4
#define AC97_SAMPLES_PER_ENTRY  512

typedef struct __attribute__((packed)) {
    uint32_t buf_addr;
    uint16_t num_samples;
    uint16_t flags;
} ac97_bdl_entry_t;

#define AC97_BDL_FLAG_IOC (1 << 15)

typedef struct {
    uint16_t nam_base;
    uint16_t nabm_base;
    uint8_t  irq_line;
    pci_device_t *pci_dev;
} ac97_dev_t;

static ac97_dev_t g_ac97;
static dma_buffer_t g_bdl_dma;
static dma_buffer_t g_pcm_dma;

static const audio_api_t *g_audmgr = NULL;
static int g_ac97_output_id = -1;

/* ── DEBUG: tiny hex/decimal logging helpers (no printf available) ── */
static void dbg_hex16(const char *prefix, uint16_t v) {
    static const char hexd[] = "0123456789ABCDEF";
    char buf[7];
    buf[0] = '0'; buf[1] = 'x';
    buf[2] = hexd[(v >> 12) & 0xF];
    buf[3] = hexd[(v >> 8) & 0xF];
    buf[4] = hexd[(v >> 4) & 0xF];
    buf[5] = hexd[v & 0xF];
    buf[6] = 0;
    g_api->com_write_string(COM1_PORT, "[ac97][DEBUG] ");
    g_api->com_write_string(COM1_PORT, prefix);
    g_api->com_write_string(COM1_PORT, buf);
    g_api->com_write_string(COM1_PORT, "\n");
}

static void dbg_dec32(const char *prefix, uint32_t v) {
    char buf[12];
    int i = 10;
    buf[11] = 0;
    if (v == 0) { buf[i--] = '0'; }
    else { while (v > 0 && i >= 0) { buf[i--] = (char)('0' + (v % 10)); v /= 10; } }
    g_api->com_write_string(COM1_PORT, "[ac97][DEBUG] ");
    g_api->com_write_string(COM1_PORT, prefix);
    g_api->com_write_string(COM1_PORT, &buf[i + 1]);
    g_api->com_write_string(COM1_PORT, "\n");
}

/* Caps so a working system doesn't spam the serial console forever —
 * we only need the first few data points to diagnose this. */
static uint32_t g_dbg_handler_entries = 0;
#define DBG_HANDLER_LOG_CAP   8u
static uint32_t g_dbg_refills = 0;
#define DBG_REFILL_LOG_CAP    8u

static int ac97_pci_find_and_map(void) {
    pci_device_t *dev = NULL;
    for (int i = 0; i < 256; i++) {
        pci_device_t *tmpdev = g_api->pci_get_device(i);
        if (!tmpdev) continue;

        if (tmpdev->class_code == 0x04 && tmpdev->subclass == 0x01) {
            dev = tmpdev;
            break;
        }
    }

    if (!dev) {
        g_api->com_write_string(COM1_PORT, "[ac97] no matching PCI multimedia audio controller found\n");
        return -1;
    }

    g_ac97.pci_dev = dev;

    g_api->pci_enable_io_space(dev);
    g_api->pci_enable_bus_mastering(dev);

    if (dev->bar_type[0] != PCI_BAR_IO || dev->bar_type[1] != PCI_BAR_IO) {
        g_api->com_write_string(COM1_PORT,
            "[ac97] BAR0/BAR1 aren't both I/O-space — unexpected layout, bailing\n");
        return -1;
    }

    g_ac97.nam_base  = (uint16_t)(dev->bar[0] & 0xFFFC);
    g_ac97.nabm_base = (uint16_t)(dev->bar[1] & 0xFFFC);
    g_ac97.irq_line  = dev->interrupt_line;

    /* DEBUG: confirm exactly what we think the IRQ line and I/O bases
     * are, and — critically — whether bus mastering actually stuck
     * (some chipsets/emulators silently ignore the enable if written
     * before other init steps, or need it re-asserted later). */
    dbg_hex16("nam_base=", g_ac97.nam_base);
    dbg_hex16("nabm_base=", g_ac97.nabm_base);
    dbg_dec32("irq_line=", g_ac97.irq_line);
    /* (Bus-mastering-enable readback dropped: pci_cfg_read32() takes
     * raw (bus, device, function, offset), and pci_device_t's field
     * names for those aren't visible here to wire it up correctly.
     * Not essential to the core diagnosis below.) */

    return 0;
}

static void ac97_codec_reset(void) {
    g_api->outw(g_ac97.nam_base + AC97_NAM_RESET, 0x0000);
    g_api->sleep_ms(5);
}

static void ac97_set_master_volume(uint8_t left_atten, uint8_t right_atten, int mute) {
    uint16_t v = ((uint16_t)(left_atten & 0x1F) << 8) | (right_atten & 0x1F);
    if (mute) v |= 0x8000;
    g_api->outw(g_ac97.nam_base + AC97_NAM_MASTER_VOL, v);
    g_api->outw(g_ac97.nam_base + AC97_NAM_PCM_OUT_VOL, v);
}

#define AC97_TONE_FREQ_HZ    3000
#define AC97_SAMPLE_RATE_HZ  48000
#define AC97_TONE_AMPLITUDE  12000

static void ac97_fill_tone_buffer(int16_t *frames, size_t frame_count) {
    size_t half_period = AC97_SAMPLE_RATE_HZ / (AC97_TONE_FREQ_HZ * 2);
    if (half_period == 0) half_period = 1;

    for (size_t i = 0; i < frame_count; i++) {
        int16_t v = ((i / half_period) % 2 == 0) ? AC97_TONE_AMPLITUDE : -AC97_TONE_AMPLITUDE;
        frames[i * 2 + 0] = v;
        frames[i * 2 + 1] = v;
    }
}

#define AC97_SEGMENT_BYTES ((size_t)AC97_SAMPLES_PER_ENTRY * 2 * sizeof(int16_t))

static int16_t *ac97_segment(uint8_t idx) {
    return (int16_t *)((uint8_t *)g_pcm_dma.virt + (size_t)idx * AC97_SEGMENT_BYTES);
}

static void ac97_refill_segment(uint8_t idx) {
    int16_t *seg = ac97_segment(idx);
    if (g_audmgr) {
        g_audmgr->pull_output_frames(seg, AC97_SAMPLES_PER_ENTRY);
    } else {
        ac97_fill_tone_buffer(seg, AC97_SAMPLES_PER_ENTRY);
    }
    /* DEBUG: confirm the refill path itself actually executes and
     * which source (audmgr vs local tone) it took. */
    if (g_dbg_refills < DBG_REFILL_LOG_CAP) {
        g_dbg_refills++;
        dbg_dec32("refill executed, segment=", idx);
    }
}

static int ac97_setup_dma(void) {
    size_t pcm_bytes = (size_t)AC97_BDL_ENTRIES * AC97_SEGMENT_BYTES;
    size_t bdl_bytes = (size_t)AC97_BDL_ENTRIES * sizeof(ac97_bdl_entry_t);

    if (g_api->dma_alloc(&g_pcm_dma, pcm_bytes, 8) != 0) {
        g_api->com_write_string(COM1_PORT, "[ac97] dma_alloc(pcm) failed\n");
        return -1;
    }
    if (g_api->dma_alloc(&g_bdl_dma, bdl_bytes, 8) != 0) {
        g_api->com_write_string(COM1_PORT, "[ac97] dma_alloc(bdl) failed\n");
        g_api->dma_free(&g_pcm_dma);
        return -1;
    }

    ac97_bdl_entry_t *bdl = (ac97_bdl_entry_t *)g_bdl_dma.virt;
    for (int i = 0; i < AC97_BDL_ENTRIES; i++) {
        ac97_fill_tone_buffer(ac97_segment((uint8_t)i), AC97_SAMPLES_PER_ENTRY);

        bdl[i].buf_addr    = (uint32_t)(g_pcm_dma.phys + (uint64_t)i * AC97_SEGMENT_BYTES);
        bdl[i].num_samples = AC97_SAMPLES_PER_ENTRY * 2;
        bdl[i].flags       = AC97_BDL_FLAG_IOC;
    }

    g_api->outl(g_ac97.nabm_base + AC97_NABM_PO_BDBAR, (uint32_t)g_bdl_dma.phys);
    g_api->outb(g_ac97.nabm_base + AC97_NABM_PO_LVI, AC97_BDL_ENTRIES - 1);

    /* DEBUG: read back BDBAR/LVI immediately to confirm the writes
     * actually landed (catches I/O port mis-mapping). */
    dbg_hex16("BDBAR low16 readback=", (uint16_t)g_api->inl(g_ac97.nabm_base + AC97_NABM_PO_BDBAR));
    dbg_dec32("LVI readback=", g_api->inb(g_ac97.nabm_base + AC97_NABM_PO_LVI));

    return 0;
}

static void ac97_irq_handler(void) {
    uint16_t sr = g_api->inw(g_ac97.nabm_base + AC97_NABM_PO_SR);

    /* DEBUG: log every entry into this handler (capped) with the raw SR
     * value, BEFORE the condition check, so we can tell apart:
     *   (a) handler never entered at all -> nothing logged, ever
     *   (b) handler entered but sr bits never match -> logged, but
     *       "refill executed" from ac97_refill_segment() never appears
     *   (c) handler entered, bits matched, refill executed -> both
     *       lines appear and audio should actually flow */
    if (g_dbg_handler_entries < DBG_HANDLER_LOG_CAP) {
        g_dbg_handler_entries++;
        dbg_hex16("IRQ handler entered, SR=", sr);
    }

    if (sr & (AC97_SR_BCIS | AC97_SR_LVBCI)) {
        g_api->outw(g_ac97.nabm_base + AC97_NABM_PO_SR, sr);
        uint8_t civ = g_api->inb(g_ac97.nabm_base + AC97_NABM_PO_CIV);
        uint8_t finished = (uint8_t)((civ + AC97_BDL_ENTRIES - 1) % AC97_BDL_ENTRIES);
        ac97_refill_segment(finished);
        g_api->outb(g_ac97.nabm_base + AC97_NABM_PO_LVI, finished);
    }
    g_api->pic_send_eoi(g_ac97.irq_line);
}

static void ac97_boot_test_tone_start(void) {
    g_api->outb(g_ac97.nabm_base + AC97_NABM_PO_CR, AC97_CR_RPBM | AC97_CR_IOCE);
    /* DEBUG: read CR back immediately — if RPBM doesn't stick, the bus
     * master never actually started despite the write "succeeding". */
    dbg_hex16("PO_CR readback after start=", g_api->inb(g_ac97.nabm_base + AC97_NABM_PO_CR));
}

int sqrm_module_init(const sqrm_kernel_api_t *api) {
    if (!api) return -1;
    g_api = api;

    g_api->com_write_string(COM1_PORT, "[ac97] TYPE_AUDIO module loading\n");

    if (ac97_pci_find_and_map() != 0) {
        return -1;
    }

    uint32_t sta = g_api->inl(g_ac97.nabm_base + AC97_NABM_GLOB_STA);
    dbg_hex16("GLOB_STA at init=", (uint16_t)sta);
    if (!(sta & AC97_GLOB_STA_PCRDY)) {
        g_api->com_write_string(COM1_PORT, "[ac97] codec not ready (GLOB_STA.PCRDY clear)\n");
        return -1;
    }

    ac97_codec_reset();
    ac97_set_master_volume(0, 0, 0);

    if (ac97_setup_dma() != 0) {
        return -1;
    }

    if (api->sqrm_service_get) {
        size_t api_size = 0;
        g_audmgr = (const audio_api_t *)api->sqrm_service_get("audiomanager", &api_size);
        if (g_audmgr && api_size < sizeof(audio_api_t)) {
            g_api->com_write_string(COM1_PORT,
                "[ac97] audiomanager service blob smaller than expected audio_api_t, ignoring\n");
            g_audmgr = NULL;
        }
    }
    if (g_audmgr) {
        aud_pcm_config_t preferred;
        preferred.sample_rate = AC97_SAMPLE_RATE_HZ;
        preferred.channels = 2;
        preferred.format = AUD_FMT_S16_LE;

        g_ac97_output_id = g_audmgr->register_controller(AUDDEV_TYPE_OUTPUT, "ac97audio", &preferred);
        if (g_ac97_output_id < 0) {
            g_api->com_write_string(COM1_PORT, "[ac97] register_controller(\"ac97audio\") failed\n");
            g_audmgr = NULL;
        } else {
            g_api->com_write_string(COM1_PORT, "[ac97] registered with audiomanager as OUTPUT controller\n");
        }
    } else {
        g_api->com_write_string(COM1_PORT,
            "[ac97] \"audiomanager\" service not found — falling back to boot self-test tone only\n");
    }

    if (g_ac97.irq_line == 0 || g_ac97.irq_line == 0xFF) {
        g_api->com_write_string(COM1_PORT,
            "[ac97] PCI interrupt_line is unset/invalid — BIOS didn't route "
            "an IRQ for this function, can't install a handler\n");
        return -1;
    }

    g_api->irq_install_handler(g_ac97.irq_line, ac97_irq_handler);

    ac97_boot_test_tone_start();

    /* DEBUG: one final readback ~immediately after starting, of SR and
     * CIV, to see the very first hardware state post-start without even
     * needing an interrupt to have fired yet. */
    dbg_hex16("SR right after start=", g_api->inw(g_ac97.nabm_base + AC97_NABM_PO_SR));
    dbg_dec32("CIV right after start=", g_api->inb(g_ac97.nabm_base + AC97_NABM_PO_CIV));

    if (g_audmgr) {
        g_api->com_write_string(COM1_PORT,
            "[ac97] playback started — registered with audiomanager, "
            "each IRQ now pulls real frames from the ring (self-test "
            "tone plays until the first frames are pulled)\n");
    } else {
        g_api->com_write_string(COM1_PORT,
            "[ac97] boot self-test tone started (continuous, ~3kHz) — "
            "audiomanager unavailable, direct HW use only\n");
    }

    return 0;
}