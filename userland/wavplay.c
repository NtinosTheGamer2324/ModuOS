/*
 * wavplay.c — ModuOS userland WAV player
 *
 * Usage: wavplay <path/to/file.wav>
 *
 * Reads a canonical PCM WAV file (16-bit signed integer samples), opens
 * $/dev/audio/aud0, negotiates the format via AUDMGR_CMD_SET_FORMAT,
 * requests a ring via AUDMGR_CMD_MAP_RING, and streams the file into
 * the ring using the copy-batch fallback path: audmgr_ring_slot_t
 * entries are appended inline in the AUDMGR_CMD_SUBMIT packet itself
 * and written via write().
 *
 * NOTE: zero-copy mode (mmap'ing the ring via dev_mmap) is intentionally
 * not used — dev_mmap() does not currently work in this environment, so
 * this player always uses the copy-batch path regardless of what the
 * driver reports back from MAP_RING.
 *
 * Single-file build: audmgr_wire.h / types.h contents are embedded
 * inline below (Makefile can't resolve the audiosqrm/ include path),
 * so only libc.h is included externally.
 */

#include "libc.h"

/* ---------------------------------------------------------------------
 * audiosqrm/types.h and audiosqrm/audmgr_wire.h embedded directly below
 * (Makefile doesn't resolve the audiosqrm/ include path, so no external
 * headers besides libc.h are required to build this).
 * ------------------------------------------------------------------- */

#ifndef AUDIO_TYPES_H
#define AUDIO_TYPES_H

typedef enum {
    AUD_FMT_S16_LE = 1,
    AUD_FMT_S32_LE = 2,
    AUD_FMT_F32_LE = 3,
} aud_format_t;

typedef struct {
    uint32_t sample_rate;
    uint16_t channels;
    aud_format_t format;
} aud_pcm_config_t;

typedef enum {
    AUDDEV_TYPE_OUTPUT,
    AUDDEV_TYPE_INPUT,
} auddev_type_t;

#endif /* AUDIO_TYPES_H */

#ifndef AUDMGR_WIRE_H
#define AUDMGR_WIRE_H

#define AUDMGR_MAGIC        0x41554431u /* "AUD1" */
#define AUDMGR_ABI_VERSION  1u

typedef enum {
    AUDMGR_CMD_GET_INFO   = 1,
    AUDMGR_CMD_SET_FORMAT = 2,
    AUDMGR_CMD_MAP_RING   = 3,
    AUDMGR_CMD_SUBMIT     = 4,
    AUDMGR_CMD_SET_VOLUME = 5,
} audmgr_cmd_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t abi_version;
    uint32_t cmd;
    uint32_t size_bytes;
} aud_hdr_t;

typedef struct __attribute__((packed)) {
    aud_hdr_t hdr;
    aud_pcm_config_t current;
    uint32_t caps;
    char driver[32];
} aud_get_info_resp_t;

typedef struct __attribute__((packed)) {
    aud_hdr_t hdr;
    aud_pcm_config_t requested;
} aud_set_format_req_t;

#define AUDMGR_RING_FRAMES_PER_SLOT 256u
#define AUDMGR_RING_MAX_CHANNELS    2u

typedef struct __attribute__((packed)) {
    int16_t samples[AUDMGR_RING_FRAMES_PER_SLOT * AUDMGR_RING_MAX_CHANNELS];
    uint32_t frame_count;
} audmgr_ring_slot_t;

typedef struct __attribute__((packed)) {
    aud_hdr_t hdr;
    uint64_t requested_size;
} aud_map_ring_req_t;

typedef struct __attribute__((packed)) {
    aud_hdr_t hdr;
    uint64_t user_addr;
    uint64_t actual_size;
    uint32_t slot_size;
    uint32_t slot_count;
} aud_map_ring_resp_t;

typedef struct __attribute__((packed)) {
    aud_hdr_t hdr;
    uint32_t advance;
    uint32_t inline_count;
    audmgr_ring_slot_t slots[];
} aud_submit_t;

typedef struct __attribute__((packed)) {
    aud_hdr_t hdr;
    uint8_t left_atten;
    uint8_t right_atten;
    uint8_t mute;
    uint8_t _pad;
} aud_set_volume_req_t;

#define AUDMGR_OFF_RING 0x0ull

#endif /* AUDMGR_WIRE_H */

/* ---------------------------------------------------------------------
 * Minimal WAV (RIFF/WAVE, canonical "fmt "/"data" chunks) parsing.
 * We only support PCM (audioFormat == 1) 8/16/24/32-bit; everything is
 * converted to S16_LE interleaved frames before hitting the ring, since
 * that's the simplest format the wire protocol/ring slot always handles
 * cleanly regardless of what handle_set_format on the other end actually
 * wired up in hardware.
 * ------------------------------------------------------------------- */

typedef struct __attribute__((packed)) {
    char     riff[4];      /* "RIFF" */
    uint32_t riff_size;
    char     wave[4];      /* "WAVE" */
} riff_hdr_t;

typedef struct __attribute__((packed)) {
    char     id[4];
    uint32_t size;
} chunk_hdr_t;

typedef struct __attribute__((packed)) {
    uint16_t audio_format;
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
} fmt_chunk_t;

#define WAVE_FMT_PCM        1
#define WAVE_FMT_EXTENSIBLE 0xFFFE

static uint32_t rd_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd_le16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

/* ---------------------------------------------------------------------
 * audmgr transport helpers: write() a request packet, read() the
 * response packet (driver buffers exactly one response for the next
 * read(), per aud0_read/session_push_resp in audmgr_sqrm.c).
 * ------------------------------------------------------------------- */

static int audmgr_txn(int fd, const void *req, size_t req_len,
                       void *resp, size_t resp_len) {
    ssize_t w = write(fd, req, req_len);
    if (w != (ssize_t)req_len) {
        puts("wavplay: audmgr write() failed");
        return -1;
    }

    /* Response is buffered whole by the driver; loop in case the fd
     * hands it back in more than one read() (kept generic, cheap). */
    uint8_t *out = (uint8_t *)resp;
    size_t got = 0;
    int spins = 0;
    while (got < resp_len && spins < 100000) {
        ssize_t r = read(fd, out + got, resp_len - got);
        if (r > 0) {
            got += (size_t)r;
        } else {
            yield();
            spins++;
        }
    }
    if (got != resp_len) {
        puts("wavplay: audmgr read() (response) failed/short");
        return -1;
    }

    const aud_hdr_t *h = (const aud_hdr_t *)resp;
    if (h->magic != AUDMGR_MAGIC) {
        puts("wavplay: bad response magic from aud0");
        return -1;
    }
    return 0;
}

int md_main(long argc, char **argv) {
    if (argc < 2) {
        puts("usage: wavplay <file.wav>");
        return 1;
    }
    const char *path = argv[1];

    /* ---- Open + parse the WAV file ---- */
    int wfd = open(path, O_RDONLY, 0);
    if (wfd < 0) {
        printf("wavplay: cannot open %s\n", path);
        return 1;
    }

    riff_hdr_t rh;
    if (read(wfd, &rh, sizeof(rh)) != (ssize_t)sizeof(rh) ||
        memcmp(rh.riff, "RIFF", 4) != 0 || memcmp(rh.wave, "WAVE", 4) != 0) {
        puts("wavplay: not a RIFF/WAVE file");
        close(wfd);
        return 1;
    }

    fmt_chunk_t fmt;
    int have_fmt = 0;
    long data_off = -1;
    uint32_t data_size = 0;

    for (;;) {
        chunk_hdr_t ch;
        ssize_t n = read(wfd, &ch, sizeof(ch));
        if (n != (ssize_t)sizeof(ch)) break; /* EOF: done scanning chunks */

        if (memcmp(ch.id, "fmt ", 4) == 0) {
            size_t take = sizeof(fmt);
            if (ch.size < take) take = ch.size;
            memset(&fmt, 0, sizeof(fmt));
            if (read(wfd, &fmt, take) != (ssize_t)take) break;
            if (ch.size > take) lseek(wfd, (long)(ch.size - take), SEEK_CUR);
            have_fmt = 1;
        } else if (memcmp(ch.id, "data", 4) == 0) {
            data_off = lseek(wfd, 0, SEEK_CUR);
            data_size = ch.size;
            /* Stop scanning: we'll stream from here. Any chunks after
             * "data" (e.g. LIST/id3) are irrelevant to playback. */
            break;
        } else {
            lseek(wfd, (long)ch.size, SEEK_CUR);
        }

        /* RIFF chunks are word-aligned; skip the pad byte if size is odd. */
        if (ch.size & 1) lseek(wfd, 1, SEEK_CUR);
    }

    if (!have_fmt || data_off < 0) {
        puts("wavplay: missing fmt or data chunk");
        close(wfd);
        return 1;
    }
    if (fmt.audio_format != WAVE_FMT_PCM && fmt.audio_format != WAVE_FMT_EXTENSIBLE) {
        puts("wavplay: only PCM WAV is supported");
        close(wfd);
        return 1;
    }
    if (fmt.num_channels < 1 || fmt.num_channels > AUDMGR_RING_MAX_CHANNELS) {
        printf("wavplay: unsupported channel count %d (max %d)\n",
               (int)fmt.num_channels, (int)AUDMGR_RING_MAX_CHANNELS);
        close(wfd);
        return 1;
    }
    if (fmt.bits_per_sample != 8 && fmt.bits_per_sample != 16 &&
        fmt.bits_per_sample != 24 && fmt.bits_per_sample != 32) {
        printf("wavplay: unsupported bit depth %d\n", (int)fmt.bits_per_sample);
        close(wfd);
        return 1;
    }

    printf("wavplay: %s — %dHz, %dch, %d-bit, %u bytes of data\n",
           path, (int)fmt.sample_rate, (int)fmt.num_channels,
           (int)fmt.bits_per_sample, data_size);

    lseek(wfd, data_off, SEEK_SET);

    /* ---- Open audmgr and negotiate ---- */
    int afd = open("$/dev/audio/aud0", O_RDWR, 0);
    if (afd < 0) {
        puts("wavplay: cannot open $/dev/audio/aud0");
        close(wfd);
        return 1;
    }

    aud_set_format_req_t sfreq;
    memset(&sfreq, 0, sizeof(sfreq));
    sfreq.hdr.magic = AUDMGR_MAGIC;
    sfreq.hdr.abi_version = AUDMGR_ABI_VERSION;
    sfreq.hdr.cmd = AUDMGR_CMD_SET_FORMAT;
    sfreq.hdr.size_bytes = sizeof(sfreq);
    sfreq.requested.sample_rate = fmt.sample_rate;
    sfreq.requested.channels = fmt.num_channels;
    sfreq.requested.format = AUD_FMT_S16_LE; /* we convert everything to this */

    aud_hdr_t sfresp;
    if (audmgr_txn(afd, &sfreq, sizeof(sfreq), &sfresp, sizeof(sfresp)) != 0) {
        close(afd); close(wfd);
        return 1;
    }

    aud_map_ring_req_t mrreq;
    memset(&mrreq, 0, sizeof(mrreq));
    mrreq.hdr.magic = AUDMGR_MAGIC;
    mrreq.hdr.abi_version = AUDMGR_ABI_VERSION;
    mrreq.hdr.cmd = AUDMGR_CMD_MAP_RING;
    mrreq.hdr.size_bytes = sizeof(mrreq);
    mrreq.requested_size = 0; /* server default slot count */

    aud_map_ring_resp_t mrresp;
    if (audmgr_txn(afd, &mrreq, sizeof(mrreq), &mrresp, sizeof(mrresp)) != 0) {
        close(afd); close(wfd);
        return 1;
    }
    if (mrresp.slot_size != sizeof(audmgr_ring_slot_t) || mrresp.slot_count == 0) {
        puts("wavplay: MAP_RING gave an unusable ring");
        close(afd); close(wfd);
        return 1;
    }

    /* Zero-copy mode (mmap'ing the ring via dev_mmap) is intentionally
     * not used here — dev_mmap() does not currently work in this
     * environment. Always use the copy-batch SUBMIT path, regardless of
     * whether the driver handed back a non-zero user_addr in mrresp. */
    printf("wavplay: ring = %u slots, mode = copy-batch\n", mrresp.slot_count);

    /* ---- Streaming loop ----
     * Read raw file bytes into a scratch buffer sized to exactly one
     * ring slot's worth of frames, convert to interleaved S16_LE, hand
     * the slot to the driver inline in a SUBMIT packet, repeat until EOF.
     */
    const uint32_t channels = fmt.num_channels;
    const uint32_t bytes_per_sample = fmt.bits_per_sample / 8;
    const uint32_t src_frame_bytes = bytes_per_sample * channels;
    const uint32_t frames_per_slot = AUDMGR_RING_FRAMES_PER_SLOT;

    uint8_t *raw = (uint8_t *)malloc((size_t)frames_per_slot * src_frame_bytes);
    if (!raw) {
        puts("wavplay: out of memory");
        close(afd); close(wfd);
        return 1;
    }

    uint32_t bytes_left = data_size;

    /* Pacing: without this the whole file gets pushed into the ring in
     * a fraction of a second, lapping ring_head before the hardware IRQ
     * ever pulls it — audio silently gets overwritten and you hear
     * nothing. Sleep roughly one slot's worth of real playback time
     * between submits, based on time_ms(). */
    const uint32_t slot_ms = (fmt.sample_rate > 0)
        ? (frames_per_slot * 1000u) / fmt.sample_rate
        : 5u;
    uint64_t next_deadline = time_ms();

    /* copy-batch SUBMIT packet: header + advance + inline_count + 1 slot */
    size_t submit_pkt_sz = sizeof(aud_submit_t) + sizeof(audmgr_ring_slot_t);
    aud_submit_t *submit_pkt = (aud_submit_t *)malloc(submit_pkt_sz);
    if (!submit_pkt) {
        puts("wavplay: out of memory");
        free(raw);
        close(afd); close(wfd);
        return 1;
    }

    while (bytes_left > 0) {
        uint32_t want_frames = frames_per_slot;
        uint32_t want_bytes = want_frames * src_frame_bytes;
        if (want_bytes > bytes_left) {
            want_bytes = bytes_left - (bytes_left % src_frame_bytes);
            want_frames = want_bytes / src_frame_bytes;
        }
        if (want_frames == 0) break;

        ssize_t got = read(wfd, raw, want_bytes);
        if (got <= 0) break;
        uint32_t got_frames = (uint32_t)got / src_frame_bytes;
        if (got_frames == 0) break;
        bytes_left -= (uint32_t)got;

        audmgr_ring_slot_t slot;
        memset(&slot, 0, sizeof(slot));
        slot.frame_count = got_frames;

        /* Convert to interleaved S16_LE, up-mixing/down-mixing channel
         * count 1:1 (mono stays mono in slot ch0, stereo fills both). */
        for (uint32_t f = 0; f < got_frames; f++) {
            for (uint32_t c = 0; c < channels && c < AUDMGR_RING_MAX_CHANNELS; c++) {
                const uint8_t *sp = raw + (size_t)f * src_frame_bytes + (size_t)c * bytes_per_sample;
                int32_t sample;
                if (bytes_per_sample == 1) {
                    sample = ((int32_t)sp[0] - 128) << 8; /* WAV 8-bit is unsigned */
                } else if (bytes_per_sample == 2) {
                    sample = (int16_t)rd_le16(sp);
                } else if (bytes_per_sample == 3) {
                    int32_t v = sp[0] | (sp[1] << 8) | (sp[2] << 16);
                    if (v & 0x800000) v |= 0xFF000000; /* sign-extend 24->32 */
                    sample = v >> 8; /* scale 24-bit to 16-bit */
                } else { /* 32-bit */
                    int32_t v = (int32_t)rd_le32(sp);
                    sample = v >> 16; /* scale 32-bit to 16-bit */
                }
                if (sample > 32767) sample = 32767;
                if (sample < -32768) sample = -32768;
                slot.samples[f * AUDMGR_RING_MAX_CHANNELS + c] = (int16_t)sample;
            }
            /* mono source: duplicate ch0 into ch1 so stereo hardware
             * still gets audio on both channels. */
            if (channels == 1) {
                slot.samples[f * AUDMGR_RING_MAX_CHANNELS + 1] =
                    slot.samples[f * AUDMGR_RING_MAX_CHANNELS + 0];
            }
        }

        /* Wait until this slot's playback time has actually elapsed
         * before handing it over, so we stay roughly one slot ahead of
         * the hardware instead of lapping it. */
        next_deadline += slot_ms;
        while (time_ms() < next_deadline) {
            yield();
        }

        submit_pkt->hdr.magic = AUDMGR_MAGIC;
        submit_pkt->hdr.abi_version = AUDMGR_ABI_VERSION;
        submit_pkt->hdr.cmd = AUDMGR_CMD_SUBMIT;
        submit_pkt->hdr.size_bytes = (uint32_t)submit_pkt_sz;
        submit_pkt->advance = 0;
        submit_pkt->inline_count = 1;
        submit_pkt->slots[0] = slot;
        if (write(afd, submit_pkt, submit_pkt_sz) != (ssize_t)submit_pkt_sz) {
            puts("wavplay: SUBMIT write failed");
            break;
        }
    }

    puts("wavplay: done");

    free(submit_pkt);
    free(raw);
    close(afd);
    close(wfd);
    return 0;
}