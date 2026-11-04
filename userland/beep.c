/*
 * beep.c — ModuOS userland audio subsystem test tone
 *
 * Usage: beep [freq_hz] [duration_sec]
 *   defaults: 1000 Hz for 2.0 seconds
 *
 * Deliberately bypasses WAV parsing entirely: generates a square wave
 * directly in memory and pushes it through the exact same audmgr wire
 * path wavplay.c uses (SET_FORMAT -> MAP_RING -> copy-batch SUBMIT).
 * If this plays cleanly but wavplay doesn't, the bug is in wavplay's
 * WAV parsing/PCM conversion. If this ALSO fails/glitches, the bug is
 * further down in audmgr_sqrm.c or the ac97 driver.
 *
 * Same as wavplay.c: zero-copy mmap ring mode is intentionally not
 * used (dev_mmap doesn't currently work here) — always copy-batch.
 *
 * Single-file build: audmgr_wire.h / types.h embedded inline below,
 * same as wavplay.c, so only libc.h is included externally.
 */

#include "libc/libc.h"

/* ---------------------------------------------------------------------
 * audiosqrm/types.h and audiosqrm/audmgr_wire.h embedded directly below,
 * identical to wavplay.c's copies.
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
 * audmgr transport helper — identical to wavplay.c's audmgr_txn().
 * ------------------------------------------------------------------- */

static int audmgr_txn(int fd, const void *req, size_t req_len,
                       void *resp, size_t resp_len) {
    ssize_t w = write(fd, req, req_len);
    if (w != (ssize_t)req_len) {
        puts("beep: audmgr write() failed");
        return -1;
    }

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
        puts("beep: audmgr read() (response) failed/short");
        return -1;
    }

    const aud_hdr_t *h = (const aud_hdr_t *)resp;
    if (h->magic != AUDMGR_MAGIC) {
        puts("beep: bad response magic from aud0");
        return -1;
    }
    return 0;
}

/* Simple integer string -> long parser (no atoi/strtol assumed in libc.h). */
static long parse_long(const char *s, long dflt) {
    if (!s || !*s) return dflt;
    int neg = 0;
    long v = 0;
    if (*s == '-') { neg = 1; s++; }
    if (!*s) return dflt;
    while (*s) {
        if (*s < '0' || *s > '9') return dflt;
        v = v * 10 + (*s - '0');
        s++;
    }
    return neg ? -v : v;
}

#define BEEP_SAMPLE_RATE_HZ 48000u
#define BEEP_CHANNELS       2u
#define BEEP_AMPLITUDE      12000

int md_main(long argc, char **argv) {
    long freq_hz = (argc >= 2) ? parse_long(argv[1], 1000) : 1000;
    long duration_x10 = (argc >= 3) ? parse_long(argv[2], 20) : 20; /* unused path kept simple below */
    (void)duration_x10;
    double duration_sec = (argc >= 3) ? (double)parse_long(argv[2], 2) : 2.0;

    if (freq_hz < 20 || freq_hz > 20000) {
        printf("beep: freq %ld Hz out of human-audible range, clamping to 1000\n", freq_hz);
        freq_hz = 1000;
    }
    if (duration_sec <= 0.0) duration_sec = 2.0;

    printf("beep: generating %ld Hz square wave for %.1fs (bypasses WAV parsing entirely)\n",
           freq_hz, duration_sec);

    /* ---- Open audmgr and negotiate ---- */
    int afd = open("$/dev/audio/aud0", O_RDWR, 0);
    if (afd < 0) {
        puts("beep: cannot open $/dev/audio/aud0");
        return 1;
    }

    aud_set_format_req_t sfreq;
    memset(&sfreq, 0, sizeof(sfreq));
    sfreq.hdr.magic = AUDMGR_MAGIC;
    sfreq.hdr.abi_version = AUDMGR_ABI_VERSION;
    sfreq.hdr.cmd = AUDMGR_CMD_SET_FORMAT;
    sfreq.hdr.size_bytes = sizeof(sfreq);
    sfreq.requested.sample_rate = BEEP_SAMPLE_RATE_HZ;
    sfreq.requested.channels = BEEP_CHANNELS;
    sfreq.requested.format = AUD_FMT_S16_LE;

    aud_hdr_t sfresp;
    if (audmgr_txn(afd, &sfreq, sizeof(sfreq), &sfresp, sizeof(sfresp)) != 0) {
        close(afd);
        return 1;
    }

    aud_map_ring_req_t mrreq;
    memset(&mrreq, 0, sizeof(mrreq));
    mrreq.hdr.magic = AUDMGR_MAGIC;
    mrreq.hdr.abi_version = AUDMGR_ABI_VERSION;
    mrreq.hdr.cmd = AUDMGR_CMD_MAP_RING;
    mrreq.hdr.size_bytes = sizeof(mrreq);
    mrreq.requested_size = 0;

    aud_map_ring_resp_t mrresp;
    if (audmgr_txn(afd, &mrreq, sizeof(mrreq), &mrresp, sizeof(mrresp)) != 0) {
        close(afd);
        return 1;
    }
    if (mrresp.slot_size != sizeof(audmgr_ring_slot_t) || mrresp.slot_count == 0) {
        puts("beep: MAP_RING gave an unusable ring");
        close(afd);
        return 1;
    }

    /* Same as wavplay.c: dev_mmap doesn't work here, always copy-batch. */
    printf("beep: ring = %u slots, mode = copy-batch\n", mrresp.slot_count);

    const uint32_t frames_per_slot = AUDMGR_RING_FRAMES_PER_SLOT;
    const uint32_t total_frames = (uint32_t)(duration_sec * (double)BEEP_SAMPLE_RATE_HZ);

    /* Square wave: half-period in samples, deterministic phase so it's
     * continuous and glitch-free across slot boundaries (same trick
     * ac97_audio.c's boot self-test tone uses). */
    uint32_t half_period = BEEP_SAMPLE_RATE_HZ / ((uint32_t)freq_hz * 2u);
    if (half_period == 0) half_period = 1;

    size_t submit_pkt_sz = sizeof(aud_submit_t) + sizeof(audmgr_ring_slot_t);
    aud_submit_t *submit_pkt = (aud_submit_t *)malloc(submit_pkt_sz);
    if (!submit_pkt) {
        puts("beep: out of memory");
        close(afd);
        return 1;
    }

    /* Pacing: same reasoning as wavplay.c — without it we push the whole
     * tone into the ring far faster than hardware drains it and lap
     * ring_head, so audio gets silently overwritten. */
    const uint32_t slot_ms = (frames_per_slot * 1000u) / BEEP_SAMPLE_RATE_HZ;
    uint64_t next_deadline = time_ms();

    uint32_t frames_sent = 0;
    uint64_t phase_index = 0; /* running sample index, for continuous phase */

    while (frames_sent < total_frames) {
        uint32_t this_frames = frames_per_slot;
        if (frames_sent + this_frames > total_frames)
            this_frames = total_frames - frames_sent;

        audmgr_ring_slot_t slot;
        memset(&slot, 0, sizeof(slot));
        slot.frame_count = this_frames;

        for (uint32_t f = 0; f < this_frames; f++) {
            int16_t v = (((phase_index + f) / half_period) % 2 == 0)
                            ? BEEP_AMPLITUDE : -BEEP_AMPLITUDE;
            slot.samples[f * AUDMGR_RING_MAX_CHANNELS + 0] = v;
            slot.samples[f * AUDMGR_RING_MAX_CHANNELS + 1] = v;
        }
        phase_index += this_frames;
        frames_sent += this_frames;

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
            puts("beep: SUBMIT write failed");
            break;
        }
    }

    puts("beep: done");

    free(submit_pkt);
    close(afd);
    return 0;
}