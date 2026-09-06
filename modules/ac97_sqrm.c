/*
 * ac97_audio.c — AC'97 driver, registered as SQRM_TYPE_AUDIO.
 *
 * Uses "moduos/kernel/sqrm.h" (the in-tree kernel header), not the
 * third-party sqrm_sdk.h — this gives us the real pci_device_t layout
 * instead of an opaque void*, which was the actual bug last time: the
 * previous version read PCI config space at bus/dev/func = 0,0,0 instead
 * of the AC'97 controller's real location, so every register access was
 * hitting the wrong device.
 *
 * Registers itself with the "audiomanager" service (see contract.h) as
 * an OUTPUT controller. Once registered, each playback IRQ refills the
 * BDL entry that just finished by pulling fresh frames from the audio
 * manager's ring via audio_api_t::pull_output_frames, instead of just
 * looping the fixed boot self-test tone forever. If "audiomanager"
 * isn't present at init time (module load order, or it's simply not
 * loaded), this falls back to the original direct-hardware, tone-only
 * behavior rather than failing to load.
 */

#include "moduos/kernel/sqrm.h"
#include "audiosqrm/contract.h"
#include <stdint.h>
#include <stddef.h>

const sqrm_kernel_api_t *g_api;

#define COM1_PORT 0x3F8

static const char *deps[] = { "audiomanager" };
SQRM_DEFINE_MODULE_V2(SQRM_TYPE_AUDIO, "ac97", 2, 1, 1, deps);

/* Intel 82801 (ICH) AC'97 Audio Controller — classic reference ID.
 * If your target is a different chipset, this is the value to change. */
#define AC97_PCI_VENDOR_INTEL   0x8086
#define AC97_PCI_DEVICE_ICH     0x2415

/* ===================== AC'97 register offsets ===================== */

/* NAM = Native Audio Mixer (codec registers), relative to NAMBAR */
#define AC97_NAM_RESET          0x00
#define AC97_NAM_MASTER_VOL     0x02
#define AC97_NAM_PCM_OUT_VOL    0x18

/* NABM = Native Audio Bus Master (controller registers), relative to NABMBAR */
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

/* NULL until (and unless) "audiomanager" is found at init; the IRQ
 * handler checks this on every interrupt to decide whether to pull real
 * frames or keep looping the self-test tone. */
static const audio_api_t *g_audmgr = NULL;
static int g_ac97_output_id = -1;

/* ===================== real PCI discovery ===================== */

static int ac97_pci_find_and_map(void) {
    /*
    pci_device_t *dev = g_api->pci_find_device(AC97_PCI_VENDOR_INTEL, AC97_PCI_DEVICE_ICH);
    if (!dev) {
        g_api->com_write_string(COM1_PORT, "[ac97] no matching PCI AC'97 controller found\n");
        return -1;
    }
    g_ac97.pci_dev = dev;
    */

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

    /* dev->bar[0]/[1] are already parsed by the kernel's PCI scan — no
     * need to re-read config space ourselves. bar_type[i] == PCI_BAR_IO
     * (1) confirms these are I/O-space BARs, matching a legacy AC'97
     * controller's NAMBAR/NABMBAR layout. */
    if (dev->bar_type[0] != PCI_BAR_IO || dev->bar_type[1] != PCI_BAR_IO) {
        g_api->com_write_string(COM1_PORT,
            "[ac97] BAR0/BAR1 aren't both I/O-space — unexpected layout, bailing\n");
        return -1;
    }

    g_ac97.nam_base  = (uint16_t)(dev->bar[0] & 0xFFFC);
    g_ac97.nabm_base = (uint16_t)(dev->bar[1] & 0xFFFC);
    g_ac97.irq_line  = dev->interrupt_line;

    return 0;
}

/* ===================== codec bring-up ===================== */

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

/* ===================== tone generation ===================== */

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

/* Bytes per BDL entry's own segment of g_pcm_dma. Each entry now gets a
 * distinct segment (rather than all AC97_BDL_ENTRIES entries pointing at
 * one shared buffer) so the IRQ handler can refill exactly the segment
 * hardware just finished playing without disturbing the others. */
#define AC97_SEGMENT_BYTES ((size_t)AC97_SAMPLES_PER_ENTRY * 2 * sizeof(int16_t))

static int16_t *ac97_segment(uint8_t idx) {
    return (int16_t *)((uint8_t *)g_pcm_dma.virt + (size_t)idx * AC97_SEGMENT_BYTES);
}

/* Refill one BDL segment: pull real frames from the audio manager if
 * we're registered with it, otherwise keep regenerating the boot
 * self-test tone (same deterministic waveform each call, so looping
 * this way is seamless — no phase discontinuity at segment boundaries). */
static void ac97_refill_segment(uint8_t idx) {
    int16_t *seg = ac97_segment(idx);
    if (g_audmgr) {
        g_audmgr->pull_output_frames(seg, AC97_SAMPLES_PER_ENTRY);
    } else {
        ac97_fill_tone_buffer(seg, AC97_SAMPLES_PER_ENTRY);
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
        /* g_audmgr isn't registered yet at this point in init, so this
         * always seeds with the self-test tone; segments get pulled from
         * the audio manager (once registered) starting with the first
         * IRQ-driven refill. */
        ac97_fill_tone_buffer(ac97_segment((uint8_t)i), AC97_SAMPLES_PER_ENTRY);

        bdl[i].buf_addr    = (uint32_t)(g_pcm_dma.phys + (uint64_t)i * AC97_SEGMENT_BYTES);
        bdl[i].num_samples = AC97_SAMPLES_PER_ENTRY * 2;
        bdl[i].flags       = AC97_BDL_FLAG_IOC;
    }

    g_api->outl(g_ac97.nabm_base + AC97_NABM_PO_BDBAR, (uint32_t)g_bdl_dma.phys);
    g_api->outb(g_ac97.nabm_base + AC97_NABM_PO_LVI, AC97_BDL_ENTRIES - 1);

    return 0;
}

/* ===================== keep it playing forever ===================== */

static void ac97_irq_handler(void) {
    uint16_t sr = g_api->inw(g_ac97.nabm_base + AC97_NABM_PO_SR);
    if (sr & (AC97_SR_BCIS | AC97_SR_LVBCI)) {
        g_api->outw(g_ac97.nabm_base + AC97_NABM_PO_SR, sr);
        uint8_t civ = g_api->inb(g_ac97.nabm_base + AC97_NABM_PO_CIV);
        /* civ - 1 (mod N) is the descriptor hardware just moved past —
         * it's done with that buffer, so it's safe to refill here. This
         * is the same index the original code computed to extend LVI
         * and keep the loop going forever; now we also rewrite its
         * contents before re-extending into it.
         * pull_output_frames() runs in IRQ context per contract.h — it
         * must not (and doesn't) allocate or block. */
        uint8_t finished = (uint8_t)((civ + AC97_BDL_ENTRIES - 1) % AC97_BDL_ENTRIES);
        ac97_refill_segment(finished);
        g_api->outb(g_ac97.nabm_base + AC97_NABM_PO_LVI, finished);
    }
    g_api->pic_send_eoi(g_ac97.irq_line);
}

static void ac97_boot_test_tone_start(void) {
    g_api->outb(g_ac97.nabm_base + AC97_NABM_PO_CR, AC97_CR_RPBM | AC97_CR_IOCE);
}

/* ===================== module entry point ===================== */

int sqrm_module_init(const sqrm_kernel_api_t *api) {
    if (!api) return -1;
    g_api = api;

    g_api->com_write_string(COM1_PORT, "[ac97] TYPE_AUDIO module loading\n");

    if (ac97_pci_find_and_map() != 0) {
        return -1;
    }

    uint32_t sta = g_api->inl(g_ac97.nabm_base + AC97_NABM_GLOB_STA);
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
            /* audiomanager registered a smaller/older blob than the
             * audio_api_t we're linked against — treat as absent rather
             * than risk calling through a function pointer past the end
             * of what it actually published. */
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