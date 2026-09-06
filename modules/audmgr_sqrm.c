/*
 * audmgr_sqrm.c — ModuOS Audio Manager — SQRM module
 *
 * Two faces:
 *   1. In-kernel service ("audiomanager", see contract.h) — hardware
 *      drivers (e.g. ac97audio) register themselves and pull frames.
 *   2. $/dev/audio/aud0 devfs node — userland opens it, negotiates a
 *      format, maps a zero-copy ring (like mvi0's MAP_RING), and writes
 *      PCM frames into ring slots for the active OUTPUT controller to
 *      consume.
 *
 * Modeled on inputmanager (service registration pattern) and mvc3_sqrm.c
 * (devfs session/ring/packet-framing pattern). The wire format is its own
 * (see audmgr_wire.h) rather than a guess at mvc3.h's actual layout.
 */

#include "moduos/kernel/sqrm.h"
#include "moduos/fs/devfs.h"
#include "audiosqrm/contract.h"
#include "audiosqrm/audmgr_wire.h"
#include <stdint.h>
#include <stddef.h>

SQRM_DEFINE_MODULE(SQRM_TYPE_AUDIO, "audiomanager");

/* ── Config ────────────────────────────────────────────────────────── */

#define COM1_PORT 0x3F8

#define AUDMGR_MAX_CONTROLLERS   8
#define AUDMGR_MAX_SESSIONS      4
#define AUDMGR_DEFAULT_RING_SLOTS 64u   /* * sizeof(audmgr_ring_slot_t) */
#define AUDMGR_MAX_RING_SLOTS     512u

/* ── Globals ───────────────────────────────────────────────────────── */

static const sqrm_kernel_api_t *g_api;

static audio_controller_t g_controllers[AUDMGR_MAX_CONTROLLERS];
static int g_next_controller = 0;
static int g_active_output_id = -1; /* first registered OUTPUT controller, for now */

/* ── Logging ───────────────────────────────────────────────────────── */

static void log_(const char *s) {
    g_api->com_write_string(COM1_PORT, "[audmgr] ");
    g_api->com_write_string(COM1_PORT, s);
    g_api->com_write_string(COM1_PORT, "\n");
}

/* ── libc-lite ─────────────────────────────────────────────────────── */

static int strcmp_(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) { ++s1; ++s2; }
    return *(unsigned char *)s1 - *(unsigned char *)s2;
}

static void *memset_(void *dst, int c, size_t n) {
    unsigned char *p = (unsigned char *)dst;
    while (n--) *p++ = (unsigned char)c;
    return dst;
}

static void *memcpy_(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
    return dst;
}

/* ── Controller registry (kernel-side service) ───────────────────────
 *
 * Mirrors inputmanager's register_controller/remove_controller: a flat
 * table, name comparison for removal (not pointer comparison, same
 * reasoning as inputmanager — two different pointers can hold the same
 * string), and a bounds check performed before any write. */

static int register_controller(auddev_type_t type, const char *name,
                               const aud_pcm_config_t *preferred) {
    if (g_next_controller >= AUDMGR_MAX_CONTROLLERS) {
        return -aeoutofspace;
    }
    if (!name) {
        return -aeinvalinfo;
    }

    int id = g_next_controller;
    g_controllers[id].id = id;
    g_controllers[id].name = name;
    g_controllers[id].type = type;
    if (preferred) {
        g_controllers[id].preferred = *preferred;
    } else {
        g_controllers[id].preferred.sample_rate = 48000;
        g_controllers[id].preferred.channels = 2;
        g_controllers[id].preferred.format = AUD_FMT_S16_LE;
    }
    g_next_controller++;

    if (type == AUDDEV_TYPE_OUTPUT && g_active_output_id < 0) {
        g_active_output_id = id;
        log_("first OUTPUT controller registered, made active");
    }

    return id;
}

static int remove_controller(int id, const char *name) {
    if (id < 0 || id >= AUDMGR_MAX_CONTROLLERS) {
        log_("remove_controller: id out of range");
        return -aeinvalinfo;
    }
    if (g_controllers[id].name == NULL || !name || strcmp_(name, g_controllers[id].name) != 0) {
        log_("remove_controller: name mismatch");
        return -aeinvalinfo;
    }

    audio_controller_t nall = {0};
    g_controllers[id] = nall;

    if (g_active_output_id == id) {
        g_active_output_id = -1;
        for (int i = 0; i < AUDMGR_MAX_CONTROLLERS; i++) {
            if (g_controllers[i].name && g_controllers[i].type == AUDDEV_TYPE_OUTPUT) {
                g_active_output_id = i;
                break;
            }
        }
    }

    return aesuccess;
}

/* ── Session state (devfs side) ───────────────────────────────────────
 *
 * One session per open() on $/dev/audio/aud0, same shape as mvc3's
 * mvc3_session_t: a small response buffer for the next read(), a packet
 * accumulation buffer for write(), and ring state. */

typedef struct {
    int active;

    uint8_t  resp_buf[256];
    uint32_t resp_len;
    uint32_t resp_off;

    uint8_t  pkt_buf[4096];
    uint32_t pkt_len;

    audmgr_ring_slot_t *ring;
    uint32_t ring_slot_count;
    uint32_t ring_head;      /* next slot the OUTPUT controller will consume */
    uint32_t ring_write_pos; /* next slot userland should fill (tracked here
                              * for the copy-batch fallback path; in
                              * zero-copy mode userland tracks its own
                              * write position and just tells us how far
                              * to advance via SUBMIT.advance) */
    uint64_t ring_user_va;
    int      ring_is_mapped;

    aud_pcm_config_t format;
} audmgr_session_t;

static audmgr_session_t g_sessions[AUDMGR_MAX_SESSIONS];

/* The session currently feeding the active output controller. Simplification:
 * single active playback session, same as inputmanager's single global
 * queue — a future revision could mix multiple sessions, this one doesn't. */
static audmgr_session_t *g_playback_session = NULL;

static audmgr_session_t *session_alloc(void) {
    for (int i = 0; i < AUDMGR_MAX_SESSIONS; i++) {
        if (!g_sessions[i].active) {
            memset_(&g_sessions[i], 0, sizeof(g_sessions[i]));
            g_sessions[i].active = 1;
            g_sessions[i].format.sample_rate = 48000;
            g_sessions[i].format.channels = 2;
            g_sessions[i].format.format = AUD_FMT_S16_LE;
            return &g_sessions[i];
        }
    }
    return NULL;
}

static void session_free(audmgr_session_t *s) {
    if (!s) return;
    if (s->ring && !s->ring_is_mapped) {
        g_api->kfree(s->ring);
    }
    if (g_playback_session == s) {
        g_playback_session = NULL;
    }
    memset_(s, 0, sizeof(*s));
}

static void session_push_resp(audmgr_session_t *s, const void *data, uint32_t len) {
    if (len > sizeof(s->resp_buf)) len = sizeof(s->resp_buf);
    memcpy_(s->resp_buf, data, len);
    s->resp_len = len;
    s->resp_off = 0;
}

/* ── Command handlers ──────────────────────────────────────────────── */

static void handle_get_info(audmgr_session_t *s) {
    aud_get_info_resp_t resp;
    memset_(&resp, 0, sizeof(resp));
    resp.hdr.magic = AUDMGR_MAGIC;
    resp.hdr.abi_version = AUDMGR_ABI_VERSION;
    resp.hdr.cmd = AUDMGR_CMD_GET_INFO;
    resp.hdr.size_bytes = sizeof(resp);
    resp.current = s->format;

    const char *drv = "audiomanager";
    if (g_active_output_id >= 0 && g_controllers[g_active_output_id].name) {
        drv = g_controllers[g_active_output_id].name;
    }
    uint32_t di = 0;
    while (drv[di] && di + 1 < sizeof(resp.driver)) { resp.driver[di] = drv[di]; di++; }
    resp.driver[di] = 0;

    session_push_resp(s, &resp, sizeof(resp));
}

static void handle_set_format(audmgr_session_t *s, const aud_set_format_req_t *req) {
    s->format = req->requested;

    aud_hdr_t resp;
    resp.magic = AUDMGR_MAGIC;
    resp.abi_version = AUDMGR_ABI_VERSION;
    resp.cmd = AUDMGR_CMD_SET_FORMAT;
    resp.size_bytes = sizeof(resp);
    session_push_resp(s, &resp, sizeof(resp));
}

static void handle_map_ring(audmgr_session_t *s, const aud_map_ring_req_t *req) {
    aud_map_ring_resp_t resp;
    memset_(&resp, 0, sizeof(resp));
    resp.hdr.magic = AUDMGR_MAGIC;
    resp.hdr.abi_version = AUDMGR_ABI_VERSION;
    resp.hdr.cmd = AUDMGR_CMD_MAP_RING;
    resp.hdr.size_bytes = sizeof(resp);
    resp.slot_size = sizeof(audmgr_ring_slot_t);

    if (s->ring) {
        if (!s->ring_is_mapped) g_api->kfree(s->ring);
        s->ring = NULL;
        s->ring_is_mapped = 0;
        s->ring_slot_count = 0;
        s->ring_head = 0;
        s->ring_write_pos = 0;
        s->ring_user_va = 0;
    }

    uint32_t want_slots = AUDMGR_DEFAULT_RING_SLOTS;
    if (req->requested_size >= sizeof(audmgr_ring_slot_t)) {
        want_slots = (uint32_t)(req->requested_size / sizeof(audmgr_ring_slot_t));
    }
    if (want_slots > AUDMGR_MAX_RING_SLOTS) want_slots = AUDMGR_MAX_RING_SLOTS;
    if (want_slots == 0) want_slots = 1;

    size_t bytes = (size_t)want_slots * sizeof(audmgr_ring_slot_t);
    s->ring = (audmgr_ring_slot_t *)g_api->kmalloc(bytes);
    if (!s->ring) {
        log_("MAP_RING: kmalloc failed");
        session_push_resp(s, &resp, sizeof(resp));
        return;
    }
    memset_(s->ring, 0, bytes);
    s->ring_slot_count = want_slots;
    s->ring_head = 0;
    s->ring_write_pos = 0;

    if (g_api->devfs_mmap_region) {
        void *uva = g_api->devfs_mmap_region((uint64_t)(uintptr_t)s->ring, bytes, 3 /*R|W*/, 0 /*virt*/);
        if (uva != (void *)-1) {
            s->ring_user_va = (uint64_t)(uintptr_t)uva;
            s->ring_is_mapped = 1;
            resp.user_addr = s->ring_user_va;
            resp.actual_size = bytes;
            resp.slot_count = want_slots;
            log_("MAP_RING: zero-copy ring mapped into userland");
        } else {
            resp.user_addr = 0;
            resp.actual_size = bytes;
            resp.slot_count = want_slots;
            log_("MAP_RING: mmap failed, copy-batch fallback available via SUBMIT");
        }
    } else {
        resp.user_addr = 0;
        resp.actual_size = bytes;
        resp.slot_count = want_slots;
        log_("MAP_RING: devfs_mmap_region unavailable, copy-batch fallback only");
    }

    /* First mapped-ring session becomes the playback source. Same
     * single-active-session simplification as g_active_output_id. */
    if (!g_playback_session) {
        g_playback_session = s;
    }

    session_push_resp(s, &resp, sizeof(resp));
}

static void handle_submit(audmgr_session_t *s, const aud_submit_t *sub, uint32_t consumed) {
    if (!s->ring) return;

    if (s->ring_is_mapped) {
        /* Zero-copy mode: userland already wrote directly into the mapped
         * ring. We only need to record how far to advance the write
         * cursor; the OUTPUT controller's pull consumes from ring_head
         * up to ring_write_pos. */
        uint32_t adv = sub->advance;
        if (adv > s->ring_slot_count) adv = s->ring_slot_count;
        s->ring_write_pos = (s->ring_write_pos + adv) % s->ring_slot_count;
        return;
    }

    /* Copy-batch fallback: slots were appended inline in the packet. */
    uint32_t avail_bytes = consumed - sizeof(aud_hdr_t) - sizeof(uint32_t) * 2;
    uint32_t max_inline = (uint32_t)(avail_bytes / sizeof(audmgr_ring_slot_t));
    uint32_t n = sub->inline_count;
    if (n > max_inline) n = max_inline;

    for (uint32_t i = 0; i < n; i++) {
        uint32_t idx = (s->ring_write_pos + i) % s->ring_slot_count;
        s->ring[idx] = sub->slots[i];
    }
    s->ring_write_pos = (s->ring_write_pos + n) % s->ring_slot_count;

    if (!g_playback_session) {
        g_playback_session = s;
    }
}

static void handle_set_volume(audmgr_session_t *s, const aud_set_volume_req_t *req) {
    (void)s;
    /* No generic "set master volume" hook exists on audio_api_t yet — a
     * hardware-specific call would go through the active controller.
     * Left as a log line rather than silently doing nothing, so it's
     * obvious this needs wiring up once a controller exposes a volume
     * hook in contract.h. */
    log_("SET_VOLUME: no controller-side volume hook wired yet, ignored");
    (void)req;

    aud_hdr_t resp;
    resp.magic = AUDMGR_MAGIC;
    resp.abi_version = AUDMGR_ABI_VERSION;
    resp.cmd = AUDMGR_CMD_SET_VOLUME;
    resp.size_bytes = sizeof(resp);
    session_push_resp(s, &resp, sizeof(resp));
}

/* ── Packet processor ──────────────────────────────────────────────── */

static uint32_t process_packet(audmgr_session_t *s) {
    if (s->pkt_len < sizeof(aud_hdr_t)) return 0;
    const aud_hdr_t *hdr = (const aud_hdr_t *)s->pkt_buf;
    if (hdr->magic != AUDMGR_MAGIC) return 1; /* desync: drop one byte and resync */
    if (hdr->size_bytes < sizeof(aud_hdr_t)) return 1;
    if (s->pkt_len < hdr->size_bytes) return 0; /* wait for more data */

    uint32_t consumed = hdr->size_bytes;
    switch ((audmgr_cmd_t)hdr->cmd) {
    case AUDMGR_CMD_GET_INFO:
        handle_get_info(s);
        break;
    case AUDMGR_CMD_SET_FORMAT:
        if (consumed >= sizeof(aud_set_format_req_t))
            handle_set_format(s, (const aud_set_format_req_t *)s->pkt_buf);
        break;
    case AUDMGR_CMD_MAP_RING:
        if (consumed >= sizeof(aud_map_ring_req_t))
            handle_map_ring(s, (const aud_map_ring_req_t *)s->pkt_buf);
        break;
    case AUDMGR_CMD_SUBMIT:
        if (consumed >= sizeof(aud_submit_t))
            handle_submit(s, (const aud_submit_t *)s->pkt_buf, consumed);
        break;
    case AUDMGR_CMD_SET_VOLUME:
        if (consumed >= sizeof(aud_set_volume_req_t))
            handle_set_volume(s, (const aud_set_volume_req_t *)s->pkt_buf);
        break;
    default:
        break;
    }
    return consumed;
}

/* ── DevFS callbacks ───────────────────────────────────────────────── */

static void *aud0_open(void *ctx, int flags) {
    (void)ctx; (void)flags;
    audmgr_session_t *s = session_alloc();
    if (!s) { log_("aud0: open failed — no free sessions"); return NULL; }
    log_("aud0: opened");
    return (void *)s;
}

static int aud0_close(void *ctx) {
    audmgr_session_t *s = (audmgr_session_t *)ctx;
    if (s) session_free(s);
    log_("aud0: closed");
    return 0;
}

static ssize_t aud0_read(void *ctx, void *buf, size_t count) {
    audmgr_session_t *s = (audmgr_session_t *)ctx;
    if (!s || s->resp_len == 0) return 0;
    uint32_t avail = s->resp_len - s->resp_off;
    if ((uint32_t)count < avail) avail = (uint32_t)count;
    memcpy_(buf, s->resp_buf + s->resp_off, avail);
    s->resp_off += avail;
    if (s->resp_off >= s->resp_len) { s->resp_len = 0; s->resp_off = 0; }
    return (ssize_t)avail;
}

static ssize_t aud0_write(void *ctx, const void *buf, size_t count) {
    audmgr_session_t *s = (audmgr_session_t *)ctx;
    if (!s || !buf || count == 0) return 0;

    const uint8_t *src = (const uint8_t *)buf;
    size_t left = count;

    while (left > 0) {
        uint32_t space = sizeof(s->pkt_buf) - s->pkt_len;
        if (space == 0) { s->pkt_len = 0; space = sizeof(s->pkt_buf); }
        uint32_t copy = (uint32_t)(left < space ? left : space);
        memcpy_(s->pkt_buf + s->pkt_len, src, copy);
        s->pkt_len += copy; src += copy; left -= copy;

        while (s->pkt_len >= sizeof(aud_hdr_t)) {
            uint32_t consumed = process_packet(s);
            if (consumed == 0) break;
            uint32_t remaining = s->pkt_len - consumed;
            if (remaining > 0) {
                /* memmove, not memcpy — source and dest overlap when a
                 * partial trailing packet sits right after a consumed one. */
                uint8_t tmp[sizeof(s->pkt_buf)];
                memcpy_(tmp, s->pkt_buf + consumed, remaining);
                memcpy_(s->pkt_buf, tmp, remaining);
            }
            s->pkt_len = remaining;
        }
    }
    return (ssize_t)count;
}

static void *aud0_mmap(void *ctx, void *hint, size_t length, int prot, int flags, uint64_t offset) {
    (void)hint; (void)flags;
    audmgr_session_t *s = (audmgr_session_t *)ctx;
    if (!s || !g_api->devfs_mmap_region) return (void *)-1;

    if (offset == AUDMGR_OFF_RING) {
        if (!s->ring || s->ring_slot_count == 0) return (void *)-1;
        if (s->ring_user_va) return (void *)(uintptr_t)s->ring_user_va;
        size_t sz = (length == 0) ? (size_t)s->ring_slot_count * sizeof(audmgr_ring_slot_t) : length;
        void *uva = g_api->devfs_mmap_region((uint64_t)(uintptr_t)s->ring, sz, prot, 0);
        if (uva != (void *)-1) {
            s->ring_user_va = (uint64_t)(uintptr_t)uva;
            s->ring_is_mapped = 1;
        }
        return uva;
    }

    return (void *)-1;
}

static devfs_device_ops_t g_ops_aud0 = {
    .name = "aud0",
    .open = aud0_open,
    .read = aud0_read,
    .write = aud0_write,
    .close = aud0_close,
    .mmap = aud0_mmap,
    .can_replace = NULL,
};

/* ── Pull hook, called by an OUTPUT controller (e.g. ac97audio's IRQ
 * handler) via the audiomanager service to get frames to play. Runs in
 * whatever context the caller is in — for AC'97 that's IRQ context, so
 * this must not allocate or block. On underrun it silence-fills the
 * remainder rather than leaving stale/garbage samples in buf. ── */

static uint32_t pull_output_frames(void *buf, uint32_t frame_count) {
    int16_t *out = (int16_t *)buf;
    uint32_t written_frames = 0;

    if (!g_playback_session || !g_playback_session->ring) {
        memset_(buf, 0, (size_t)frame_count * 2 * sizeof(int16_t));
        return 0;
    }

    audmgr_session_t *s = g_playback_session;

    while (written_frames < frame_count && s->ring_head != s->ring_write_pos) {
        audmgr_ring_slot_t *slot = &s->ring[s->ring_head];
        uint32_t take = slot->frame_count;
        if (take > frame_count - written_frames) take = frame_count - written_frames;

        memcpy_(out + (size_t)written_frames * 2, slot->samples, (size_t)take * 2 * sizeof(int16_t));
        written_frames += take;

        /* Only advance ring_head once the whole slot has been consumed —
         * partial-slot consumption (take < slot->frame_count) can't
         * happen with equal-sized pulls in practice but this keeps the
         * bookkeeping correct if pull sizes ever change. */
        if (take >= slot->frame_count) {
            s->ring_head = (s->ring_head + 1) % s->ring_slot_count;
        }
    }

    if (written_frames < frame_count) {
        memset_(out + (size_t)written_frames * 2, 0,
               (size_t)(frame_count - written_frames) * 2 * sizeof(int16_t));
    }

    return written_frames;
}

static void push_input_frames(int controllerid, const void *buf, uint32_t frame_count) {
    /* No input-side session routing implemented yet — capture controllers
     * have nowhere to deliver frames to until a session opens aud0 for
     * reading raw PCM. Logged once per call is too noisy for a hot path,
     * so this intentionally does nothing rather than pretending to work. */
    (void)controllerid; (void)buf; (void)frame_count;
}

/* ── Module init ───────────────────────────────────────────────────── */

int sqrm_module_init(const sqrm_kernel_api_t *api) {
    if (!api || !api->devfs_register_path || !api->sqrm_service_register) return -1;
    g_api = api;

    memset_(g_controllers, 0, sizeof(g_controllers));
    memset_(g_sessions, 0, sizeof(g_sessions));
    g_next_controller = 0;
    g_active_output_id = -1;
    g_playback_session = NULL;

    static audio_api_t audio_api;
    audio_api.register_controller = register_controller;
    audio_api.remove_controller = remove_controller;
    audio_api.pull_output_frames = pull_output_frames;
    audio_api.push_input_frames = push_input_frames;
    api->sqrm_service_register("audiomanager", &audio_api, sizeof(audio_api));
    log_("service 'audiomanager' registered");


    if (api->devfs_register_path("audio/aud0", &g_ops_aud0, NULL) != 0) {
        log_("devfs_register_path(audio/aud0) failed");
        return -1;
    }
    log_("$/dev/audio/aud0 registered");

    return 0;
}